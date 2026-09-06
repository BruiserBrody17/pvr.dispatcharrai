#pragma once

// DispatcharrClient talks to a Dispatcharr server's native REST API
// (NOT the Xtream Codes compatibility layer) so that DVR actions taken in
// Kodi map directly onto Dispatcharr's own recording engine.
//
// Confirmed against a live instance's own OpenAPI schema (GET /api/schema/)
// at the time this was written:
//   POST   {base}/api/accounts/token/                -> {access, refresh} JWT
//   POST   {base}/api/accounts/token/refresh/         -> {access}
//   GET    {base}/api/channels/channels/              -> paginated channel list
//   GET    {base}/api/channels/streams/               -> paginated stream list
//   GET    {base}/api/channels/logos/{id}/cache/      -> channel logo image
//   GET    {base}/output/epg                          -> full XMLTV guide document
//   GET    {base}/proxy/ts/stream/{channel_uuid}      -> live MPEG-TS stream
//   GET    {base}/api/channels/recordings/            -> bare array of Recording
//                                                         {id, start_time, end_time,
//                                                         task_id, custom_properties,
//                                                         channel} -- NOT title/
//                                                         subtitle/description/
//                                                         duration/in_progress, see
//                                                         GetRecordings() below
//   POST   {base}/api/channels/recordings/            -> create; body is
//                                                         {channel, start_time, end_time,
//                                                         custom_properties?} -- no
//                                                         title/name field exists
//   DELETE {base}/api/channels/recordings/{id}/       -> delete one recording
//   GET    {base}/api/channels/recordings/{id}/file/  -> recording playback,
//                                                         Range-seekable, redirects to
//                                                         .../hls/index.m3u8 while
//                                                         still recording -- despite
//                                                         its documented security
//                                                         schemes including anonymous
//                                                         access, a real instance
//                                                         returned 403 for both this
//                                                         and the redirect target
//                                                         without an X-API-Key header
//                                                         or Bearer token, confirmed
//                                                         for both an in-progress and
//                                                         a completed recording -- see
//                                                         OpenRecordingStream()
//   GET    {base}/api/channels/series-rules/          -> {"rules": [...]}, NOT a bare
//                                                         array or {results: [...]}
//   POST   {base}/api/channels/series-rules/          -> body is {title, tvg_id?,
//                                                         channel_id?, mode?,
//                                                         title_mode?, ...} -- NOT
//                                                         {channel, title_pattern}
//   DELETE {base}/api/channels/series-rules/?title=&tvg_id=&epg_source_id=
//                                                      -> deletes by query params,
//                                                         NOT /{id}/ -- series rules
//                                                         have no path-addressable id
//   POST   {base}/api/channels/series-rules/evaluate/ -> evaluate series rules
//   POST   {base}/api/accounts/api-keys/generate/     -> {key, user}. Regenerating
//                                                         replaces the previous key
//                                                         (confirmed: calling this
//                                                         twice returns two different
//                                                         keys) -- only call this once
//                                                         per account and cache the
//                                                         result, see GenerateApiKey()
//
// A Recording's custom_properties key names (title/subtitle/description
// nested under "program", plus status/file paths/poster logo) and a
// series-rules list item's shape are both confirmed against real created
// objects too -- see docs/API_NOTES.md for the details.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace dispatcharr
{

struct Config
{
  std::string host;
  int port = 9191;
  bool useHttps = false;
  std::string username;
  std::string password;
  bool verifySsl = true;
  int timeoutSeconds = 30;
  bool debugLogging = false;
  // Long-lived Dispatcharr API key, used only to authenticate recording
  // playback (see OpenRecordingStream()) -- unlike the JWT access token
  // (30-minute lifetime), this doesn't expire on its own. It CAN still go
  // stale, though: Dispatcharr keeps only one active key account-wide, so
  // another Kodi install regenerating its own key silently invalidates
  // this one -- OpenRecordingStream()/ReadRecordingStream() detect that (a
  // 401) and self-heal by regenerating and retrying. Auto-generated and
  // persisted back to the addon's own settings on first use if left empty;
  // see PVRDispatcharr's constructor.
  std::string apiKey;
};

struct Channel
{
  int id = 0;
  std::string uuid; // used to build the live-stream proxy URL
  std::string name;
  int logoId = -1; // -1 means no logo; pass to GetChannelLogoUrl()
  int channelNumber = 0; // also what the XMLTV guide's <channel id="..."> uses, not tvgId
  int groupId = -1;
  std::string groupName;
  std::string tvgId;
  // Catch-up/archive playback, backed by the upstream provider's own
  // archive (Xtream "tv_archive"), not a generic Dispatcharr-side rolling
  // timeshift buffer for every channel -- see docs/API_NOTES.md.
  bool catchupEnabled = false;
  int catchupDays = 0;
};

struct ChannelGroup
{
  int id = 0;
  std::string name;
};

struct Recording
{
  int id = 0;
  // Dispatcharr's Recording object itself carries no title/subtitle/
  // description fields at all (confirmed against its live OpenAPI schema:
  // just id, start_time, end_time, task_id, custom_properties, channel) --
  // these are read out of custom_properties on a best-effort basis, see
  // DispatcharrClient.cpp.
  std::string title;
  std::string subtitle;
  std::string description;
  time_t startTime = 0;
  time_t endTime = 0;
  int durationSeconds = 0;
  int channelId = 0;
  bool isInProgress = false; // startTime <= now < endTime
  bool isUpcoming = false;   // now < startTime (scheduled, not yet started)
  // True while custom_properties._hls_dir is still present -- Dispatcharr
  // sets status away from "recording" (to "stopped") the instant the user
  // stops it, synchronously in the stop endpoint, well before the HLS-to-MKV
  // concat that happens afterward in the background recording task actually
  // finishes (confirmed in tasks.py: the stop endpoint's own comment says so
  // outright, and _hls_dir is only ever popped from custom_properties after
  // the directory is actually removed, post-concat, post-viewer-wait). So
  // isInProgress alone going false does NOT mean a stable, complete file
  // exists yet to open for byte-range playback -- confirmed live: opening a
  // just-stopped recording as a completed one errored outright (the file
  // didn't exist yet) or played without seeking (the file existed but was
  // still being actively written by the concat, an unstable Content-Length
  // this addon's completed-recording path was never designed to handle).
  // OpenRecordedStream() uses this, independent of isInProgress, to decide
  // whether to keep using the growing-buffer HLS reader -- which handles a
  // frozen (no-longer-growing) manifest correctly already, since
  // RefreshInProgressRecordingManifest() ties "finished" to isInProgress,
  // not to whether the underlying ffmpeg process happens to still be alive.
  // Deliberately NOT folded into isInProgress itself: that field also drives
  // Kodi's timer-state UI (PVR_TIMER_STATE_RECORDING), which must keep
  // reflecting Dispatcharr's real status, not this file-readiness detail.
  bool hlsDirStillPresent = false;
  // Non-zero when this Recording was materialized by Dispatcharr's own
  // recurring-rule scheduler (custom_properties.rule.{type:"recurring",id})
  // rather than created directly -- see RecurringRule below. Used to link
  // this occurrence back to its parent rule as a Kodi PVR_TIMER child
  // (PVRTimer::SetParentClientIndex()).
  int recurringRuleId = 0;
};

// A scheduled recording: either a one-off (isSeries == false) or a
// standing series rule (isSeries == true) evaluated by Dispatcharr itself.
struct TimerRule
{
  int id = 0;
  int channelId = 0;
  std::string tvgId;
  std::string title;
  std::string titlePattern; // series rules only
  time_t startTime = 0;
  time_t endTime = 0;
  bool isSeries = false;
  bool recordNewOnly = false; // Dispatcharr's mode == "new" vs "all"
};

// A recurring day-of-week rule, backed by Dispatcharr's own
// RecurringRecordingRule model/scheduler (confirmed against its live
// source and API: GET/POST /api/channels/recurring-rules/, an hourly
// Celery task materializing real Recording rows up to 14 days ahead,
// each tagged custom_properties.rule.{type:"recurring",id} -- see
// GetRecurringRules()/CreateRecurringRule() below). Distinct from
// TimerRule (series rules): this one has a real numeric id, and its
// schedule is a fixed weekly time-of-day pattern rather than
// EPG-title matching.
struct RecurringRule
{
  int id = 0;
  int channelId = 0;
  std::string name;
  // 0=Monday .. 6=Sunday (confirmed against Dispatcharr's own source:
  // RecurringRecordingRuleSerializer's validation error text and
  // sync_recurring_rule_impl's date.weekday() comparison both agree on
  // this convention) -- conveniently the same convention Kodi's own
  // PVR_WEEKDAY_MONDAY=(1<<0)..PVR_WEEKDAY_SUNDAY=(1<<6) bitmask uses, so
  // converting between the two is a plain 1 << day, no reordering.
  std::vector<int> daysOfWeek;
  // Seconds since midnight, in Dispatcharr's own configured system
  // timezone (NOT UTC -- confirmed via source that Dispatcharr's
  // recurring-rule scheduler combines these naive values with its system
  // timezone CoreSetting, with no server-side conversion; see
  // recurring_rule_utc_offset_minutes in settings.xml for how this addon
  // bridges that against Kodi's UTC-based timer times).
  int startTimeOfDaySeconds = 0;
  int endTimeOfDaySeconds = 0;
  time_t startDate = 0; // UTC midnight of Dispatcharr's local start_date
  time_t endDate = 0;   // likewise, end_date
  bool enabled = true;
};

// One comskip-detected commercial break (or other marker), as returned by
// the recording_edl companion plugin's get_edl action -- see
// GetRecordingEdl()'s own comment.
struct RecordingEdlEntry
{
  int64_t startMs = 0;
  int64_t endMs = 0;
  int type = 3; // matches Kodi's own PVR_EDL_TYPE_COMBREAK; see plugin.py
};

// Thin, synchronous REST client. Callers (PVRDispatcharr) are responsible
// for running these off Kodi's calling thread where the PVR API allows it;
// none of the calls here touch Kodi's own API.
class DispatcharrClient
{
public:
  explicit DispatcharrClient(Config config);
  ~DispatcharrClient();

  // Logs in with username/password and stores the JWT pair. Safe to call
  // repeatedly; it is a no-op if a still-valid token is already held.
  bool EnsureAuthenticated(std::string& error);

  bool GetChannels(std::vector<Channel>& out, std::string& error);
  bool GetChannelGroups(std::vector<ChannelGroup>& out, std::string& error);

  // Full-body caller must fetch and parse this with XmlTvParser; this
  // client only returns the raw document.
  bool GetXmlTvGuide(std::string& xmlOut, std::string& error);

  // Plain live-stream URL passthrough for live_timeshift_mode's "Off"
  // setting -- no server-side buffering, no admin-account requirement.
  std::string GetLiveStreamUrl(const Channel& channel) const;
  // logoId is a Logo object's own id (Channel::logoId), not the channel's id.
  std::string GetChannelLogoUrl(int logoId) const;

  // Creates a catch-up (archived-programme) playback session via
  // POST /api/catchup/sessions/ and returns a fully-qualified, session-bound
  // URL that plays and seeks without needing any further auth for the life
  // of the session (a 10-minute *sliding* idle window, refreshed by each
  // range/seek request -- i.e. it doesn't expire mid-playback the way a
  // short-lived JWT embedded directly in the URL would). Only meaningful
  // for a channel with Channel::catchupEnabled set; programmeStart must be
  // the EPG entry's own start time, not when the viewer pressed play.
  bool CreateCatchupSession(const std::string& channelUuid,
                            time_t programmeStart,
                            int durationMinutes,
                            std::string& playbackUrlOut,
                            std::string& error);

  // Starts (or, if already running, confirms) a server-side rolling live
  // buffer for a channel via this addon's companion Dispatcharr plugin
  // (dispatcharr-plugin/timeshift_buffer/ in this repo -- not built into
  // Dispatcharr itself, must be installed and enabled separately). Returns
  // a fully-qualified URL to the buffer's rolling HLS playlist, built from
  // this client's own configured host plus the port/path the plugin
  // reports back for its own file server -- deliberately always http://
  // regardless of the use_https setting, since the plugin's minimal file
  // server has no TLS of its own and isn't assumed to sit behind whatever
  // reverse proxy/TLS termination the main API port might (see
  // docs/API_NOTES.md for the limitation this implies if your setup splits
  // those differently). Requires the Dispatcharr account this addon is
  // configured with to be an admin account -- confirmed against
  // Dispatcharr's own source (apps/accounts/permissions.py) that the
  // plugin run endpoint requires IsAdmin (user_level >= 10) for POST, not
  // just any authenticated user.
  bool StartTimeshiftBuffer(const std::string& channelUuid,
                            std::string& playlistUrlOut,
                            std::string& error);
  // Tells the plugin this specific viewer (m_liveTimeshiftStream.viewerId)
  // is done with the channel's buffer -- NOT an unconditional stop. The
  // plugin reference-counts viewers per buffer (registered by
  // StartTimeshiftBuffer()'s own viewer_id param) and only actually stops
  // the underlying ffmpeg process once the *last* registered viewer leaves;
  // if others are still registered, this just deregisters the caller and
  // the buffer keeps running for them. Only called from
  // CloseLiveTimeshiftStream(), synchronously -- see its own comment for
  // why a detached background thread here (an earlier version of this
  // fix) was itself a real bug: it let a fast channel switch (Kodi's own
  // back-to-back Close-then-Open) race the actual teardown, so a
  // provider's own concurrent-stream limit could still be fully consumed
  // by the channel just switched away from at the exact moment the new
  // channel's own Open() asked for a slot. See docs/TIMESHIFT.md's
  // "Concurrent viewers" section for the full account, including the
  // original bug this reference-counted design itself fixes (a buffer
  // staying exhausted well after its only real viewer stopped, since
  // nothing used to proactively tell the plugin so). Best-effort:
  // "nothing was running" is success, not an error.
  bool StopTimeshiftBuffer(const std::string& channelUuid, const std::string& viewerId,
                           std::string& error);

  bool GetRecordings(std::vector<Recording>& out, std::string& error);
  // Fetches comskip-detected commercial-break markers for a completed
  // recording via this addon's companion Dispatcharr plugin
  // (dispatcharr-plugin/recording_edl/ in this repo -- not built into
  // Dispatcharr itself, must be installed and enabled separately, same
  // admin-account requirement as StartTimeshiftBuffer()'s own plugin).
  // Confirmed against Dispatcharr's own source that there is no other way
  // to reach this data over HTTP at all: the /file/ endpoint always
  // serves exactly custom_properties.file_path with no way to redirect it
  // at a sibling .edl file, and no generic static route reaches
  // /data/recordings/... either -- see the plugin's own README for the
  // full investigation. Returns true with an empty `out` (not an error)
  // when the recording simply has no markers -- comskip never ran, found
  // nothing, or ran in "cut" mode (which deletes the .edl file once it's
  // done physically removing the commercials, so there's nothing left to
  // report) -- since that's the normal outcome for most recordings, not a
  // failure. Only returns false for a genuine call failure (plugin not
  // installed/enabled, wrong account, network error).
  bool GetRecordingEdl(int recordingId, std::vector<RecordingEdlEntry>& out, std::string& error);
  bool DeleteRecording(int recordingId, std::string& error);
  // Confirmed against the live schema: POST .../recordings/{id}/stop/
  // "Stop[s] a recording early while retaining the partial content for
  // playback" -- distinct from DeleteRecording(), which removes the file
  // entirely. Kodi's "Stop Recording" action (and "Delete" on a timer
  // it knows is still recording) both call this addon's DeleteTimer()
  // with forceDelete=true specifically to mean "this is still recording"
  // (confirmed against Kodi's own source, xbmc/pvr/timers/PVRTimers.cpp);
  // see PVRDispatcharr::DeleteTimer() for why that maps to this call, not
  // DeleteRecording().
  bool StopRecording(int recordingId, std::string& error);

  // True if Config::apiKey is already set. Callers use this to decide
  // whether GenerateApiKey() is worth calling at all. Deliberately reads
  // m_config.apiKey directly rather than through GetApiKey() -- safe
  // unlocked only because its one real call site (PVRDispatcharr's
  // constructor) runs before any thread that could concurrently call
  // GenerateApiKey() exists yet; do not add a second call site without
  // reconsidering that.
  bool HasApiKey() const { return !m_config.apiKey.empty(); }
  // A currently-valid JWT access token, for the real-time-updates
  // WebSocket connection (see PVRDispatcharr's realtime-update thread) --
  // logs in/refreshes first via EnsureAuthenticated() if needed.
  // Dispatcharr's own WebSocket auth middleware only checks the token
  // once, at connect time (confirmed by reading its JWTAuthMiddleware
  // source), so an already-open connection keeps working past the
  // token's own 30-minute expiry; a fresh one is only needed when
  // (re)connecting.
  bool GetAccessToken(std::string& tokenOut, std::string& error);
  // Current API key, e.g. to re-persist it after OpenRecordingStream()/
  // ReadRecordingStream() have silently regenerated a stale one (see their
  // comments below) -- this client has no knowledge of Kodi's settings
  // storage, so the caller must notice the change and save it itself.
  // Thread-safe (m_apiKeyMutex) -- GenerateApiKey() can be triggered by a
  // self-healing regenerate-on-401 from any of this client's several
  // stream-opening call sites, on whichever Kodi/background thread
  // happens to be using them, concurrently with another thread reading
  // the key for its own request.
  std::string GetApiKey() const
  {
    std::lock_guard<std::mutex> lock(m_apiKeyMutex);
    return m_config.apiKey;
  }
  // Generates a new Dispatcharr API key and stores it in this client's own
  // config for immediate use by GetRecordingStreamUrl(). Regenerating
  // replaces any previous key for the account (confirmed against a live
  // instance) -- Dispatcharr keeps only one active key account-wide, so
  // running this addon against the same account from more than one Kodi
  // install means whichever one last called this silently invalidates
  // every other install's stored key. OpenRecordingStream()/
  // ReadRecordingStream() call this automatically on a 401 to self-heal
  // from that; call it directly only when HasApiKey() is false (e.g. first
  // run), and persist the result so a restart doesn't invalidate a key
  // some other install is actively relying on.
  bool GenerateApiKey(std::string& keyOut, std::string& error);

  bool GetTimerRules(std::vector<TimerRule>& out, std::string& error);
  // title is used only as a client-side placeholder (see GetRecordings()'s
  // pending-title cache) -- not sent to Dispatcharr itself; see the .cpp for
  // why.
  bool CreateOneTimeRecording(int channelId,
                               time_t start,
                               time_t end,
                               const std::string& title,
                               std::string& error);
  // Reschedules an existing one-time recording's start/end time via
  // PATCH /api/channels/recordings/{id}/. Deliberately sends ONLY
  // start_time/end_time, mirroring CreateOneTimeRecording()'s own choice
  // not to touch custom_properties -- confirmed two things live against a
  // real EPG-matched recording before relying on either: (1) a PATCH
  // that omits both times crashes with an uncaught 500 (Dispatcharr's own
  // RecordingSerializer.validate() does `end_time < now` with end_time
  // still None on a bare partial update -- a real server-side bug, not
  // something this addon can prevent except by never sending that shape
  // of request), so both fields are always included, never a bare
  // partial; (2) resending the *same* start/end time an EPG-matched
  // recording already had did NOT drift them via repeated pre/post-offset
  // reapplication, despite that being a real risk suggested by
  // validate()'s own source (it re-derives the offset-adjusted times
  // whenever custom_properties.program is a dict and both times are
  // present) -- confirmed live, not just theorized either way, so this
  // method sends exactly the new times without trying to work around a
  // compounding-offset bug that didn't actually reproduce.
  bool UpdateOneTimeRecording(int recordingId, time_t start, time_t end, std::string& error);
  // recordNewOnly maps to Dispatcharr's SeriesRuleRequest.mode ("new" vs
  // the server default "all") -- confirmed against the live schema: "all"
  // records every matching episode including reruns, "new" only
  // first-run ones.
  bool CreateSeriesRule(int channelId,
                        const std::string& tvgId,
                        const std::string& titlePattern,
                        bool recordNewOnly,
                        std::string& error);
  // Series rules have no numeric id in Dispatcharr's API at all -- they're
  // deleted by DELETE /api/channels/series-rules/?title=...&tvg_id=...
  // (confirmed against the live OpenAPI schema), not by path id.
  bool DeleteSeriesRule(const std::string& title, const std::string& tvgId, std::string& error);

  bool GetRecurringRules(std::vector<RecurringRule>& out, std::string& error);
  // daysOfWeek: 0=Monday..6=Sunday (see RecurringRule's own comment).
  // startTimeOfDaySeconds/endTimeOfDaySeconds: seconds since midnight in
  // Dispatcharr's own configured system timezone, already offset-adjusted
  // by the caller (PVRDispatcharr::AddTimer()) using the
  // recurring_rule_utc_offset_minutes setting -- this method sends them
  // through as plain "HH:MM:SS" with no further conversion. startDate/
  // endDate: UTC time_t values (midnight); only the calendar date portion
  // is sent, as Dispatcharr's own start_date/end_date DateField expects.
  // Dispatcharr's serializer requires both start_date and end_date on
  // create despite the model declaring them nullable (confirmed against
  // its live validation code) -- endDate should already reflect the
  // caller's chosen "how far out" default (see AddTimer()), not left at 0.
  bool CreateRecurringRule(int channelId,
                           const std::string& name,
                           const std::vector<int>& daysOfWeek,
                           int startTimeOfDaySeconds,
                           int endTimeOfDaySeconds,
                           time_t startDate,
                           time_t endDate,
                           std::string& error);
  // Edits an existing recurring rule -- also how Kodi's own "enable/
  // disable" timer action reaches this rule type
  // (PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE), since Dispatcharr has no
  // separate enable/disable endpoint, just this same field on the rule
  // itself. Deliberately a PARTIAL PATCH that omits end_date -- confirmed
  // against RecurringRecordingRuleSerializer's own source that a missing
  // field falls back to the existing instance's value rather than
  // failing validation (unlike RecordingSerializer's create-oriented
  // validate(), this one was written partial-update-safe), so the rule's
  // existing end_date (set once at creation, see CreateRecurringRule()'s
  // own comment on why that's a somewhat arbitrary "far enough out"
  // value) is preserved automatically rather than needing to be
  // re-fetched and resent on every edit.
  bool UpdateRecurringRule(int ruleId,
                           int channelId,
                           const std::string& name,
                           const std::vector<int>& daysOfWeek,
                           int startTimeOfDaySeconds,
                           int endTimeOfDaySeconds,
                           time_t startDate,
                           bool enabled,
                           std::string& error);
  bool DeleteRecurringRule(int ruleId, std::string& error);
  // Extends an existing recurring rule's end_date forward -- a partial
  // PATCH sending only that one field, the same partial-update-safe
  // pattern UpdateRecurringRule() relies on. Called periodically by
  // PVRDispatcharr's background renewal (see its own comment) to keep a
  // "permanent" recurring rule's materialized-occurrence window topped up,
  // now that CreateRecurringRule() itself sets a much shorter initial
  // end_date than it originally did -- confirmed live that Dispatcharr
  // eagerly materializes every occurrence between start_date and end_date
  // synchronously on create/update, not a lazy rolling window as this
  // addon's own docs used to (incorrectly) assume, so a single far-future
  // end_date isn't "cheap" the way it was once thought to be; periodic
  // small extensions here are what actually keeps that cost bounded.
  // Confirmed live, separately: an update like this one, that changes
  // end_date, only regenerates *future* (not yet started) occurrences --
  // an already in-progress or completed one survives untouched, same id,
  // same file, `started_at` unchanged -- see docs/RECURRING_RULES.md. The
  // caller (PVRDispatcharr::RenewRecurringRules()) still skips calling
  // this at all for a rule with an occurrence currently recording or about
  // to start soon, as defense in depth rather than relying solely on that
  // server-side scoping.
  bool ExtendRecurringRuleEndDate(int ruleId, time_t newEndDate, std::string& error);

  // Dispatcharr's global recording pre/post padding, in minutes
  // (custom_properties has no per-recording/per-rule equivalent -- this
  // is genuinely global-only, confirmed against Dispatcharr's own
  // source). Only actually applied server-side to EPG-based scheduling
  // (series rules, an EPG-matched one-time recording) -- confirmed a
  // recurring (day-of-week) rule's own scheduler never reads or applies
  // this at all, a real inconsistency on Dispatcharr's side this addon
  // can't fix, just report accurately.
  bool GetDvrOffsetMinutes(int& preMinutesOut, int& postMinutesOut, std::string& error);
  // Read-modify-write: Dispatcharr stores this alongside several other,
  // unrelated settings (comskip mode/hw-accel, recording path templates)
  // in the same single JSON blob (CoreSettings key "dvr_settings") --
  // confirmed live that this is one shared row, not a dedicated one for
  // just padding. Fetches the current blob first and only changes the
  // two offset keys within it, so a naive whole-field overwrite doesn't
  // silently wipe out the unrelated settings sharing that same row.
  bool SetDvrOffsetMinutes(int preMinutes, int postMinutes, std::string& error);

  // Dispatcharr's own configured system timezone (CoreSettings key
  // "system_settings", field "time_zone"), as a raw IANA zone name (e.g.
  // "America/Chicago") -- confirmed live against a real instance. Surfaced
  // as read-only info next to recurring_rule_utc_offset_minutes so the
  // user has a concrete reference for what numeric offset to enter there,
  // without this addon needing to bundle a real timezone database just to
  // compute that offset itself (see docs/RECURRING_RULES.md for why that
  // was deliberately ruled out).
  bool GetSystemTimeZone(std::string& timeZoneOut, std::string& error);

  // Auto-computes the current UTC offset (minutes) for the small set of
  // well-known IANA zones (US/Canada, UK/EU) hardcoded in
  // DispatcharrClient.cpp, using their real, stable DST transition rules
  // -- see docs/RECURRING_RULES.md for why a full timezone database isn't
  // bundled to do this for every possible zone instead. Returns false
  // (offsetMinutesOut untouched) for any zone not in that short list, in
  // which case recurring_rule_utc_offset_minutes still needs to be set
  // manually. Pure computation, no network/instance state needed --
  // static so PVRDispatcharr's constructor can call it directly.
  // `nowUtc` is a parameter purely for testability; real callers should
  // always pass the actual current time.
  static bool ComputeKnownZoneOffsetMinutes(const std::string& ianaZoneName, time_t nowUtc,
                                             int& offsetMinutesOut);

  // Raw byte-range recording playback, called through the addon's
  // OpenRecordedStream/ReadRecordedStream/SeekRecordedStream/
  // LengthRecordedStream. Kodi's kodi-dev-kit docs describe
  // PVR_STREAM_PROPERTY_STREAMURL as a fallback used only when an addon
  // doesn't implement these -- but confirmed against a real failure (a live
  // kodi.log showed Kodi's generic CCurlFile hitting a populated STREAMURL
  // directly, bypassing these entirely, including the 401-retry logic
  // below) that populating STREAMURL anyway is NOT harmless once these are
  // implemented: Kodi will happily use it instead, silently skipping this
  // code path. GetRecordingStreamProperties() deliberately leaves STREAMURL
  // unset for a completed recording for that reason. Only supports a
  // completed recording (a real, Range-seekable file) -- an in-progress one
  // is instead served via OpenInProgressRecordingStream() below.
  bool OpenRecordingStream(int recordingId, std::string& error);
  int ReadRecordingStream(uint8_t* buffer, unsigned int size);
  int64_t SeekRecordingStream(int64_t position, int whence);
  int64_t GetRecordingStreamLength() const;
  void CloseRecordingStream();

  // Growing, seekable byte-stream access to the server-side live timeshift
  // buffer -- the actual consumer of StartTimeshiftBuffer() above. Exposes
  // the buffer to Kodi via OpenLiveStream/ReadLiveStream/SeekLiveStream
  // (PVRCapabilities::SetHandlesInputStream), the same "one growing/
  // seekable byte source, Kodi's own internal demuxer does the actual
  // MPEG-TS parsing and PTS-based seek refinement" pattern already proven
  // for completed recordings (OpenRecordingStream/ReadRecordingStream/
  // SeekRecordingStream above), just against the companion plugin's
  // per-segment Range-served files instead of one Dispatcharr-served
  // recording file -- confirmed live: real pause/rewind/fast-forward/
  // live-follow on a real channel, including a 95-second rewind spanning
  // several manifest refreshes. This replaced an earlier STREAMURL +
  // inputstream.ffmpegdirect approach that routed through ffmpegdirect's
  // own generic HLS seek instead of Kodi's native demuxer -- confirmed
  // broken 100% of the time regardless of direction or position (see
  // docs/TIMESHIFT.md for that investigation). Calls StartTimeshiftBuffer()
  // itself first to ensure a buffer is actually running for this channel
  // (same as the plain-Play path already does).
  bool OpenLiveTimeshiftStream(const std::string& channelUuid, std::string& error);
  int ReadLiveTimeshiftStream(uint8_t* buffer, unsigned int size);
  int64_t SeekLiveTimeshiftStream(int64_t position, int whence);
  int64_t GetLiveTimeshiftStreamLength();
  // Duration of the buffer currently known to be available, in milliseconds
  // -- for PVRDispatcharr::GetStreamTimes()'s ptsEnd, which must grow as the
  // live buffer does (see kodi-dev-kit's own PVRStreamTimes doc comment:
  // "For Live TV, this must be ... point to end of the timeshift buffer").
  int64_t GetLiveTimeshiftStreamDurationMs();
  void CloseLiveTimeshiftStream();
  // Genuine "is a live-timeshift stream currently open" state, as opposed to
  // just "is server-side timeshift mode enabled in settings" -- the latter
  // doesn't change once a stream closes, so PVRDispatcharr's GetStreamTimes()/
  // CanPauseStream()/CanSeekStream()/IsRealTimeStream() need this to avoid
  // misreporting live-timeshift state while an in-progress recording (or a
  // plain completed recording) is what's actually open. See
  // IsInProgressRecordingStreamOpen()'s own comment for why these callbacks
  // need per-stream-flavour state at all.
  bool IsLiveTimeshiftStreamOpen() const;

  // Growing, seekable byte-stream access to an in-progress recording --
  // the same "expose a growing HLS source as one fixed-origin byte
  // address space, let Kodi's own native demuxer handle MPEG-TS parsing
  // and seek refinement" pattern as OpenLiveTimeshiftStream() above,
  // applied to a recording instead of a live channel. Replaced an earlier
  // STREAMURL + inputstream.ffmpegdirect approach (see git history /
  // docs/RECORDINGS.md) that needed a "play from start (seek)"
  // vs. "play live (follow, no seek)" toggle -- a limitation of routing
  // through libavformat's own HLS demuxer, which won't offer seeking
  // without a #EXT-X-ENDLIST-terminated (i.e. static, no-longer-growing)
  // playlist. CInputStreamPVRRecording extends the same
  // CInputStreamPVRBase as CInputStreamPVRChannel (confirmed in Kodi-core
  // source), so GetStreamTimes()/CanPauseStream()/CanSeekStream()/
  // IsRealTimeStream() apply identically here -- a recording reported
  // through those the same way the live buffer is gets real seek and
  // live-follow simultaneously, no toggle needed. Unlike live-timeshift,
  // there's no server-side buffer to start/stop: Dispatcharr's own DVR
  // task keeps writing the recording regardless of whether this addon is
  // reading it, so opening always starts at true byte 0, matching normal
  // recording/VOD conventions (and the existing completed-recording
  // behaviour).
  bool OpenInProgressRecordingStream(int recordingId, std::string& error);
  int ReadInProgressRecordingStream(uint8_t* buffer, unsigned int size);
  int64_t SeekInProgressRecordingStream(int64_t position, int whence);
  int64_t GetInProgressRecordingStreamLength();
  // Mirrors GetLiveTimeshiftStreamDurationMs() -- for GetStreamTimes()'s
  // ptsEnd, which must grow as the recording does.
  int64_t GetInProgressRecordingStreamDurationMs();
  void CloseInProgressRecordingStream();
  // PVRDispatcharr uses this to tell which of OpenRecordedStream()'s two
  // implementations (this one, or the plain completed-recording one) is
  // the one currently open, since ReadRecordedStream()/SeekRecordedStream()/
  // LengthRecordedStream()/GetStreamTimes()/CanPauseStream()/CanSeekStream()/
  // IsRealTimeStream() are all shared Kodi PVR client callbacks with no
  // parameter telling them which recording-stream flavour is active.
  bool IsInProgressRecordingStreamOpen() const;

private:
  std::string BaseUrl() const;
  bool Login(std::string& error);
  bool RefreshAccessToken(std::string& error);

  // Finds the one CoreSettings row with the given key and returns its id
  // and full value blob (unmodified) -- GetDvrOffsetMinutes()/
  // SetDvrOffsetMinutes() (key kDvrSettingsKey) and GetSystemTimeZone()
  // (key kSystemSettingsKey) all need this same lookup, just against
  // different rows of the same /api/core/settings/ list.
  bool FindCoreSettingsRow(const std::string& key, int& idOut, nlohmann::json& valueOut,
                            std::string& error);

  // Fetches the raw HLS playlist text for an in-progress recording, with a
  // self-healing retry on a 401. Returns false (with `error` set) on a
  // genuine network/HTTP failure. Called from
  // RefreshInProgressRecordingManifest().
  bool FetchRawInProgressPlaylist(int recordingId, const std::string& playlistUrl,
                                   std::string& playlistText, std::string& error);

  // The CURLSH* behind m_curlShareState, or nullptr if it failed to
  // initialise -- pass to CURLOPT_SHARE on every easy handle this client
  // creates (Request(), OpenRecordingStream()'s probe, ReadRecordingStream()'s
  // persistent handle) so they all pull from one connection/DNS/TLS-session
  // cache. Returns void* (actually CURLSH*) so this header doesn't need
  // <curl/curl.h>; defined in the .cpp, which does.
  void* GetCurlShare() const;

  // A second, separate CURLSH for ProbeSegmentByteSize()'s concurrent probe
  // burst only -- shares DNS/TLS-session but deliberately not connections,
  // to sidestep a real macOS-libcurl connection-cache crash under that
  // specific concurrency pattern. See m_probeCurlShareState's own comment.
  void* GetProbeCurlShare() const;

  // A tiny ranged GET with the current API key attached, used by
  // RefreshInProgressRecordingManifest() as a proactive self-heal check
  // (cheaper to catch a stale key here than mid-read of an actual
  // segment). Returns true on anything but a 401 (a transport error or
  // other status isn't this check's problem to solve -- fail open rather
  // than block playback on a check that was only ever a best-effort head
  // start on a problem the caller can't fully prevent anyway).
  bool IsApiKeyValidFor(const std::string& url) const;

  // Performs one HTTP call. `body` is sent as the JSON request body for
  // POST/PATCH/DELETE-with-body; pass an empty object for bodyless calls.
  // On success, parses the response into `responseOut` (may be left null
  // for 204 No Content) and returns true.
  bool Request(const std::string& method,
               const std::string& path,
               const nlohmann::json& body,
               nlohmann::json& responseOut,
               std::string& error,
               bool withAuth = true,
               int retryOnAuthFailure = 1);

  // Confirmed live: a freshly-(re)started buffer's playlist URL can be
  // unreachable for a real moment after CallTimeshiftPluginAction()
  // returns it -- ffmpeg needs time to connect, probe, and write its first
  // segment/playlist, and the plugin's own response comes back as soon as
  // it's been launched, not once it's produced anything. Best-effort poll
  // (a few seconds, small sleeps between tiny GETs against the playlist
  // URL itself, no auth needed) so the common case doesn't race this;
  // returns false rather than blocking indefinitely if it times out, but
  // callers proceed with the URL regardless either way.
  bool WaitForTimeshiftPlaylistReady(const std::string& playlistUrl);

  // Calls the timeshift_buffer plugin's run/ endpoint for `action` and
  // unwraps a {status, http_port, playlist_route} response shape. Only
  // StartTimeshiftBuffer() uses this now (SnapshotTimeshiftBuffer(), the
  // other original caller, was removed once server-side timeshift stopped
  // using STREAMURL+ffmpegdirect -- see docs/TIMESHIFT.md). Left as its own
  // function rather than folded into StartTimeshiftBuffer() since
  // RefreshLiveManifest() below is the same kind of "POST an action, unwrap
  // the envelope" call against a differently-shaped response, so the split
  // still documents the shared pattern even with one caller of this exact
  // signature.
  bool CallTimeshiftPluginAction(const std::string& action,
                                 const std::string& channelUuid,
                                 std::string& playlistUrlOut,
                                 std::string& error,
                                 const nlohmann::json& extraParams);

  Config m_config;
  // Guards only m_config.apiKey -- every other Config field is set once in
  // the constructor (from LoadConfigFromSettings()) and never written
  // again, so reading them elsewhere needs no synchronization; apiKey
  // alone can be rewritten later, at any time, by GenerateApiKey() (see
  // its own comment on why -- a self-healing regenerate-on-401, callable
  // from several different stream-opening code paths on whichever thread
  // happens to be using them). Mutable so the several const read sites
  // (GetApiKey(), IsApiKeyValidFor()) can still lock it.
  mutable std::mutex m_apiKeyMutex;
  // Recursive: Login()/RefreshAccessToken() each hold this for their own
  // full duration (including the nested Request() call that actually
  // performs the HTTP round-trip) so every write to the token fields
  // below is serialized regardless of caller -- EnsureAuthenticated()
  // already held this across calling them, but Request()'s own 401-retry
  // path (see Request()'s definition) calls them directly, with no lock
  // of its own, and Request() also reads m_accessToken to build the auth
  // header. A plain mutex would deadlock the moment any of these nest on
  // the same thread (EnsureAuthenticated -> Login -> Request all doing
  // so already); recursive_mutex allows that same-thread re-entry while
  // still serializing genuinely concurrent callers on different threads
  // -- confirmed necessary, not just theoretical, once the channel/EPG
  // refresh thread and realtime-updates thread joined Kodi's own
  // PVR-calling threads as concurrent callers into this client.
  std::recursive_mutex m_authMutex;
  std::string m_accessToken;
  std::string m_refreshToken;
  std::chrono::steady_clock::time_point m_accessTokenExpiry;
  // Local IP curl reports (CURLINFO_LOCAL_IP) for the most recent
  // successful Request() -- i.e. the interface this machine actually
  // reaches Dispatcharr through. OpenLiveTimeshiftStream() passes this to
  // start_buffer so the timeshift plugin's ffmpeg connection (which
  // otherwise looks like it comes from Dispatcharr's own container, since
  // it runs server-side) can be attributed to the real viewing device via
  // an X-Forwarded-For-style header instead of showing 127.0.0.1.
  std::mutex m_lastLocalIpMutex;
  std::string m_lastLocalIp;

  // Opaque pointer to a small heap-allocated struct (CurlShareState, defined
  // in the .cpp) holding a CURLSH* and the mutexes that guard it. Every
  // curl_easy_init() this client does (Request(), the recording-stream
  // helpers) is still a fresh easy handle per call/open recording -- unlike
  // ReadRecordingStream's single reused CURL*, a lone shared easy handle
  // isn't safe here, since Kodi's PVR API can call into this client from
  // multiple threads at once (see the class comment above). A CURLSH share
  // object is libcurl's own answer to exactly that: a connection/DNS/
  // TLS-session cache safely shared across separate, concurrently-used easy
  // handles, as long as the application supplies lock/unlock callbacks
  // (libcurl doesn't lock it internally) -- see GetCurlShare() and the
  // constructor/destructor. Found necessary by a companion session's real
  // measurements: a single "Record" press fires several Request() calls in
  // a row (create, then a timer/recordings refresh), and on WiFi each one
  // independently exposed to a fresh-connection latency spike produced a
  // visible (1.8s-10s observed) delay before Kodi's own "recording started"
  // notification appeared, versus ~20ms/call under calm conditions.
  void* m_curlShareState = nullptr;

  // Second CURLSH, used only by ProbeSegmentByteSize() when called from
  // RefreshInProgressRecordingManifest()'s concurrent probe fan-out (up to
  // 16 threads at once, all against the same host). Confirmed live on
  // macOS 26.6.2 (real system libcurl, arm64e -- crash report
  // Kodi-2026-09-06-135331.ips) that sharing CURL_LOCK_DATA_CONNECT across
  // that specific burst crashes inside that libcurl build's own
  // connection-cache return/close path, not this addon's lock/unlock
  // callbacks (which were already correctly handling the *other* kind of
  // concurrent access this client always allowed -- occasional background-
  // thread calls alongside active playback -- crash-free through extensive
  // live testing). Shares DNS and TLS-session (far more mature in
  // libcurl's share interface than connection sharing, and still a real
  // win for a same-host burst, especially TLS handshake avoidance over
  // HTTPS) but never touches the connection cache at all, sidestepping the
  // crash mechanism entirely rather than working around one specific
  // libcurl build/version.
  void* m_probeCurlShareState = nullptr;

  // Kodi only ever has one recording open for playback at a time.
  struct RecordingStreamState
  {
    bool open = false;
    std::string url; // final URL after following any redirect
    int64_t length = -1;
    int64_t position = 0;
    // Persistent libcurl easy handle, reused across every ReadRecordingStream()
    // call for the current open recording so HTTP keep-alive actually applies
    // across sequential range reads -- a fresh curl_easy_init()/cleanup() per
    // read meant a brand-new TCP connection (and TLS handshake, over HTTPS)
    // for every single demuxer read, which is negligible on a low-latency LAN
    // but confirmed (via a companion session's WiFi measurements: 68.5 MB/s
    // over one connection vs. 1.13 MB/s doing 64KB reads with a fresh
    // connection each, both against the same host) to starve playback on a
    // higher-latency/jittery link even with plenty of raw bandwidth for the
    // recording's bitrate. Created lazily on the first read, cleaned up in
    // CloseRecordingStream(). Stored as void* rather than CURL* so this
    // header doesn't need <curl/curl.h>; CURL is itself just an opaque alias
    // for void in curl.h, so the cast back in the .cpp is exact.
    void* curl = nullptr;
  };
  RecordingStreamState m_recordingStream;

  struct LiveTimeshiftSegmentInfo
  {
    std::string filename;
    int64_t sequence = 0;   // HLS media-sequence-derived, stable across refetches
    int64_t byteOffset = 0; // in this stream's own fixed-origin address space
    int64_t byteSize = 0;
    int64_t timeOffsetMs = 0; // ditto, fixed-origin
  };

  // Only one live-timeshift stream open at a time, same as recordings.
  struct LiveTimeshiftStreamState
  {
    bool open = false;
    std::string channelUuid;
    // Generated fresh by OpenLiveTimeshiftStream() and sent as start_buffer's
    // viewer_id -- lets the plugin reference-count viewers of a shared
    // buffer (registers on start, deregisters on the matching
    // StopTimeshiftBuffer() at Close), so it can tell whether *this* viewer
    // was the last one before actually tearing anything down, instead of
    // either killing a buffer other viewers still need (the original
    // concurrent-viewer bug) or never proactively tearing one down at all
    // (which starves a provider's concurrent-stream limit -- see
    // docs/TIMESHIFT.md's "Concurrent viewers" section for both).
    std::string viewerId;
    std::string segmentBaseUrl; // "http://host:port/<uuid>/" -- filename appended per-request
    // Ordered by sequence, append-only for the life of this open stream --
    // byteOffset/timeOffsetMs are this stream's OWN fixed-origin addressing,
    // deliberately NOT the plugin response's own (relative-to-that-fetch)
    // offsets: the plugin's rolling window means "byte 0" in a fresh fetch
    // shifts to newer content over time, which would silently invalidate
    // any position already handed to Kodi's demuxer. See
    // RefreshLiveManifest()'s merge logic and get_live_manifest's own
    // docstring in plugin.py for why sequence is the stable join key.
    std::vector<LiveTimeshiftSegmentInfo> segments;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
    int64_t position = 0;
    std::chrono::steady_clock::time_point lastManifestFetch{};
    // Set by SeekLiveTimeshiftStream() on every call. ReadLiveTimeshiftStream()
    // uses this to tell "this read is likely one of ffmpeg's own internal
    // seek probes" apart from "normal sequential playback that's caught up
    // to live" when deciding how long to wait for the tail to grow -- see
    // its own comment for why that distinction matters.
    std::chrono::steady_clock::time_point lastSeekTime{};
    // Position where a short (likely-probe) catch-up wait last gave up, so
    // a later read landing at that exact same position -- meaning it's not
    // a fresh probe candidate anymore, ffmpeg is genuinely stuck waiting
    // there -- escalates to the full segment-duration budget instead of
    // repeating the short one indefinitely. See ReadLiveTimeshiftStream()'s
    // own comment.
    int64_t lastShortGiveUpPosition = -1;
    void* curl = nullptr; // persistent handle, same rationale as RecordingStreamState::curl
  };
  LiveTimeshiftStreamState m_liveTimeshiftStream;

  // Fetches the plugin's get_live_manifest action and merges any segments
  // not already known into m_liveTimeshiftStream, extending its fixed-origin
  // address space -- called on open, and again whenever a read/seek/length
  // call needs to know about content newer than what's already known. Not
  // merely a cache refresh: an unconditional replace would shift byte 0 out
  // from under a position already handed to Kodi's demuxer. `force` bypasses
  // the small throttle (see the .cpp) that keeps a tight demux-read loop
  // from re-fetching the manifest on every single call. `fatalOut`, if
  // non-null, is set true when the plugin reports the buffer will never
  // succeed (ffmpeg already exited -- see plugin.py's own
  // BufferFailedError) rather than just "not ready yet" -- only
  // OpenLiveTimeshiftStream()'s cold-start retry loop passes one, to stop
  // retrying immediately instead of waiting out its full budget against
  // something that can't recover on its own.
  bool RefreshLiveManifest(bool force, std::string& error, bool* fatalOut = nullptr);

  struct InProgressRecordingSegmentInfo
  {
    std::string url; // absolute, already resolved against the playlist's own baseDir
    int64_t byteOffset = 0; // in this stream's own fixed-origin address space
    int64_t byteSize = 0;
    int64_t timeOffsetMs = 0; // ditto, fixed-origin
  };

  // Only one in-progress-recording stream open at a time, same as
  // completed recordings and the live-timeshift buffer.
  struct InProgressRecordingStreamState
  {
    bool open = false;
    int recordingId = -1;
    // Append-only for the life of this open stream: Dispatcharr's own HLS
    // output for a recording, unlike the live-timeshift plugin's rolling
    // buffer, never evicts old segments (a recording is meant to be kept
    // in full), so there's no rolling-window/sequence-number complication
    // to handle here -- "already have N segments, only look at any past
    // that" is enough.
    std::vector<InProgressRecordingSegmentInfo> segments;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
    int64_t position = 0;
    // Set once Dispatcharr reports this recording as no longer in
    // progress (checked on every manifest refresh) -- lets
    // ReadInProgressRecordingStream() treat "caught up to the known tail"
    // as genuine EOF instead of polling for more that will never come.
    bool finished = false;
    std::chrono::steady_clock::time_point lastManifestFetch{};
    // Same seek-probe-vs-real-catch-up distinction as
    // LiveTimeshiftStreamState -- see ReadLiveTimeshiftStream()'s comment
    // for why this matters; the same generic Kodi/ffmpeg seek-probing
    // behaviour applies here too, since this uses the same native-demuxer
    // mechanism.
    std::chrono::steady_clock::time_point lastSeekTime{};
    int64_t lastShortGiveUpPosition = -1;
    void* curl = nullptr; // persistent handle, same rationale as RecordingStreamState::curl
    // Whole-segment cache for ReadInProgressRecordingStream() -- required,
    // not just an optimisation: unlike the completed-recording `/file/`
    // endpoint, Dispatcharr's in-progress-recording HLS segment endpoint
    // ignores the Range header entirely and always serves the full segment
    // body from its own byte 0 regardless of what was requested (confirmed
    // live -- see ProbeSegmentByteSize()'s own comment for the same finding
    // against a HEAD/ranged-GET probe). A per-read ranged GET against that
    // endpoint would therefore silently hand back the segment's own leading
    // bytes on every read past the first, corrupting the reconstructed
    // stream from the second read of each segment onward -- confirmed live
    // by a companion session as continuous H.264 decode errors and growing
    // audio desync from the very start of playback. Fetching each segment's
    // full body exactly once and serving every read against it from memory
    // sidesteps the server's lack of Range support entirely, the same way
    // the HEAD-based size probe does for sizing. cachedSegmentByteOffset is
    // that segment's own byteOffset (its address in this stream's
    // fixed-origin space), -1 when nothing is cached; only ever holds the
    // one segment current reads are landing in, evicted (replaced) the
    // moment position moves to a different segment.
    std::vector<uint8_t> cachedSegmentBytes;
    int64_t cachedSegmentByteOffset = -1;
  };
  InProgressRecordingStreamState m_inProgressRecordingStream;

  // Cross-open cache of already-probed segments for an in-progress
  // recording, keyed by recordingId. Dispatcharr's in-progress HLS output
  // is append-only (see InProgressRecordingStreamState::segments' own
  // comment) -- a segment probed on one open is still valid, at the same
  // byte offset, on the next -- so without this, reopening the same
  // still-recording (e.g. after a channel switch, or resuming after
  // pausing playback in the Kodi UI) would otherwise re-probe every single
  // already-known segment from scratch every time -- on top of the cost
  // every *first* open already pays for a recording that's been running a
  // while, which was confirmed live (a ~2h-in recording took 29.4s to
  // open, entirely spent probing 1,842 already-elapsed segments one at a
  // time). Cleared for a recording once RefreshInProgressRecordingManifest()
  // sees it's finished -- a finished recording is played back through the
  // completed-recording path instead, so its entry here would just sit
  // unused.
  struct InProgressRecordingSegmentCache
  {
    std::vector<InProgressRecordingSegmentInfo> segments;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
  };
  std::map<int, InProgressRecordingSegmentCache> m_inProgressSegmentCache;
  std::mutex m_inProgressSegmentCacheMutex;

  // Fetches the recording's current HLS playlist (FetchRawInProgressPlaylist)
  // and merges any segments not already known into
  // m_inProgressRecordingStream, extending its fixed-origin address space,
  // probing each newly-discovered segment's byte size with a tiny HEAD
  // request (HLS playlists carry durations via #EXTINF, never byte sizes) --
  // same pattern as RefreshLiveManifest(), adapted for a plain HLS text
  // response instead of the timeshift plugin's own JSON manifest action.
  // `force` bypasses the small throttle that keeps a tight demux-read loop
  // from re-fetching on every single call.
  bool RefreshInProgressRecordingManifest(bool force, std::string& error);

  // A tiny ranged GET (mirrors OpenRecordingStream()'s own probe) to learn
  // one segment's total byte size -- HLS playlists carry each segment's
  // duration (#EXTINF) but never its size. Returns -1 on any failure
  // (network error, non-2xx/206, or no parseable Content-Range); the
  // caller skips a segment it can't size rather than corrupting the
  // cumulative offsets that follow.
  int64_t ProbeSegmentByteSize(const std::string& segmentUrl) const;

  // Client-side placeholder for a just-created one-time recording's title,
  // matched by channelId (not also start time -- see below) to whatever
  // this addon was called with in CreateOneTimeRecording(). Dispatcharr
  // only learns a recording's real title asynchronously (custom_properties.
  // program.title, populated a moment after the recording actually starts,
  // see GetRecordings()), but Kodi already told AddTimer() the correct
  // EPG-derived title *before* this client ever calls Dispatcharr --
  // CreateFromEpg() reads it from the EPG tag the user clicked "Record" on.
  // Caching that and using it in GetRecordings() in place of the
  // "Recording <id>" fallback means the correct title shows immediately,
  // without needing to wait for Dispatcharr's enrichment or a later refresh
  // to catch up at all, for the common EPG-matched case.
  // Deliberately NOT also matched on start time: confirmed against a real
  // recording of an already-airing EPG event that Dispatcharr silently
  // clamps the stored start_time to the moment it actually began recording
  // (e.g. "now"), not the EPG programme's own start time this addon sent --
  // exact-time matching missed every such case, which is the single most
  // common one ("Record" on something currently on). Matching by channel
  // alone (picking the most recently inserted match, and consuming it so it
  // isn't reused for a later recording on the same channel) is good enough
  // for what this is: a short-lived, best-effort bridge, not an
  // authoritative mapping. Entries expire after a few minutes regardless
  // (pruned in GetRecordings()) since Dispatcharr's own enrichment should
  // have long since caught up by then, and to avoid an unbounded cache.
  struct PendingTitle
  {
    int channelId = 0;
    std::string title;
    std::chrono::steady_clock::time_point insertedAt;
  };
  std::mutex m_pendingTitlesMutex;
  std::vector<PendingTitle> m_pendingTitles;
};

} // namespace dispatcharr
