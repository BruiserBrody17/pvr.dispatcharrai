#pragma once

// DispatcharrClient talks to a Dispatcharr server's native REST API
// (NOT the Xtream Codes compatibility layer) so that DVR actions taken in
// Kodi map directly onto Dispatcharr's own recording engine.
//
// Confirmed against Dispatcharr's public GitHub issues/release notes and
// docs at the time this was written:
//   POST {base}/api/accounts/token/            -> {access, refresh} JWT
//   POST {base}/api/accounts/token/refresh/     -> {access}
//   GET  {base}/api/channels/channels/          -> paginated channel list
//   GET  {base}/api/channels/streams/           -> paginated stream list
//   GET  {base}/api/channels/logos/{id}/cache/  -> channel logo image
//   GET  {base}/output/epg                      -> full XMLTV guide document
//   GET  {base}/proxy/ts/stream/{channel_uuid}  -> live MPEG-TS stream
//   GET  {base}/api/channels/recordings/{id}/file/  -> recording playback
//                                                       (Range-seekable, no auth)
//   POST {base}/api/channels/series-rules/evaluate/ -> evaluate series rules
//
// NOT independently confirmed against a live Swagger document (Dispatcharr's
// schema has changed across recent releases) and should be checked against
// http://<your-server>:9191/swagger/ before relying on them in production:
//   - Exact create/list/delete routes and payload shape under
//     /api/channels/recordings/ and /api/channels/series-rules/
//   - Whether pagination wraps list responses in {results: [...]} (typical
//     Django REST Framework default, assumed here) or returns a bare array
//
// See docs/API_NOTES.md for how to verify/update these against your instance.

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
  // Delay before handing Kodi a new channel's stream URL, to give
  // Dispatcharr's proxy (or the upstream provider) time to release the
  // previous channel's connection before the next one is requested. See
  // docs/API_NOTES.md's channel-switching notes for why this exists.
  int channelSwitchDelaySeconds = 2;
};

struct Channel
{
  int id = 0;
  std::string uuid; // used to build the live-stream proxy URL
  std::string name;
  int logoId = -1; // -1 means no logo; pass to GetChannelLogoUrl()
  int channelNumber = 0;
  int groupId = -1;
  std::string groupName;
  std::string tvgId; // links this channel to <channel id="..."> in XMLTV
};

struct ChannelGroup
{
  int id = 0;
  std::string name;
};

struct Recording
{
  int id = 0;
  std::string title;
  std::string subtitle;
  std::string description;
  time_t startTime = 0;
  int durationSeconds = 0;
  int channelId = 0;
  bool isInProgress = false;
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
  bool DeleteTimerRule(int ruleId, bool isSeries, std::string& error);

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
