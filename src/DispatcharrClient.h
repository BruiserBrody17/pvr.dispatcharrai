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
  // Optional delay before handing Kodi a new channel's stream URL. Added
  // while diagnosing a channel-switching failure that turned out to be an
  // unrelated IPv6/DNS issue (see docs/API_NOTES.md) -- a delay didn't fix
  // that, so this defaults to off. Left available in case a genuinely
  // different Dispatcharr/provider setup needs a moment between switches.
  int channelSwitchDelaySeconds = 0;
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

  void UpdateConfig(Config config);

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
  // is instead served via GetInProgressRecordingStreamUrl() below, routed
  // through inputstream.ffmpegdirect (opt-in), not through here at all.
  bool OpenRecordingStream(int recordingId, std::string& error);
  int ReadRecordingStream(uint8_t* buffer, unsigned int size);
  int64_t SeekRecordingStream(int64_t position, int whence);
  int64_t GetRecordingStreamLength() const;
  void CloseRecordingStream();

  // Builds the pipe-delimited HLS URL (with an X-API-Key header suffix,
  // Kodi's "|key=value" stream URL syntax) for an in-progress recording, to
  // be routed through inputstream.ffmpegdirect rather than
  // OpenRecordingStream()/ReadRecordingStream() above. A recording still
  // being written is served as a growing HLS playlist (confirmed against a
  // live instance: {base}/api/channels/recordings/{id}/hls/index.m3u8,
  // segments named seg_NNNNN.ts), not a single Range-seekable file, and
  // each segment independently requires the same X-API-Key header --
  // confirmed that query-param auth is NOT accepted as an alternative
  // (both ?api_key= and ?X-API-Key= got 403; only the real header works),
  // and Kodi's own native (non-ffmpeg) HLS handling has no mechanism to
  // attach a custom header to the segment requests it discovers by parsing
  // the playlist itself. ffmpegdirect's plain pass-through mode (no
  // stream_mode set) instead delegates the whole thing to ffmpeg's own HLS
  // demuxer, which does propagate custom headers to every segment fetch,
  // not just the manifest -- confirmed by reading its source
  // (FFmpegStream::OpenWithFFmpeg -> GetFFMpegOptionsFromInput()) -- PROVIDED
  // inputstream.ffmpegdirect.open_mode is explicitly forced to "ffmpeg": a
  // plain http(s) URL otherwise defaults to its OpenWithCURL() path
  // instead, which sets no header options at all when opening the format
  // context and would silently reproduce this exact failure. Also confirmed
  // (both by reading GetFFMpegOptionsFromInput()'s source and by an actual
  // failed attempt logging "ignoring header option 'X-API-Key'"): it only
  // forwards a fixed allowlist of standard HTTP header names as real
  // headers; anything else -- X-API-Key included -- needs a literal "!"
  // prefix on the option name, which it strips before using the rest as the
  // header name, hence "!X-API-Key" below rather than "X-API-Key". See
  // GetRecordingStreamProperties() in PVRDispatcharr.cpp for the rest of
  // the properties this needs alongside the URL. The URL is constructed
  // directly here (not resolved via a live redirect probe the way
  // OpenRecordingStream() does) since the caller has already independently
  // confirmed in-progress status via GetRecordings() -- the same condition
  // Dispatcharr's own /file/ endpoint uses to decide whether to redirect
  // here at all.
  //
  // Not const: unlike OpenRecordingStream()/ReadRecordingStream(), there's
  // no callback into this addon if the key turns out to be stale -- the URL
  // (with the key already baked in) is handed to inputstream.ffmpegdirect
  // once, upfront, with no way for it to ask this addon to regenerate and
  // retry the way this addon's own HTTP client does on a 401. Confirmed via
  // real multi-install testing that the shared account-wide key (see
  // GenerateApiKey()'s comment) can already be stale by the time this is
  // called, so this checks it live first and regenerates before baking in
  // a key already known to be invalid, rather than baking it in blind and
  // letting ffmpegdirect fail outright with no recovery.
  std::string GetInProgressRecordingStreamUrl(int recordingId);

private:
  std::string BaseUrl() const;
  bool Login(std::string& error);
  bool RefreshAccessToken(std::string& error);

  // The CURLSH* behind m_curlShareState, or nullptr if it failed to
  // initialise -- pass to CURLOPT_SHARE on every easy handle this client
  // creates (Request(), OpenRecordingStream()'s probe, ReadRecordingStream()'s
  // persistent handle) so they all pull from one connection/DNS/TLS-session
  // cache. Returns void* (actually CURLSH*) so this header doesn't need
  // <curl/curl.h>; defined in the .cpp, which does.
  void* GetCurlShare() const;

  // A tiny ranged GET (mirrors OpenRecordingStream()'s own probe -- a HEAD
  // request isn't confirmed to behave the same on this endpoint) with the
  // current API key attached, used only to check the key is still live
  // before GetInProgressRecordingStreamUrl() bakes it into a URL that can't
  // self-heal later. Returns true on anything but a 401 (a transport error
  // or other status isn't this check's problem to solve -- fail open rather
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

  Config m_config;
  std::mutex m_authMutex;
  std::string m_accessToken;
  std::string m_refreshToken;
  std::chrono::steady_clock::time_point m_accessTokenExpiry;

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
