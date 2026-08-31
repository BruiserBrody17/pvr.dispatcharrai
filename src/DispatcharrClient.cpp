#include "DispatcharrClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

using json = nlohmann::json;

namespace dispatcharr
{

namespace
{

// ---------------------------------------------------------------------
// NOTE ON ASSUMED ENDPOINTS/FIELDS
// Paths marked "assumed" below follow standard Django REST Framework
// ModelViewSet conventions (matching the confirmed /api/channels/channels/
// and /api/channels/streams/ routes) but were not individually confirmed
// against a live Dispatcharr Swagger document. Check them against
// http://<your-server>:9191/swagger/ and adjust here if they've drifted.
// ---------------------------------------------------------------------
constexpr const char* kTokenPath = "/api/accounts/token/";
constexpr const char* kTokenRefreshPath = "/api/accounts/token/refresh/";
constexpr const char* kChannelsPath = "/api/channels/channels/";
// Confirmed against a live instance's own OpenAPI schema (GET /api/schema/).
// The old primary guess, /api/channels/channel-groups/, doesn't exist at all
// -- Dispatcharr's SPA catches unmatched routes and serves index.html for it,
// which returned HTTP 200 but wasn't JSON, so it always failed to parse.
constexpr const char* kChannelGroupsPath = "/api/channels/groups/";
constexpr const char* kChannelGroupsPathFallback = "/api/channels/channel-groups/"; // pre-confirmation guess, kept as a fallback in case older Dispatcharr versions differ
constexpr const char* kEpgOutputPath = "/output/epg";
// Both confirmed against a live instance's own OpenAPI schema -- see the
// endpoint/payload notes in DispatcharrClient.h.
constexpr const char* kRecordingsPath = "/api/channels/recordings/";
constexpr const char* kSeriesRulesPath = "/api/channels/series-rules/";
constexpr const char* kLogosPath = "/api/channels/logos/";
// Confirmed against a live instance: creates a session-bound catch-up
// (archived programme) playback URL that stays valid via a sliding idle
// window for as long as it's actively used, rather than embedding a
// short-lived JWT directly in the stream URL.
constexpr const char* kCatchupSessionsPath = "/api/catchup/sessions/";
constexpr const char* kApiKeyGeneratePath = "/api/accounts/api-keys/generate/";

// Writes into a fixed-size caller-owned buffer, capping at its capacity --
// used for recording stream reads, where the caller (Kodi's demuxer) owns
// the destination buffer. A Range request should never actually return
// more than requested, so hitting the cap would indicate a confused
// server response rather than a normal condition.
struct FixedBufferSink
{
  uint8_t* buffer;
  unsigned int capacity;
  unsigned int written = 0;
};

size_t FixedBufferWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* sink = static_cast<FixedBufferSink*>(userdata);
  size_t totalBytes = size * nmemb;
  size_t remaining = sink->capacity - sink->written;
  size_t toCopy = std::min(totalBytes, remaining);
  if (toCopy > 0)
  {
    std::memcpy(sink->buffer + sink->written, ptr, toCopy);
    sink->written += static_cast<unsigned int>(toCopy);
  }
  return totalBytes;
}

// Captures the total resource size from a "Content-Range: bytes X-Y/TOTAL"
// response header -- the only reliable way to learn a ranged request's
// full size, since Content-Length on a 206 response reflects only the
// requested slice.
size_t RecordingHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* totalOut = static_cast<int64_t*>(userdata);
  size_t len = size * nitems;
  std::string line(buffer, len);
  std::string prefix = line.size() >= 14 ? line.substr(0, 14) : std::string();
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (prefix == "content-range:")
  {
    size_t slash = line.rfind('/');
    if (slash != std::string::npos)
    {
      try
      {
        *totalOut = std::stoll(line.substr(slash + 1));
      }
      catch (const std::exception&)
      {
      }
    }
  }
  return len;
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string IsoFromTime(time_t t)
{
  char buf[32];
  struct tm tmVal{};
#if defined(_WIN32)
  gmtime_s(&tmVal, &t);
#else
  gmtime_r(&t, &tmVal);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmVal);
  return std::string(buf);
}

// Portable timegm(): interprets a struct tm as UTC and returns a time_t,
// without touching the process-wide TZ setting (unlike mktime()).
time_t PortableTimeGm(struct tm* tmVal)
{
#if defined(_WIN32)
  return _mkgmtime(tmVal);
#else
  return timegm(tmVal);
#endif
}

// Parses the "YYYY-MM-DDTHH:MM:SS" prefix of a Dispatcharr date-time field
// (e.g. "2026-08-30T10:55:01Z" or "...+00:00"); any trailing fractional
// seconds/offset is ignored, consistent with every timestamp elsewhere in
// this API being UTC-normalized already (see IsoFromTime() above).
time_t TimeFromIso(const std::string& isoStr)
{
  if (isoStr.size() < 19)
    return 0;

  struct tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(isoStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(isoStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(isoStr.substr(8, 2));
    tmVal.tm_hour = std::stoi(isoStr.substr(11, 2));
    tmVal.tm_min = std::stoi(isoStr.substr(14, 2));
    tmVal.tm_sec = std::stoi(isoStr.substr(17, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }
  return PortableTimeGm(&tmVal);
}

// URL-encodes a query parameter value (titles/tvg_ids can contain spaces,
// '&', etc.). curl_easy_escape needs a handle but doesn't use it for
// anything beyond the escape itself, so a scratch one is fine here.
std::string UrlEncode(const std::string& value)
{
  CURL* curl = curl_easy_init();
  if (!curl)
    return value;
  char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  std::string result = escaped ? escaped : value;
  if (escaped)
    curl_free(escaped);
  curl_easy_cleanup(curl);
  return result;
}

// nlohmann::json's item.value(key, default) only substitutes the default
// when the key is *absent* -- if the key is present but explicitly JSON
// null (which Django REST Framework serializers commonly emit for empty
// nullable fields, e.g. a channel with no logo), converting it to a
// non-null-supporting type like int throws an uncaught type_error that
// aborts whatever's parsing the response (confirmed against a real
// instance: ~0.4% of a 9360-channel list had an explicit `"logo_id": null`).
// This wraps every field read so a null or wrong-typed value degrades to
// the default instead of throwing.
template <typename T>
T FieldOr(const json& item, const char* key, T defaultValue)
{
  if (!item.contains(key) || item[key].is_null())
    return defaultValue;
  try
  {
    return item[key].template get<T>();
  }
  catch (const json::exception&)
  {
    return defaultValue;
  }
}

// Backs DispatcharrClient::m_curlShareState. A CURLSH connection/DNS/
// TLS-session cache shared across every easy handle this client creates,
// so short-lived per-call handles (Request(), the recording-stream probe)
// still get keep-alive/connection reuse -- unlike ReadRecordingStream's
// single reused CURL*, a lone shared easy handle isn't safe here since
// Kodi's PVR API can call into this client from multiple threads at once.
// libcurl doesn't lock a share object internally; these mutexes back the
// lock/unlock callbacks below, which is the standard, documented pattern
// for using one across threads (CURLSHOPT_LOCKFUNC/UNLOCKFUNC).
constexpr int kCurlShareLockCount = 8; // headroom above CURL_LOCK_DATA_LAST

struct CurlShareState
{
  CURLSH* handle = nullptr;
  std::array<std::mutex, kCurlShareLockCount> locks;
};

void CurlShareLock(CURL*, curl_lock_data data, curl_lock_access, void* userptr)
{
  auto* state = static_cast<CurlShareState*>(userptr);
  int index = static_cast<int>(data);
  if (index >= 0 && index < kCurlShareLockCount)
    state->locks[index].lock();
}

void CurlShareUnlock(CURL*, curl_lock_data data, void* userptr)
{
  auto* state = static_cast<CurlShareState*>(userptr);
  int index = static_cast<int>(data);
  if (index >= 0 && index < kCurlShareLockCount)
    state->locks[index].unlock();
}

} // namespace

DispatcharrClient::DispatcharrClient(Config config) : m_config(std::move(config))
{
  auto* state = new CurlShareState();
  state->handle = curl_share_init();
  if (state->handle)
  {
    curl_share_setopt(state->handle, CURLSHOPT_LOCKFUNC, CurlShareLock);
    curl_share_setopt(state->handle, CURLSHOPT_UNLOCKFUNC, CurlShareUnlock);
    curl_share_setopt(state->handle, CURLSHOPT_USERDATA, state);
    curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  }
  m_curlShareState = state;
}

DispatcharrClient::~DispatcharrClient()
{
  auto* state = static_cast<CurlShareState*>(m_curlShareState);
  if (state)
  {
    if (state->handle)
      curl_share_cleanup(state->handle);
    delete state;
  }
}

void* DispatcharrClient::GetCurlShare() const
{
  auto* state = static_cast<CurlShareState*>(m_curlShareState);
  return state ? state->handle : nullptr;
}

void DispatcharrClient::UpdateConfig(Config config)
{
  std::lock_guard<std::mutex> lock(m_authMutex);
  m_config = std::move(config);
  m_accessToken.clear();
  m_refreshToken.clear();
}

std::string DispatcharrClient::BaseUrl() const
{
  std::string scheme = m_config.useHttps ? "https://" : "http://";
  return scheme + m_config.host + ":" + std::to_string(m_config.port);
}

bool DispatcharrClient::Request(const std::string& method,
                                 const std::string& path,
                                 const json& body,
                                 json& responseOut,
                                 std::string& error,
                                 bool withAuth,
                                 int retryOnAuthFailure)
{
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }

  std::string url = BaseUrl() + path;
  std::string responseBody;
  std::string bodyStr = body.is_null() ? std::string() : body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  std::string authHeader;
  if (withAuth && !m_accessToken.empty())
  {
    authHeader = "Authorization: Bearer " + m_accessToken;
    headers = curl_slist_append(headers, authHeader.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

  if (method == "POST")
  {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
  }
  else if (method == "PATCH" || method == "DELETE" || method == "PUT")
  {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!bodyStr.empty())
    {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
    }
  }
  // else GET: nothing extra to set

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK)
  {
    error = std::string("HTTP request failed: ") + curl_easy_strerror(res);
    return false;
  }

  if (httpCode == 401 && withAuth && retryOnAuthFailure > 0)
  {
    std::string refreshError;
    if (RefreshAccessToken(refreshError) || Login(refreshError))
      return Request(method, path, body, responseOut, error, withAuth, retryOnAuthFailure - 1);
    error = "Authentication failed: " + refreshError;
    return false;
  }

  if (httpCode < 200 || httpCode >= 300)
  {
    error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + ": " + responseBody;
    return false;
  }

  if (responseBody.empty())
  {
    responseOut = json::object();
    return true;
  }

  try
  {
    responseOut = json::parse(responseBody);
  }
  catch (const json::parse_error& e)
  {
    error = std::string("Failed to parse JSON response: ") + e.what();
    return false;
  }

  return true;
}

bool DispatcharrClient::Login(std::string& error)
{
  json body = {{"username", m_config.username}, {"password", m_config.password}};
  json response;
  if (!Request("POST", kTokenPath, body, response, error, /*withAuth=*/false, /*retry=*/0))
    return false;

  if (!response.contains("access"))
  {
    error = "Login response did not contain an access token";
    return false;
  }

  m_accessToken = FieldOr<std::string>(response, "access", "");
  m_refreshToken = FieldOr<std::string>(response, "refresh", m_refreshToken);
  // SimpleJWT's default access-token lifetime is short (often 5 minutes);
  // we don't decode the JWT to read its real `exp`, we just re-authenticate
  // reactively on the next 401 (see Request()). This timestamp is kept only
  // as an optimisation hint, not a hard guarantee.
  m_accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(4);
  return true;
}

bool DispatcharrClient::RefreshAccessToken(std::string& error)
{
  if (m_refreshToken.empty())
  {
    error = "No refresh token available";
    return false;
  }
  json body = {{"refresh", m_refreshToken}};
  json response;
  if (!Request("POST", kTokenRefreshPath, body, response, error, /*withAuth=*/false, /*retry=*/0))
    return false;

  if (!response.contains("access"))
  {
    error = "Refresh response did not contain an access token";
    return false;
  }
  m_accessToken = FieldOr<std::string>(response, "access", "");
  m_accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(4);
  return true;
}

bool DispatcharrClient::EnsureAuthenticated(std::string& error)
{
  std::lock_guard<std::mutex> lock(m_authMutex);
  if (!m_accessToken.empty() && std::chrono::steady_clock::now() < m_accessTokenExpiry)
    return true;
  if (!m_refreshToken.empty() && RefreshAccessToken(error))
    return true;
  return Login(error);
}

bool DispatcharrClient::GetChannels(std::vector<Channel>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kChannelsPath, json(), response, error))
    return false;

  // Django REST Framework's default pagination wraps results in
  // {count, next, previous, results:[...]}; handle both that and a bare
  // array in case pagination is disabled on this endpoint.
  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected /api/channels/channels/ response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
  {
    Channel ch;
    ch.id = FieldOr(item, "id", 0);
    ch.uuid = FieldOr<std::string>(item, "uuid", "");
    ch.name = FieldOr<std::string>(item, "name", "");
    ch.channelNumber = FieldOr(item, "channel_number", FieldOr(item, "channel_num", 0));
    // Channels only carry a logo_id (an FK to a separate Logo object);
    // there's no logo_url field directly on the channel (that belongs to
    // the underlying Stream model). Resolve the actual image via
    // GetChannelLogoUrl(logoId), not a direct URL field. logo_id is
    // explicitly null (not absent) for channels with no logo.
    ch.logoId = FieldOr(item, "logo_id", -1);
    // Channel group may be a nested object or a bare id depending on the
    // serializer; handle both.
    if (item.contains("channel_group") && item["channel_group"].is_object())
    {
      ch.groupId = FieldOr(item["channel_group"], "id", -1);
      ch.groupName = FieldOr<std::string>(item["channel_group"], "name", "");
    }
    else
    {
      ch.groupId = FieldOr(item, "channel_group", FieldOr(item, "channel_group_id", -1));
    }
    // EPG linkage: try a nested epg_data object first, then a flat field.
    if (item.contains("epg_data") && item["epg_data"].is_object())
      ch.tvgId = FieldOr<std::string>(item["epg_data"], "tvg_id", "");
    else
      ch.tvgId = FieldOr<std::string>(item, "tvg_id", "");

    ch.catchupEnabled = FieldOr(item, "is_catchup", false);
    ch.catchupDays = FieldOr(item, "catchup_days", 0);

    out.push_back(std::move(ch));
  }
  return true;
}

bool DispatcharrClient::GetChannelGroups(std::vector<ChannelGroup>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Neither path was independently confirmed against a live Swagger doc
  // (see docs/API_NOTES.md); try the primary one and fall back to the
  // alternate on failure rather than guessing wrong and going silent.
  json response;
  if (!Request("GET", kChannelGroupsPath, json(), response, error))
  {
    std::string fallbackError;
    if (!Request("GET", kChannelGroupsPathFallback, json(), response, fallbackError))
    {
      error += " / " + fallbackError;
      return false;
    }
  }

  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected channel-groups response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
  {
    ChannelGroup g;
    g.id = FieldOr(item, "id", 0);
    g.name = FieldOr<std::string>(item, "name", "");
    out.push_back(std::move(g));
  }
  return true;
}

bool DispatcharrClient::GetXmlTvGuide(std::string& xmlOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // /output/epg returns raw XML, not JSON -- issue Request() manually via
  // a small local curl call instead of the JSON-oriented Request() helper.
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }
  std::string url = BaseUrl() + kEpgOutputPath;
  xmlOut.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xmlOut);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds * 4));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));
  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK)
  {
    error = std::string("Failed to fetch XMLTV guide: ") + curl_easy_strerror(res);
    return false;
  }
  if (httpCode < 200 || httpCode >= 300)
  {
    error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " for /output/epg";
    return false;
  }
  return true;
}

std::string DispatcharrClient::GetLiveStreamUrl(const Channel& channel) const
{
  // `|Connection=close` is a real Kodi/CCurlFile URL option (see
  // xbmc/filesystem/CurlFile.cpp's protocol option handling) that adds a
  // literal `Connection: close` request header. This was tried while
  // diagnosing a "channel N+1 never plays" failure, on the theory that Kodi
  // was reusing/pooling a still-closing connection to the same host -- it
  // didn't fix that (the real cause turned out to be an unreachable IPv6
  // route to the host, see docs/API_NOTES.md), but it's harmless to leave
  // in place and does prevent connection reuse in general.
  return BaseUrl() + "/proxy/ts/stream/" + channel.uuid + "|Connection=close";
}

std::string DispatcharrClient::GetChannelLogoUrl(int logoId) const
{
  return BaseUrl() + kLogosPath + std::to_string(logoId) + "/cache/";
}

bool DispatcharrClient::CreateCatchupSession(const std::string& channelUuid,
                                             time_t programmeStart,
                                             int durationMinutes,
                                             std::string& playbackUrlOut,
                                             std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json body = {
      {"channel_uuid", channelUuid},
      {"start", IsoFromTime(programmeStart)},
  };
  if (durationMinutes > 0)
    body["duration"] = std::min(durationMinutes, 480); // API-enforced max

  json response;
  if (!Request("POST", kCatchupSessionsPath, body, response, error))
    return false;

  std::string playbackUrl = FieldOr<std::string>(response, "playback_url", "");
  if (playbackUrl.empty())
  {
    error = "Catch-up session response did not contain a playback_url";
    return false;
  }

  // Confirmed against a live instance: playback_url is a relative path
  // (e.g. "/proxy/catchup/{uuid}?session_id=..."), not a full URL.
  if (playbackUrl.rfind("http://", 0) != 0 && playbackUrl.rfind("https://", 0) != 0)
    playbackUrl = BaseUrl() + playbackUrl;

  playbackUrlOut = std::move(playbackUrl);
  return true;
}

bool DispatcharrClient::GetRecordings(std::vector<Recording>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kRecordingsPath, json(), response, error))
    return false;

  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected recordings response shape";
    return false;
  }

  time_t now = time(nullptr);
  out.clear();
  for (const auto& item : list)
  {
    Recording r;
    r.id = FieldOr(item, "id", 0);
    r.channelId = FieldOr(item, "channel", FieldOr(item, "channel_id", 0));
    r.startTime = TimeFromIso(FieldOr<std::string>(item, "start_time", ""));
    r.endTime = TimeFromIso(FieldOr<std::string>(item, "end_time", ""));
    r.durationSeconds =
        (r.endTime > r.startTime) ? static_cast<int>(r.endTime - r.startTime) : 0;
    r.isInProgress = r.startTime > 0 && r.startTime <= now && now < r.endTime;
    r.isUpcoming = r.startTime > now;

    // Dispatcharr's Recording object has no title/subtitle/description
    // fields of its own. Confirmed against a real recording: when created
    // without an explicit custom_properties, Dispatcharr auto-populates it
    // from the EPG programme that was airing, nested under
    // custom_properties.program.{title,sub_title,description} (alongside
    // status/file_url/etc. -- see CreateOneTimeRecording() for why this
    // addon no longer sends its own custom_properties on create, to avoid
    // stomping on that auto-enrichment). Also checks a flat
    // custom_properties.title as a fallback, in case something else wrote
    // one directly there.
    const json& custom = item.contains("custom_properties") ? item["custom_properties"] : json();
    if (custom.is_object())
    {
      // custom_properties.status is a more authoritative signal than the
      // start/end time window above when present: a recording stopped
      // early (see StopRecording()) keeps its originally-scheduled
      // end_time untouched, so the time-window check alone would keep
      // reporting it as in-progress for the rest of that original
      // duration even though it finished the moment it was stopped.
      // Confirmed values: "recording" (still active), "completed"/
      // "stopped"/"interrupted" (all finished, one way or another) --
      // exact enum not documented, so only treat "recording" as
      // authoritative for in-progress and fall back to the time window
      // for anything else/absent, rather than assuming a closed list.
      std::string status = FieldOr<std::string>(custom, "status", "");
      if (status == "recording")
        r.isInProgress = true;
      else if (!status.empty())
        r.isInProgress = false;

      const json& program = custom.contains("program") ? custom["program"] : json();
      if (program.is_object())
      {
        r.title = FieldOr<std::string>(program, "title", "");
        r.subtitle = FieldOr<std::string>(program, "sub_title", "");
        r.description = FieldOr<std::string>(program, "description", "");
      }
      if (r.title.empty())
        r.title = FieldOr<std::string>(custom, "title", "");
      if (r.subtitle.empty())
        r.subtitle = FieldOr<std::string>(custom, "sub_title", "");
      if (r.description.empty())
        r.description = FieldOr<std::string>(custom, "description", "");
    }
    if (r.title.empty())
    {
      // See PendingTitle's comment on why this can be filled in before
      // Dispatcharr's own async enrichment has caught up, and why it's
      // matched by channel alone rather than also start time.
      std::lock_guard<std::mutex> lock(m_pendingTitlesMutex);
      constexpr auto kPendingTitleTtl = std::chrono::minutes(3);
      auto now = std::chrono::steady_clock::now();
      m_pendingTitles.erase(
          std::remove_if(m_pendingTitles.begin(), m_pendingTitles.end(),
                          [&](const PendingTitle& p) { return now - p.insertedAt > kPendingTitleTtl; }),
          m_pendingTitles.end());
      // Deliberately NOT erased on match: this runs on every poll until
      // Dispatcharr's own enrichment lands (at which point r.title is no
      // longer empty and this isn't consulted again for that recording), so
      // erasing after the first match would make the title flicker back to
      // "Recording <id>" on the very next poll if enrichment hadn't caught
      // up yet. Left to expire via the TTL prune above instead.
      const PendingTitle* latest = nullptr;
      for (const auto& pending : m_pendingTitles)
      {
        if (pending.channelId == r.channelId && (!latest || pending.insertedAt > latest->insertedAt))
          latest = &pending;
      }
      if (latest)
        r.title = latest->title;
    }
    if (r.title.empty())
      r.title = "Recording " + std::to_string(r.id);
    out.push_back(std::move(r));
  }
  return true;
}

bool DispatcharrClient::GenerateApiKey(std::string& keyOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("POST", kApiKeyGeneratePath, json(), response, error))
    return false;

  std::string key = FieldOr<std::string>(response, "key", "");
  if (key.empty())
  {
    error = "API key generation response did not contain a key";
    return false;
  }
  m_config.apiKey = key;
  keyOut = std::move(key);
  return true;
}

bool DispatcharrClient::DeleteRecording(int recordingId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/";
  return Request("DELETE", path, json(), response, error);
}

bool DispatcharrClient::StopRecording(int recordingId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/stop/";
  return Request("POST", path, json(), response, error);
}

bool DispatcharrClient::GetTimerRules(std::vector<TimerRule>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kSeriesRulesPath, json(), response, error))
    return false;

  // Confirmed against a live instance: the response is {"rules": [...]},
  // not a bare array and not the usual DRF {"results": [...]} wrapper.
  const json& list = response.contains("rules")     ? response["rules"]
                      : response.contains("results") ? response["results"]
                                                      : response;
  if (!list.is_array())
  {
    error = "Unexpected series-rules response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
  {
    TimerRule t;
    // Exact per-rule field names aren't confirmed (the account available
    // while developing this lacked permission to create a series rule to
    // inspect one) -- "title"/"channel_id" match the confirmed
    // SeriesRuleRequest create payload, kept alongside the older assumed
    // names as fallbacks in case the list response shape differs.
    t.id = FieldOr(item, "id", 0);
    t.channelId = FieldOr(item, "channel_id", FieldOr(item, "channel", 0));
    t.tvgId = FieldOr<std::string>(item, "tvg_id", "");
    t.title = FieldOr(item, "title", FieldOr<std::string>(item, "title_pattern", ""));
    t.titlePattern = FieldOr<std::string>(item, "title_pattern", t.title);
    t.isSeries = true;
    t.recordNewOnly = FieldOr<std::string>(item, "mode", "all") == "new";
    out.push_back(std::move(t));
  }
  return true;
}

bool DispatcharrClient::CreateOneTimeRecording(
    int channelId, time_t start, time_t end, const std::string& title, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Confirmed against a real recording: channel/start_time/end_time are the
  // only fields this needs to send. Deliberately NOT sending its own
  // custom_properties (e.g. {"title": title}) -- confirmed that Dispatcharr
  // auto-populates custom_properties.program.{title,sub_title,description}
  // (plus status/file paths/poster logo) from whatever EPG programme was
  // actually airing on this channel at this time, and sending an explicit
  // custom_properties on create *replaces* that entirely rather than
  // merging, which would throw away the richer data for what's normally an
  // exact match anyway (Kodi's "record from guide" title already came from
  // that same EPG programme).
  json body = {
      {"channel", channelId},
      {"start_time", IsoFromTime(start)},
      {"end_time", IsoFromTime(end)},
  };
  json response;
  if (!Request("POST", kRecordingsPath, body, response, error))
    return false;

  // See PendingTitle's comment: cache the title Kodi already gave us (from
  // the EPG tag the "Record" button was pressed on) so GetRecordings() can
  // show it immediately instead of "Recording <id>" while Dispatcharr's own
  // async enrichment catches up. Only useful for the EPG-matched case --
  // title is empty for a fully manual time range with nothing airing.
  if (!title.empty())
  {
    std::lock_guard<std::mutex> lock(m_pendingTitlesMutex);
    m_pendingTitles.push_back({channelId, title, std::chrono::steady_clock::now()});
  }
  return true;
}

bool DispatcharrClient::CreateSeriesRule(int channelId, const std::string& tvgId,
                                         const std::string& titlePattern, bool recordNewOnly,
                                         std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Confirmed against the live SeriesRuleRequest schema: "title" and
  // "channel_id" (not "title_pattern"/"channel"). tvg_id is genuinely
  // optional ("omit to match across all channels") so it's only sent when
  // non-empty rather than risking an empty string being read as an
  // explicit "match only channels with a blank tvg_id" filter. "mode"
  // defaults server-side to "all" (every matching episode, including
  // reruns); only sent explicitly when "new" (first-run only) is wanted.
  json body = {
      {"channel_id", channelId},
      {"title", titlePattern},
  };
  if (!tvgId.empty())
    body["tvg_id"] = tvgId;
  if (recordNewOnly)
    body["mode"] = "new";
  json response;
  if (!Request("POST", kSeriesRulesPath, body, response, error))
    return false;

  // Ask Dispatcharr to evaluate the new rule immediately so upcoming
  // recordings show up right away rather than waiting for its own
  // background scheduler.
  std::string evalError;
  json evalBody = tvgId.empty() ? json::object() : json{{"tvg_id", tvgId}};
  json evalResponse;
  Request("POST", std::string(kSeriesRulesPath) + "evaluate/", evalBody, evalResponse, evalError);
  // A failed evaluate call is non-fatal: the rule was still created.
  return true;
}

bool DispatcharrClient::DeleteSeriesRule(const std::string& title, const std::string& tvgId,
                                         std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  // Confirmed against the live schema: series rules are deleted by
  // title + tvg_id query params, not a path id -- there is no
  // /api/channels/series-rules/{id}/ route.
  std::string path = std::string(kSeriesRulesPath) + "?title=" + UrlEncode(title);
  if (!tvgId.empty())
    path += "&tvg_id=" + UrlEncode(tvgId);
  json response;
  return Request("DELETE", path, json(), response, error);
}

std::string DispatcharrClient::GetInProgressRecordingStreamUrl(int recordingId) const
{
  std::string url = BaseUrl() + kRecordingsPath + std::to_string(recordingId) + "/hls/index.m3u8";
  // ffmpegdirect's header mapping (CDVDDemuxFFmpeg::GetFFMpegOptionsFromInput,
  // confirmed against its source and a live failed attempt: it logged
  // "ignoring header option 'X-API-Key'" without the prefix) only forwards a
  // fixed allowlist of standard HTTP header names as real headers -- anything
  // else needs a literal "!" prefix, which it strips before using the rest
  // as the header name. X-API-Key isn't on that allowlist.
  if (!m_config.apiKey.empty())
    url += "|!X-API-Key=" + UrlEncode(m_config.apiKey);
  return url;
}

bool DispatcharrClient::OpenRecordingStream(int recordingId, std::string& error)
{
  CloseRecordingStream();

  std::string url = BaseUrl() + kRecordingsPath + std::to_string(recordingId) + "/file/";

  // Up to two attempts: Dispatcharr keeps only one active API key
  // account-wide, so another Kodi install regenerating its own key can
  // silently invalidate this addon's stored one between restarts. On a 401,
  // regenerate once and retry before failing outright -- see GetApiKey()'s
  // comment for why the caller still needs to re-persist the result.
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      error = "Failed to initialise libcurl";
      return false;
    }

    struct curl_slist* headers = nullptr;
    std::string apiKeyHeader;
    if (!m_config.apiKey.empty())
    {
      apiKeyHeader = "X-API-Key: " + m_config.apiKey;
      headers = curl_slist_append(headers, apiKeyHeader.c_str());
    }

    // A tiny ranged GET rather than a HEAD request: confirmed the "in
    // progress -> redirect to HLS" behaviour on this endpoint, and it's
    // safer to assume that only applies to the method a real player
    // actually uses (GET) rather than trust it also applies to HEAD.
    std::string discard;
    int64_t totalLength = -1;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RecordingHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &totalLength);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    char* effectiveUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    char* contentTypeRaw = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentTypeRaw);
    std::string resolvedUrl = effectiveUrl ? effectiveUrl : url;
    std::string contentType = contentTypeRaw ? contentTypeRaw : "";
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
      error = std::string("HTTP request failed: ") + curl_easy_strerror(res);
      return false;
    }

    if (httpCode == 401 && attempt == 0)
    {
      std::string regenKey, regenError;
      if (GenerateApiKey(regenKey, regenError))
        continue;
    }

    if (httpCode < 200 || httpCode >= 300)
    {
      error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " opening recording stream";
      return false;
    }
    // Confirmed against a live instance: an in-progress recording's /file/
    // redirects to an HLS playlist (.../hls/index.m3u8), which this reader
    // doesn't understand -- treating it as a flat byte range would just
    // hand the demuxer m3u8 text instead of video.
    if (contentType.find("mpegurl") != std::string::npos ||
        resolvedUrl.find("/hls/") != std::string::npos)
    {
      error = "This recording is still in progress; playback of in-progress "
              "recordings isn't supported yet, only completed ones";
      return false;
    }

    m_recordingStream.open = true;
    m_recordingStream.url = resolvedUrl;
    m_recordingStream.length = totalLength;
    m_recordingStream.position = 0;
    return true;
  }
  error = "Dispatcharr returned HTTP 401 opening recording stream even after regenerating the API key";
  return false;
}

int DispatcharrClient::ReadRecordingStream(uint8_t* buffer, unsigned int size)
{
  if (!m_recordingStream.open || size == 0)
    return 0;
  if (m_recordingStream.length >= 0 && m_recordingStream.position >= m_recordingStream.length)
    return 0; // EOF

  int64_t rangeEnd = m_recordingStream.position + static_cast<int64_t>(size) - 1;
  std::string range = std::to_string(m_recordingStream.position) + "-" + std::to_string(rangeEnd);

  // See OpenRecordingStream(): the API key can be invalidated mid-playback
  // by another install regenerating it, so retry once after a self-heal.
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    // Reuse one persistent handle across every read (see RecordingStreamState's
    // comment) instead of curl_easy_init()/cleanup() per call, so libcurl's
    // connection cache lets HTTP keep-alive apply across sequential reads.
    CURL* curl = static_cast<CURL*>(m_recordingStream.curl);
    if (!curl)
    {
      curl = curl_easy_init();
      if (!curl)
        return -1;
      m_recordingStream.curl = curl;
    }

    struct curl_slist* headers = nullptr;
    std::string apiKeyHeader;
    if (!m_config.apiKey.empty())
    {
      apiKeyHeader = "X-API-Key: " + m_config.apiKey;
      headers = curl_slist_append(headers, apiKeyHeader.c_str());
    }

    FixedBufferSink sink{buffer, size, 0};
    curl_easy_setopt(curl, CURLOPT_URL, m_recordingStream.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);

    if (httpCode == 401 && attempt == 0)
    {
      std::string regenKey, regenError;
      if (GenerateApiKey(regenKey, regenError))
        continue;
    }

    if (res != CURLE_OK)
    {
      // A transport-level failure, as opposed to a bad HTTP status, might
      // mean the reused connection went stale/dead -- e.g. the server or an
      // intervening proxy silently closed a keep-alive connection during a
      // long pause. Drop the handle so the next read opens a fresh
      // connection instead of retrying the same broken one indefinitely.
      curl_easy_cleanup(curl);
      m_recordingStream.curl = nullptr;
      return -1;
    }
    if (httpCode != 200 && httpCode != 206)
      return -1;

    m_recordingStream.position += static_cast<int64_t>(sink.written);
    return static_cast<int>(sink.written);
  }
  return -1;
}

int64_t DispatcharrClient::SeekRecordingStream(int64_t position, int whence)
{
  if (!m_recordingStream.open)
    return -1;

  int64_t newPos;
  switch (whence)
  {
    case SEEK_SET:
      newPos = position;
      break;
    case SEEK_CUR:
      newPos = m_recordingStream.position + position;
      break;
    case SEEK_END:
      if (m_recordingStream.length < 0)
        return -1;
      newPos = m_recordingStream.length + position;
      break;
    default:
      return -1;
  }
  if (newPos < 0)
    return -1;

  m_recordingStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetRecordingStreamLength() const
{
  return m_recordingStream.length;
}

void DispatcharrClient::CloseRecordingStream()
{
  if (m_recordingStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_recordingStream.curl));
  m_recordingStream = RecordingStreamState();
}

} // namespace dispatcharr
