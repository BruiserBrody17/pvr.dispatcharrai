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
  // Stops and discards the channel's currently-running server-side buffer
  // (the plugin's own stop_buffer action -- kills ffmpeg, removes its
  // segment files). OpenLiveTimeshiftStream() calls this before every
  // StartTimeshiftBuffer() so each Play starts a genuinely fresh buffer
  // rather than reattaching to whatever's still running from a previous
  // session -- see its own comment for why. Best-effort: "nothing was
  // running" is success, not an error, since there's nothing to prefer over
  // starting fresh either way.
  bool StopTimeshiftBuffer(const std::string& channelUuid, std::string& error);

  bool GetRecordings(std::vector<Recording>& out, std::string& error);
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
  // whether GenerateApiKey() is worth calling at all.
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
  std::string GetApiKey() const { return m_config.apiKey; }
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
  std::mutex m_authMutex;
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
  // from re-fetching the manifest on every single call.
  bool RefreshLiveManifest(bool force, std::string& error);

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
