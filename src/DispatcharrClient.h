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
//   GET    {base}/api/channels/recordings/{id}/file/  -> recording playback
//                                                         (Range-seekable, no auth)
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
//
// NOT independently confirmed (the schema shows no example values and the
// account used to develop this lacked permission to create a live
// recording/series rule to inspect one): the exact key names inside a
// Recording's custom_properties, and the exact shape of an item inside a
// series-rules list response. See docs/API_NOTES.md.

#include <chrono>
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
};

// Thin, synchronous REST client. Callers (PVRDispatcharr) are responsible
// for running these off Kodi's calling thread where the PVR API allows it;
// none of the calls here touch Kodi's own API.
class DispatcharrClient
{
public:
  explicit DispatcharrClient(Config config);

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
  std::string GetRecordingStreamUrl(int recordingId) const;
  bool DeleteRecording(int recordingId, std::string& error);

  bool GetTimerRules(std::vector<TimerRule>& out, std::string& error);
  bool CreateOneTimeRecording(int channelId,
                               time_t start,
                               time_t end,
                               const std::string& title,
                               std::string& error);
  bool CreateSeriesRule(int channelId,
                        const std::string& tvgId,
                        const std::string& titlePattern,
                        std::string& error);
  // Series rules have no numeric id in Dispatcharr's API at all -- they're
  // deleted by DELETE /api/channels/series-rules/?title=...&tvg_id=...
  // (confirmed against the live OpenAPI schema), not by path id.
  bool DeleteSeriesRule(const std::string& title, const std::string& tvgId, std::string& error);

private:
  std::string BaseUrl() const;
  bool Login(std::string& error);
  bool RefreshAccessToken(std::string& error);

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
};

} // namespace dispatcharr
