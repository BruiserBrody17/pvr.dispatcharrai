#include "DispatcharrClient.h"

#include <curl/curl.h>
#include <kodi/General.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <thread>

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
constexpr const char* kRecurringRulesPath = "/api/channels/recurring-rules/";
constexpr const char* kCoreSettingsPath = "/api/core/settings/";
// Confirmed against a live instance: every CoreSettings "group" (system
// timezone, DVR padding/comskip/path-templates, proxy tuning, ...) is one
// row in this generic key/value table, addressed by its own numeric id
// (not by key directly) -- GetDvrOffsetMinutes()/SetDvrOffsetMinutes()
// find the row with this key by listing and filtering, not by assuming a
// fixed id (that id is a plain auto-increment DB primary key and isn't
// guaranteed the same across different Dispatcharr installs).
constexpr const char* kDvrSettingsKey = "dvr_settings";
constexpr const char* kLogosPath = "/api/channels/logos/";
// Confirmed against a live instance: creates a session-bound catch-up
// (archived programme) playback URL that stays valid via a sliding idle
// window for as long as it's actively used, rather than embedding a
// short-lived JWT directly in the stream URL.
constexpr const char* kCatchupSessionsPath = "/api/catchup/sessions/";
constexpr const char* kApiKeyGeneratePath = "/api/accounts/api-keys/generate/";
// Fixed to this addon's own companion Dispatcharr plugin (see
// dispatcharr-plugin/timeshift_buffer/ in this repo) -- "timeshift_buffer"
// is that plugin's directory name, which Dispatcharr's loader uses
// verbatim as its registry key. Confirmed against Dispatcharr's own source
// (apps/plugins/api_urls.py) that the generic run endpoint takes the
// plugin key as a URL segment, not a request body field.
constexpr const char* kTimeshiftPluginRunPath = "/api/plugins/plugins/timeshift_buffer/run/";
// Same mechanism, this addon's other companion plugin (see
// dispatcharr-plugin/recording_edl/ in this repo).
constexpr const char* kRecordingEdlPluginRunPath = "/api/plugins/plugins/recording_edl/run/";

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

// Captures the size from a plain "Content-Length: N" response header --
// used for a HEAD probe rather than RecordingHeaderCallback's ranged-GET
// Content-Range parsing, since Dispatcharr's in-progress-recording HLS
// segment endpoint ignores the Range header entirely and always serves the
// full segment body with a 200 (confirmed live: a "Range: 0-0" GET against
// a growing recording's seg_NNNNN.ts came back 200 with no Content-Range
// header at all, silently downloading the whole multi-MB segment on every
// probe instead of the intended few bytes -- HEAD avoids the body
// entirely, and this reads the size the same server response always
// carries either way).
size_t ContentLengthHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* totalOut = static_cast<int64_t*>(userdata);
  size_t len = size * nitems;
  std::string line(buffer, len);
  std::string prefix = line.size() >= 15 ? line.substr(0, 15) : std::string();
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (prefix == "content-length:")
  {
    try
    {
      *totalOut = std::stoll(line.substr(15));
    }
    catch (const std::exception&)
    {
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

// "HH:MM:SS" for a plain seconds-since-midnight value, wrapping into
// [0, 86400) first -- callers may have shifted a UTC time-of-day by
// recurring_rule_utc_offset_minutes, which can push it negative or past
// 24h before this is called.
std::string TimeOfDayString(int secondsSinceMidnight)
{
  int s = secondsSinceMidnight % 86400;
  if (s < 0)
    s += 86400;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return std::string(buf);
}

// Inverse of TimeOfDayString(): parses "HH:MM:SS" (or "HH:MM") into
// seconds since midnight. Returns 0 on anything unparseable.
int SecondsSinceMidnightFromString(const std::string& hms)
{
  int h = 0, m = 0, s = 0;
  if (std::sscanf(hms.c_str(), "%d:%d:%d", &h, &m, &s) < 2)
    return 0;
  return h * 3600 + m * 60 + s;
}

// "YYYY-MM-DD" for the UTC calendar date of a time_t (this addon only
// ever stores a rule's start_date/end_date as UTC midnight of the
// intended local calendar date -- see RecurringRule's own comment).
std::string DateStringFromTime(time_t t)
{
  char buf[16];
  struct tm tmVal{};
#if defined(_WIN32)
  gmtime_s(&tmVal, &t);
#else
  gmtime_r(&t, &tmVal);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmVal);
  return std::string(buf);
}

// Inverse of DateStringFromTime(): parses "YYYY-MM-DD" into a UTC
// midnight time_t. Returns 0 on anything unparseable.
time_t TimeFromDateString(const std::string& dateStr)
{
  if (dateStr.size() < 10)
    return 0;
  struct tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(dateStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(dateStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(dateStr.substr(8, 2));
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

// Unique-enough per-Open()-session id for the plugin's viewer reference
// counting (see LiveTimeshiftStreamState::viewerId's own comment) -- only
// needs to not collide between viewers concurrently watching the same
// channel, not to be cryptographically unguessable, so a random 64-bit
// value hex-encoded is plenty.
std::string GenerateViewerId()
{
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist;
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(rng)));
  return std::string(buf);
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
  if (withAuth)
  {
    // Copy under the lock rather than reading m_accessToken directly here
    // -- this can run concurrently with Login()/RefreshAccessToken()
    // writing it from another thread (see m_authMutex's own comment).
    std::string token;
    {
      std::lock_guard<std::recursive_mutex> lock(m_authMutex);
      token = m_accessToken;
    }
    if (!token.empty())
    {
      authHeader = "Authorization: Bearer " + token;
      headers = curl_slist_append(headers, authHeader.c_str());
    }
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
  if (res == CURLE_OK)
  {
    char* localIp = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_LOCAL_IP, &localIp) == CURLE_OK && localIp && *localIp)
    {
      std::lock_guard<std::mutex> lock(m_lastLocalIpMutex);
      m_lastLocalIp = localIp;
    }
  }
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
  std::lock_guard<std::recursive_mutex> lock(m_authMutex);
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
  std::lock_guard<std::recursive_mutex> lock(m_authMutex);
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
  std::lock_guard<std::recursive_mutex> lock(m_authMutex);
  if (!m_accessToken.empty() && std::chrono::steady_clock::now() < m_accessTokenExpiry)
    return true;
  if (!m_refreshToken.empty() && RefreshAccessToken(error))
    return true;
  return Login(error);
}

bool DispatcharrClient::GetAccessToken(std::string& tokenOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  std::lock_guard<std::recursive_mutex> lock(m_authMutex);
  tokenOut = m_accessToken;
  return true;
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

bool DispatcharrClient::WaitForTimeshiftPlaylistReady(const std::string& playlistUrl)
{
  // Confirmed live the hard way: the plugin's start_buffer response comes
  // back as soon as ffmpeg has been launched, not once it's actually
  // produced anything -- a fresh ffmpeg process (e.g. right after the
  // previous one was idle-reaped) needs a real moment to connect to
  // Dispatcharr's live proxy, probe the stream, and write its first
  // segment and playlist. Handing STREAMURL to ffmpegdirect immediately
  // raced that and failed outright ("Error, could not open file"), even
  // though the exact same URL was trivially fetchable moments later by
  // hand -- the file just didn't exist on disk yet at the moment
  // ffmpegdirect tried. Polls the playlist URL itself (no auth needed --
  // the plugin's own file server doesn't require any) rather than the
  // Dispatcharr API, up to a few seconds, so GetChannelStreamProperties()
  // only hands back a STREAMURL once there's actually something there to
  // open. Best-effort: if it never becomes ready in time, still returns
  // (false) rather than blocking indefinitely -- the caller proceeds with
  // the URL regardless, on the chance it becomes ready a moment later
  // anyway, but at least the common case no longer races this.
  constexpr int kMaxAttempts = 20;
  constexpr int kAttemptTimeoutMs = 500;
  constexpr int kSleepBetweenMs = 250;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
  {
    CURL* curl = curl_easy_init();
    if (!curl)
      return false;

    std::string discard;
    curl_easy_setopt(curl, CURLOPT_URL, playlistUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(kAttemptTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && httpCode == 200)
      return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepBetweenMs));
  }
  return false;
}

bool DispatcharrClient::CallTimeshiftPluginAction(const std::string& action,
                                                    const std::string& channelUuid,
                                                    std::string& playlistUrlOut,
                                                    std::string& error,
                                                    const json& extraParams)
{
  if (!EnsureAuthenticated(error))
    return false;

  json params = {{"channel_uuid", channelUuid}};
  params.update(extraParams);
  json body = {
      {"action", action},
      {"params", params},
  };

  json response;
  // Request() already turns any non-2xx (403 disabled-plugin/non-admin
  // account, 404 plugin-not-installed, 500 exception inside the plugin's
  // own run()) into a failure here, with the raw response body folded into
  // `error` -- confirmed against apps/plugins/api_views.py's
  // PluginRunAPIView that every one of those paths pairs "success": false
  // with a matching non-2xx status, never 200. So this call only needs to
  // handle the 200 case below: PluginRunAPIView always wraps whatever the
  // plugin's own run() returned inside a top-level "result" key (alongside
  // its own "success": true), and the plugin can still report its own
  // *logical* failure (e.g. hitting max_concurrent_buffers, or a buffer
  // that's genuinely gone -- see docs/TIMESHIFT.md's "concurrent-stream
  // limit" section) as a normal 200 response with "result": {"status":
  // "error", ...} rather than an exception -- that's the case the
  // "result" parsing below actually exists to catch.
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error))
    return false;

  if (!FieldOr(response, "success", false))
  {
    error = FieldOr<std::string>(response, "error", "timeshift_buffer plugin call did not succeed");
    return false;
  }

  const json& result = response.contains("result") ? response["result"] : json();
  if (FieldOr<std::string>(result, "status", "") != "ok")
  {
    error = FieldOr<std::string>(result, "message", "timeshift_buffer plugin returned an error");
    return false;
  }

  int httpPort = FieldOr(result, "http_port", 0);
  std::string playlistRoute = FieldOr<std::string>(result, "playlist_route", "");
  if (httpPort <= 0 || playlistRoute.empty())
  {
    error = "timeshift_buffer plugin response was missing http_port/playlist_route";
    return false;
  }

  playlistUrlOut = "http://" + m_config.host + ":" + std::to_string(httpPort) + playlistRoute;
  WaitForTimeshiftPlaylistReady(playlistUrlOut); // best-effort; see its own comment
  return true;
}

bool DispatcharrClient::StartTimeshiftBuffer(const std::string& channelUuid,
                                              std::string& playlistUrlOut,
                                              std::string& error)
{
  // Passed through so the plugin's ffmpeg connection (which otherwise
  // looks anonymous and container-local in Dispatcharr's own Stats screen,
  // since it runs server-side rather than from the viewer's own device --
  // confirmed live) can be attributed properly: username to the same
  // Dispatcharr account this addon is already configured with (no separate
  // plugin-side setting to keep in sync), client_ip to whichever local
  // interface this machine actually reaches Dispatcharr through (from
  // Request()'s own CURLINFO_LOCAL_IP, not a platform-specific "what's my
  // IP" lookup).
  json extraParams = {{"username", m_config.username}};
  {
    std::lock_guard<std::mutex> lock(m_lastLocalIpMutex);
    if (!m_lastLocalIp.empty())
      extraParams["client_ip"] = m_lastLocalIp;
  }
  // Lets the plugin reference-count viewers of a shared buffer -- see
  // LiveTimeshiftStreamState::viewerId's own comment and
  // StopTimeshiftBuffer()'s for the full mechanism this enables.
  if (!m_liveTimeshiftStream.viewerId.empty())
    extraParams["viewer_id"] = m_liveTimeshiftStream.viewerId;
  return CallTimeshiftPluginAction("start_buffer", channelUuid, playlistUrlOut, error, extraParams);
}

bool DispatcharrClient::StopTimeshiftBuffer(const std::string& channelUuid, const std::string& viewerId,
                                            std::string& error)
{
  // Doesn't use CallTimeshiftPluginAction(): that helper requires
  // http_port/playlist_route in the response, which stop_buffer's own
  // {"status": "ok"} reply doesn't carry.
  if (!EnsureAuthenticated(error))
    return false;

  json params = {{"channel_uuid", channelUuid}};
  if (!viewerId.empty())
    params["viewer_id"] = viewerId;

  json body = {
      {"action", "stop_buffer"},
      {"params", params},
  };
  json response;
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error))
    return false;
  if (!FieldOr(response, "success", false))
  {
    error = FieldOr<std::string>(response, "error", "timeshift_buffer plugin call did not succeed");
    return false;
  }
  const json& result = response.contains("result") ? response["result"] : json();
  if (FieldOr<std::string>(result, "status", "") != "ok")
  {
    error = FieldOr<std::string>(result, "message", "timeshift_buffer plugin returned an error");
    return false;
  }
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

      // Tagged by Dispatcharr's own recurring-rule scheduler (confirmed
      // against its source: custom_properties.rule =
      // {"type": "recurring", "id": <rule id>, ...}) -- see
      // RecurringRule's own comment for how this links back to its
      // parent rule as a Kodi timer.
      const json& rule = custom.contains("rule") ? custom["rule"] : json();
      if (rule.is_object() && FieldOr<std::string>(rule, "type", "") == "recurring")
        r.recurringRuleId = FieldOr(rule, "id", 0);
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

bool DispatcharrClient::GetRecordingEdl(int recordingId, std::vector<RecordingEdlEntry>& out,
                                         std::string& error)
{
  out.clear();
  if (!EnsureAuthenticated(error))
    return false;

  json body = {
      {"action", "get_edl"},
      {"params", {{"recording_id", recordingId}}},
  };

  json response;
  // Same response shape as CallTimeshiftPluginAction()'s own comment
  // describes: Request() already turns any non-2xx (plugin not installed/
  // enabled, non-admin account, exception inside the plugin's own run())
  // into a failure here, so only the 200 case needs handling below.
  if (!Request("POST", kRecordingEdlPluginRunPath, body, response, error))
    return false;

  if (!FieldOr(response, "success", false))
  {
    error = FieldOr<std::string>(response, "error", "recording_edl plugin call did not succeed");
    return false;
  }

  const json& result = response.contains("result") ? response["result"] : json();
  if (FieldOr<std::string>(result, "status", "") != "ok")
  {
    error = FieldOr<std::string>(result, "message", "recording_edl plugin returned an error");
    return false;
  }

  const json& entries = result.contains("entries") ? result["entries"] : json();
  if (entries.is_array())
  {
    for (const auto& item : entries)
    {
      RecordingEdlEntry entry;
      entry.startMs = FieldOr<int64_t>(item, "start", 0);
      entry.endMs = FieldOr<int64_t>(item, "end", 0);
      entry.type = FieldOr(item, "type", 3);
      if (entry.endMs > entry.startMs)
        out.push_back(entry);
    }
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
  {
    std::lock_guard<std::mutex> lock(m_apiKeyMutex);
    m_config.apiKey = key;
  }
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

bool DispatcharrClient::UpdateOneTimeRecording(int recordingId, time_t start, time_t end,
                                               std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Both fields always included -- confirmed live that a PATCH omitting
  // them crashes server-side (see this method's own header comment).
  json body = {
      {"start_time", IsoFromTime(start)},
      {"end_time", IsoFromTime(end)},
  };
  json response;
  return Request("PATCH", std::string(kRecordingsPath) + std::to_string(recordingId) + "/", body,
                 response, error);
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

bool DispatcharrClient::GetRecurringRules(std::vector<RecurringRule>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kRecurringRulesPath, json(), response, error))
    return false;

  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected recurring-rules response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
  {
    RecurringRule rule;
    rule.id = FieldOr(item, "id", 0);
    rule.channelId = FieldOr(item, "channel", 0);
    rule.name = FieldOr<std::string>(item, "name", "");
    rule.enabled = FieldOr(item, "enabled", true);
    rule.startTimeOfDaySeconds =
        SecondsSinceMidnightFromString(FieldOr<std::string>(item, "start_time", ""));
    rule.endTimeOfDaySeconds =
        SecondsSinceMidnightFromString(FieldOr<std::string>(item, "end_time", ""));
    rule.startDate = TimeFromDateString(FieldOr<std::string>(item, "start_date", ""));
    rule.endDate = TimeFromDateString(FieldOr<std::string>(item, "end_date", ""));
    const json& days = item.contains("days_of_week") ? item["days_of_week"] : json();
    if (days.is_array())
    {
      for (const auto& d : days)
      {
        if (d.is_number_integer())
          rule.daysOfWeek.push_back(d.get<int>());
      }
    }
    out.push_back(std::move(rule));
  }
  return true;
}

bool DispatcharrClient::CreateRecurringRule(int channelId, const std::string& name,
                                            const std::vector<int>& daysOfWeek,
                                            int startTimeOfDaySeconds, int endTimeOfDaySeconds,
                                            time_t startDate, time_t endDate, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Confirmed against the live RecurringRecordingRuleSerializer: channel is
  // a plain FK id (no uuid field on this model, unlike Channel itself),
  // days_of_week a non-empty list of ints 0-6, start_time/end_time plain
  // "HH:MM:SS" with no timezone suffix, and start_date/end_date are both
  // required despite the model declaring them nullable -- confirmed by its
  // validate() raising "Start date is required"/"End date is required"
  // when either is omitted, so both are always sent here.
  json body = {
      {"channel", channelId},
      {"name", name},
      {"days_of_week", daysOfWeek},
      {"start_time", TimeOfDayString(startTimeOfDaySeconds)},
      {"end_time", TimeOfDayString(endTimeOfDaySeconds)},
      {"start_date", DateStringFromTime(startDate)},
      {"end_date", DateStringFromTime(endDate)},
      {"enabled", true},
  };
  json response;
  return Request("POST", kRecurringRulesPath, body, response, error);
}

bool DispatcharrClient::UpdateRecurringRule(int ruleId, int channelId, const std::string& name,
                                            const std::vector<int>& daysOfWeek,
                                            int startTimeOfDaySeconds, int endTimeOfDaySeconds,
                                            time_t startDate, bool enabled, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // No end_date here -- see this method's own header comment on why
  // omitting it lets the existing value survive untouched via
  // RecurringRecordingRuleSerializer's partial-update fallback, rather
  // than needing a separate fetch to preserve it.
  json body = {
      {"channel", channelId},
      {"name", name},
      {"days_of_week", daysOfWeek},
      {"start_time", TimeOfDayString(startTimeOfDaySeconds)},
      {"end_time", TimeOfDayString(endTimeOfDaySeconds)},
      {"start_date", DateStringFromTime(startDate)},
      {"enabled", enabled},
  };
  json response;
  return Request("PATCH", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", body,
                 response, error);
}

bool DispatcharrClient::DeleteRecurringRule(int ruleId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  return Request("DELETE", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/",
                 json(), response, error);
}

bool DispatcharrClient::ExtendRecurringRuleEndDate(int ruleId, time_t newEndDate,
                                                   std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json body = {{"end_date", DateStringFromTime(newEndDate)}};
  json response;
  return Request("PATCH", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", body,
                 response, error);
}

bool DispatcharrClient::FindDvrSettingsRow(int& idOut, json& valueOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kCoreSettingsPath, json(), response, error))
    return false;

  // Confirmed against a live instance: a bare array, not {"results": [...]}
  // -- but tolerate that wrapper too, matching this client's usual
  // defensive style for list endpoints (see GetChannels()'s own comment).
  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected /api/core/settings/ response shape";
    return false;
  }

  for (const auto& row : list)
  {
    if (FieldOr<std::string>(row, "key", "") == kDvrSettingsKey)
    {
      idOut = FieldOr(row, "id", 0);
      valueOut = row.contains("value") && row["value"].is_object() ? row["value"] : json::object();
      return idOut != 0;
    }
  }
  error = "Dispatcharr has no dvr_settings row in /api/core/settings/";
  return false;
}

bool DispatcharrClient::GetDvrOffsetMinutes(int& preMinutesOut, int& postMinutesOut, std::string& error)
{
  int id = 0;
  json value;
  if (!FindDvrSettingsRow(id, value, error))
    return false;
  preMinutesOut = FieldOr(value, "pre_offset_minutes", 0);
  postMinutesOut = FieldOr(value, "post_offset_minutes", 0);
  return true;
}

bool DispatcharrClient::SetDvrOffsetMinutes(int preMinutes, int postMinutes, std::string& error)
{
  int id = 0;
  json value;
  if (!FindDvrSettingsRow(id, value, error))
    return false;

  // Modify only the two offset keys -- everything else already in this
  // blob (comskip settings, path templates, ...) is sent back exactly as
  // read, not reconstructed, so nothing this addon doesn't know about
  // ever gets silently dropped.
  value["pre_offset_minutes"] = preMinutes;
  value["post_offset_minutes"] = postMinutes;

  json body = {{"value", value}};
  json response;
  return Request("PATCH", std::string(kCoreSettingsPath) + std::to_string(id) + "/", body, response,
                 error);
}

bool DispatcharrClient::IsApiKeyValidFor(const std::string& url) const
{
  CURL* curl = curl_easy_init();
  if (!curl)
    return true; // fail open: a local curl-init failure isn't evidence the key is bad

  struct curl_slist* headers = nullptr;
  std::string apiKeyHeader = "X-API-Key: " + GetApiKey();
  headers = curl_slist_append(headers, apiKeyHeader.c_str());

  std::string discard;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  return res != CURLE_OK || httpCode != 401;
}

bool DispatcharrClient::FetchRawInProgressPlaylist(int recordingId, const std::string& playlistUrl,
                                                    std::string& playlistText, std::string& error)
{
  // Two attempts: the playlist fetch itself can self-heal on a 401, same
  // pattern as OpenRecordingStream().
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
    std::string apiKey = GetApiKey();
    if (!apiKey.empty())
    {
      apiKeyHeader = "X-API-Key: " + apiKey;
      headers = curl_slist_append(headers, apiKeyHeader.c_str());
    }

    playlistText.clear();
    curl_easy_setopt(curl, CURLOPT_URL, playlistUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &playlistText);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
      error = std::string("HTTP request failed fetching playlist: ") + curl_easy_strerror(res);
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
      error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " fetching in-progress playlist";
      return false;
    }
    return true;
  }
  return false;
}

int64_t DispatcharrClient::ProbeSegmentByteSize(const std::string& segmentUrl) const
{
  CURL* curl = curl_easy_init();
  if (!curl)
    return -1;

  struct curl_slist* headers = nullptr;
  std::string apiKeyHeader;
  std::string apiKey = GetApiKey();
  if (!apiKey.empty())
  {
    apiKeyHeader = "X-API-Key: " + apiKey;
    headers = curl_slist_append(headers, apiKeyHeader.c_str());
  }

  int64_t totalLength = -1;
  curl_easy_setopt(curl, CURLOPT_URL, segmentUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ContentLengthHeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &totalLength);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || httpCode != 200)
    return -1;
  return totalLength;
}

bool DispatcharrClient::RefreshInProgressRecordingManifest(bool force, std::string& error)
{
  if (!m_inProgressRecordingStream.open)
  {
    error = "no in-progress recording stream is open";
    return false;
  }

  // Throttle for the same reason as RefreshLiveManifest(): a tight
  // catch-up-loop/demux-read cycle can call this far more often than the
  // recording could possibly have grown.
  constexpr auto kMinRefreshInterval = std::chrono::milliseconds(500);
  auto now = std::chrono::steady_clock::now();
  if (!force && m_inProgressRecordingStream.lastManifestFetch.time_since_epoch().count() != 0 &&
      now - m_inProgressRecordingStream.lastManifestFetch < kMinRefreshInterval)
    return true;

  std::string baseDir =
      BaseUrl() + kRecordingsPath + std::to_string(m_inProgressRecordingStream.recordingId) + "/hls/";
  std::string playlistUrl = baseDir + "index.m3u8";

  std::string playlistText;
  if (!FetchRawInProgressPlaylist(m_inProgressRecordingStream.recordingId, playlistUrl, playlistText,
                                   error))
    return false;

  // Append-only merge: segments before the count we already know about are
  // skipped (no rolling-window eviction for a recording -- see
  // InProgressRecordingStreamState's own comment), everything past it is
  // new. #EXTINF: precedes each segment URI line with its duration; HLS
  // never carries byte size, so each newly-discovered segment gets a tiny
  // ranged-GET probe for that.
  size_t alreadyKnown = m_inProgressRecordingStream.segments.size();
  size_t segmentIndex = 0;
  double pendingDurationSec = 0.0;
  size_t pos = 0;
  while (pos <= playlistText.size())
  {
    size_t newlinePos = playlistText.find('\n', pos);
    std::string line = (newlinePos == std::string::npos) ? playlistText.substr(pos)
                                                           : playlistText.substr(pos, newlinePos - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.compare(0, 8, "#EXTINF:") == 0)
    {
      std::string durStr = line.substr(8);
      size_t comma = durStr.find(',');
      if (comma != std::string::npos)
        durStr = durStr.substr(0, comma);
      try
      {
        pendingDurationSec = std::stod(durStr);
      }
      catch (const std::exception&)
      {
        pendingDurationSec = 0.0;
      }
    }
    else if (!line.empty() && line[0] != '#')
    {
      if (segmentIndex >= alreadyKnown)
      {
        std::string segUrl = (line.compare(0, 7, "http://") == 0 || line.compare(0, 8, "https://") == 0)
                                  ? line
                                  : baseDir + line;
        int64_t segSize = ProbeSegmentByteSize(segUrl);
        if (segSize > 0)
        {
          InProgressRecordingSegmentInfo info;
          info.url = std::move(segUrl);
          info.byteOffset = m_inProgressRecordingStream.totalBytes;
          info.byteSize = segSize;
          info.timeOffsetMs = m_inProgressRecordingStream.totalDurationMs;
          m_inProgressRecordingStream.totalBytes += segSize;
          m_inProgressRecordingStream.totalDurationMs +=
              static_cast<int64_t>(pendingDurationSec * 1000 + 0.5);
          m_inProgressRecordingStream.segments.push_back(std::move(info));
        }
        // A segment that can't be sized (transient network hiccup, or
        // recycled mid-probe) is skipped rather than retried here --
        // segmentIndex still advances, so it's simply missing from this
        // stream's address space; the next manifest refresh only looks at
        // segments past the current known count anyway, so a skipped one
        // is never retried. Rare enough in practice (a recording's own
        // segments aren't recycled) not to warrant more than that.
      }
      ++segmentIndex;
      pendingDurationSec = 0.0;
    }

    if (newlinePos == std::string::npos)
      break;
    pos = newlinePos + 1;
  }

  // Fresh in-progress check every call, not just at open: this is what
  // lets ReadInProgressRecordingStream() eventually stop waiting for a
  // recording that's actually finished, and CanPauseStream()/
  // IsRealTimeStream() reflect current reality rather than whatever was
  // true when the stream was opened.
  bool stillInProgress = false;
  std::vector<Recording> recordings;
  std::string recordingsError;
  if (GetRecordings(recordings, recordingsError))
  {
    for (const auto& rec : recordings)
    {
      if (rec.id == m_inProgressRecordingStream.recordingId)
      {
        stillInProgress = rec.isInProgress;
        break;
      }
    }
  }
  m_inProgressRecordingStream.finished = !stillInProgress;

  // Proactive self-heal: cheaper to catch a stale key here than mid-read.
  if (!GetApiKey().empty() && !IsApiKeyValidFor(playlistUrl))
  {
    std::string regenKey, regenError;
    GenerateApiKey(regenKey, regenError);
  }

  m_inProgressRecordingStream.lastManifestFetch = now;
  return true;
}

bool DispatcharrClient::OpenInProgressRecordingStream(int recordingId, std::string& error)
{
  CloseInProgressRecordingStream();

  m_inProgressRecordingStream.open = true;
  m_inProgressRecordingStream.recordingId = recordingId;

  // Cold-start grace period, same reasoning as OpenLiveTimeshiftStream()'s:
  // Dispatcharr's own DVR ffmpeg needs a real few seconds to connect to
  // the live proxy and produce a full first HLS segment before there's
  // anything to report -- confirmed live this addon's own
  // ReadInProgressRecordingStream() catch-up loop wasn't a substitute for
  // this: Kodi's own CDVDDemuxFFmpeg::Open() format probe gave up after
  // ~38s with "error probing input format" rather than retrying patiently
  // the way this addon's own reads do, so Open() itself needs to already
  // have at least one real segment to hand it before returning. 45s, not
  // the 15s live-timeshift uses: confirmed live a 15s budget still wasn't
  // enough here and reading Dispatcharr's own DVR task source
  // (apps/channels/tasks.py) explains why -- it documents its own
  // `_first_segment_timeout = 15.0` for *just* the first-segment wait,
  // on top of whatever real time the recording task itself takes to get
  // scheduled and its own ffmpeg connected before that timer even starts.
  constexpr int kColdStartMaxAttempts = 90;
  constexpr int kColdStartSleepMs = 500;
  bool haveSegment = false;
  for (int attempt = 0; attempt < kColdStartMaxAttempts; ++attempt)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: OpenInProgressRecordingStream: cold-start attempt=%d",
              attempt);
    if (!RefreshInProgressRecordingManifest(/*force=*/true, error))
    {
      m_inProgressRecordingStream = InProgressRecordingStreamState();
      return false;
    }
    if (!m_inProgressRecordingStream.segments.empty())
    {
      haveSegment = true;
      break;
    }
    if (m_inProgressRecordingStream.finished)
      break; // finished with literally zero segments -- nothing to wait for
    std::this_thread::sleep_for(std::chrono::milliseconds(kColdStartSleepMs));
  }
  if (!haveSegment && !m_inProgressRecordingStream.finished)
  {
    error = "recording hasn't produced any segments yet";
    m_inProgressRecordingStream = InProgressRecordingStreamState();
    return false;
  }
  // Unlike live-timeshift, always start at true byte 0: there's no
  // server-side buffer this addon starts/stops, no "live edge" concept to
  // land near -- Dispatcharr's own DVR task has been writing this
  // recording since it began regardless of whether anything's reading it,
  // so "play a recording" naturally means "from the start", matching
  // every other recording/VOD convention (and the existing
  // completed-recording behaviour).
  m_inProgressRecordingStream.position = 0;
  return true;
}

int DispatcharrClient::ReadInProgressRecordingStream(uint8_t* buffer, unsigned int size)
{
  if (!m_inProgressRecordingStream.open || size == 0)
    return 0;

  if (m_inProgressRecordingStream.position >= m_inProgressRecordingStream.totalBytes)
  {
    if (m_inProgressRecordingStream.finished)
      return 0; // genuine EOF -- the recording is done and we're at its true end

    constexpr int kCatchUpSleepMs = 250;

    // Same seek-probe-vs-genuine-catch-up distinction as
    // ReadLiveTimeshiftStream() -- see its own comment for the full
    // reasoning (confirmed live there; the same generic Kodi/ffmpeg
    // seek-probing behaviour applies here, since this uses the same
    // native-demuxer mechanism).
    constexpr auto kSeekProbeWindow = std::chrono::milliseconds(800);
    bool sameAsLastShortGiveUp =
        m_inProgressRecordingStream.position == m_inProgressRecordingStream.lastShortGiveUpPosition;
    bool likelySeekProbe =
        !sameAsLastShortGiveUp &&
        m_inProgressRecordingStream.lastSeekTime.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - m_inProgressRecordingStream.lastSeekTime < kSeekProbeWindow;

    int64_t lastSegmentDurationMs =
        m_inProgressRecordingStream.segments.empty()
            ? 6000
            : m_inProgressRecordingStream.totalDurationMs -
                  m_inProgressRecordingStream.segments.back().timeOffsetMs;
    if (lastSegmentDurationMs <= 0)
      lastSegmentDurationMs = 6000;

    int catchUpAttempts =
        likelySeekProbe
            ? 1
            : static_cast<int>((lastSegmentDurationMs * 3 / 2) / kCatchUpSleepMs) + 1;

    for (int attempt = 0; attempt < catchUpAttempts &&
                           m_inProgressRecordingStream.position >= m_inProgressRecordingStream.totalBytes;
         ++attempt)
    {
      std::string refreshError;
      RefreshInProgressRecordingManifest(/*force=*/true, refreshError);
      if (m_inProgressRecordingStream.position < m_inProgressRecordingStream.totalBytes)
        break;
      if (m_inProgressRecordingStream.finished)
        break; // finished while we were polling -- stop waiting, report EOF below
      if (attempt + 1 < catchUpAttempts)
        std::this_thread::sleep_for(std::chrono::milliseconds(kCatchUpSleepMs));
    }

    bool caughtUp = m_inProgressRecordingStream.position < m_inProgressRecordingStream.totalBytes;
    m_inProgressRecordingStream.lastShortGiveUpPosition =
        (likelySeekProbe && !caughtUp) ? m_inProgressRecordingStream.position : -1;

    if (!caughtUp)
      return 0;
  }

  const InProgressRecordingSegmentInfo* seg = nullptr;
  for (const auto& s : m_inProgressRecordingStream.segments)
  {
    if (m_inProgressRecordingStream.position >= s.byteOffset &&
        m_inProgressRecordingStream.position < s.byteOffset + s.byteSize)
    {
      seg = &s;
      break;
    }
  }
  if (!seg)
    return 0; // shouldn't happen (no rolling eviction here), but nothing safely readable if it did

  int64_t offsetInSegment = m_inProgressRecordingStream.position - seg->byteOffset;

  // Fetch this segment's full body once and cache it, rather than issuing a
  // ranged GET per read -- see InProgressRecordingStreamState's own comment
  // on cachedSegmentBytes for why a ranged read against this specific
  // endpoint would silently return the wrong bytes, not just be wasteful.
  if (m_inProgressRecordingStream.cachedSegmentByteOffset != seg->byteOffset)
  {
    for (int attempt = 0; attempt < 2; ++attempt)
    {
      CURL* curl = static_cast<CURL*>(m_inProgressRecordingStream.curl);
      if (!curl)
      {
        curl = curl_easy_init();
        if (!curl)
          return -1;
        m_inProgressRecordingStream.curl = curl;
      }

      struct curl_slist* headers = nullptr;
      std::string apiKeyHeader;
      std::string apiKey = GetApiKey();
      if (!apiKey.empty())
      {
        apiKeyHeader = "X-API-Key: " + apiKey;
        headers = curl_slist_append(headers, apiKeyHeader.c_str());
      }

      std::string body;
      body.reserve(static_cast<size_t>(seg->byteSize));
      curl_easy_setopt(curl, CURLOPT_URL, seg->url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
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
        // Same "reused connection went stale" handling as ReadRecordingStream().
        curl_easy_cleanup(curl);
        m_inProgressRecordingStream.curl = nullptr;
        return -1;
      }
      if (httpCode != 200)
        return -1;

      m_inProgressRecordingStream.cachedSegmentBytes.assign(body.begin(), body.end());
      m_inProgressRecordingStream.cachedSegmentByteOffset = seg->byteOffset;
      break;
    }
    if (m_inProgressRecordingStream.cachedSegmentByteOffset != seg->byteOffset)
      return -1; // both attempts failed
  }

  const auto& cached = m_inProgressRecordingStream.cachedSegmentBytes;
  if (offsetInSegment < 0 || static_cast<size_t>(offsetInSegment) >= cached.size())
    return 0; // segment turned out smaller than the probed byteSize -- nothing left to give

  int64_t available = static_cast<int64_t>(cached.size()) - offsetInSegment;
  unsigned int wantSize = static_cast<unsigned int>(std::min<int64_t>(size, available));
  std::memcpy(buffer, cached.data() + offsetInSegment, wantSize);
  m_inProgressRecordingStream.position += static_cast<int64_t>(wantSize);
  return static_cast<int>(wantSize);
}

int64_t DispatcharrClient::SeekInProgressRecordingStream(int64_t position, int whence)
{
  if (!m_inProgressRecordingStream.open)
    return -1;

  m_inProgressRecordingStream.lastSeekTime = std::chrono::steady_clock::now();

  if (whence == SEEK_END)
  {
    std::string refreshError;
    RefreshInProgressRecordingManifest(/*force=*/true, refreshError);
  }

  int64_t newPos;
  switch (whence)
  {
    case SEEK_SET:
      newPos = position;
      break;
    case SEEK_CUR:
      newPos = m_inProgressRecordingStream.position + position;
      break;
    case SEEK_END:
      newPos = m_inProgressRecordingStream.totalBytes + position;
      break;
    default:
      return -1;
  }
  if (newPos < 0)
    return -1;
  if (newPos > m_inProgressRecordingStream.totalBytes)
    newPos = m_inProgressRecordingStream.totalBytes;

  m_inProgressRecordingStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetInProgressRecordingStreamLength()
{
  if (!m_inProgressRecordingStream.open)
    return -1;
  std::string refreshError;
  RefreshInProgressRecordingManifest(/*force=*/false, refreshError);
  return m_inProgressRecordingStream.totalBytes;
}

int64_t DispatcharrClient::GetInProgressRecordingStreamDurationMs()
{
  if (!m_inProgressRecordingStream.open)
    return 0;
  std::string refreshError;
  RefreshInProgressRecordingManifest(/*force=*/false, refreshError);
  return m_inProgressRecordingStream.totalDurationMs;
}

void DispatcharrClient::CloseInProgressRecordingStream()
{
  if (m_inProgressRecordingStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_inProgressRecordingStream.curl));
  m_inProgressRecordingStream = InProgressRecordingStreamState();
}

bool DispatcharrClient::IsInProgressRecordingStreamOpen() const
{
  return m_inProgressRecordingStream.open;
}

bool DispatcharrClient::IsLiveTimeshiftStreamOpen() const
{
  return m_liveTimeshiftStream.open;
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
    std::string apiKey = GetApiKey();
    if (!apiKey.empty())
    {
      apiKeyHeader = "X-API-Key: " + apiKey;
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
    std::string apiKey = GetApiKey();
    if (!apiKey.empty())
    {
      apiKeyHeader = "X-API-Key: " + apiKey;
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

bool DispatcharrClient::RefreshLiveManifest(bool force, std::string& error, bool* fatalOut)
{
  if (fatalOut)
    *fatalOut = false;

  if (!m_liveTimeshiftStream.open)
  {
    error = "no live timeshift stream is open";
    return false;
  }

  // Throttle: ReadLiveTimeshiftStream()'s catch-up-to-the-tail loop (and a
  // tight demux-read loop calling GetLiveTimeshiftStreamLength() between
  // reads) can end up calling this far more often than the buffer could
  // possibly have grown -- segment_seconds is typically several real
  // seconds, so refetching more than a couple of times a second just adds
  // load without finding anything new.
  constexpr auto kMinRefreshInterval = std::chrono::milliseconds(500);
  auto now = std::chrono::steady_clock::now();
  if (!force && m_liveTimeshiftStream.lastManifestFetch.time_since_epoch().count() != 0 &&
      now - m_liveTimeshiftStream.lastManifestFetch < kMinRefreshInterval)
    return true;

  if (!EnsureAuthenticated(error))
    return false;

  json body = {
      {"action", "get_live_manifest"},
      {"params", {{"channel_uuid", m_liveTimeshiftStream.channelUuid}}},
  };
  json response;
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error))
    return false;
  if (!FieldOr(response, "success", false))
  {
    error = FieldOr<std::string>(response, "error", "timeshift_buffer plugin call did not succeed");
    return false;
  }
  const json& result = response.contains("result") ? response["result"] : json();
  if (FieldOr<std::string>(result, "status", "") != "ok")
  {
    error = FieldOr<std::string>(result, "message", "timeshift_buffer plugin returned an error");
    if (fatalOut)
      *fatalOut = FieldOr(result, "fatal", false);
    return false;
  }

  int httpPort = FieldOr(result, "http_port", 0);
  std::string routePrefix = FieldOr<std::string>(result, "segment_route_prefix", "");
  if (httpPort <= 0 || routePrefix.empty() || !result.contains("segments") ||
      !result["segments"].is_array())
  {
    error = "timeshift_buffer plugin manifest response was missing required fields";
    return false;
  }
  m_liveTimeshiftStream.segmentBaseUrl =
      "http://" + m_config.host + ":" + std::to_string(httpPort) + routePrefix;

  // Segments come back ordered by sequence; only ones newer than what we
  // already know get appended, each extending OUR cumulative address space
  // (not reusing the response's own relative offsets -- see this method's
  // header comment for why). An already-known segment's size can't
  // legitimately change (ffmpeg only ever appends new, closed segments to
  // the playlist), so silently skipping it here rather than re-verifying
  // it is safe, not just an optimization.
  int64_t lastKnownSequence =
      m_liveTimeshiftStream.segments.empty() ? -1 : m_liveTimeshiftStream.segments.back().sequence;

  for (const json& seg : result["segments"])
  {
    int64_t sequence = FieldOr<int64_t>(seg, "sequence", -1);
    if (sequence < 0 || sequence <= lastKnownSequence)
      continue;

    LiveTimeshiftSegmentInfo info;
    info.filename = FieldOr<std::string>(seg, "filename", "");
    info.byteSize = FieldOr<int64_t>(seg, "byte_size", 0);
    int64_t durationMs = FieldOr<int64_t>(seg, "duration_ms", 0);
    if (info.filename.empty() || info.byteSize <= 0)
      continue; // malformed entry -- don't let it corrupt the cumulative offsets that follow

    info.sequence = sequence;
    info.byteOffset = m_liveTimeshiftStream.totalBytes;
    info.timeOffsetMs = m_liveTimeshiftStream.totalDurationMs;
    m_liveTimeshiftStream.totalBytes += info.byteSize;
    m_liveTimeshiftStream.totalDurationMs += durationMs;
    m_liveTimeshiftStream.segments.push_back(std::move(info));
  }

  m_liveTimeshiftStream.lastManifestFetch = now;
  return true;
}

bool DispatcharrClient::OpenLiveTimeshiftStream(const std::string& channelUuid, std::string& error)
{
  // This addon's own local bookkeeping always starts fresh on Open() --
  // the merge-by-sequence-number logic in RefreshLiveManifest() below
  // repopulates it from whatever the plugin's buffer currently holds,
  // which is the *server-side* buffer's history, not this addon's own.
  // (See the trim step further down: repopulating from everything the
  // buffer currently holds is an intermediate state, not the final local
  // address space this method leaves in place.)
  m_liveTimeshiftStream = LiveTimeshiftStreamState();

  // Deliberately does NOT stop a buffer already running for this channel
  // before starting/reattaching -- see docs/TIMESHIFT.md's "Concurrent
  // viewers" section for the real, live-confirmed bug this used to cause:
  // a second viewer's Open() killed the first viewer's still-playing
  // buffer outright, and (via CloseLiveTimeshiftStream()'s own former
  // symmetric stop-on-close) the first viewer's eventual Close() then
  // killed the second viewer's fresh replacement too -- a cascading
  // failure that left *both* viewers broken, not just the first.
  //
  // This addon previously stopped-then-restarted the buffer on every
  // Open() specifically to fix a *different*, also real bug: seeking
  // after a Stop/reopen of the same channel used to silently fall back to
  // byte 0 instead of the requested target (see docs/TIMESHIFT.md's
  // "Fixed: seeking after a Stop/reopen didn't land on target"). Simply
  // no longer stopping the buffer here was **tried and confirmed live to
  // reintroduce that exact regression**: reattaching to a buffer that had
  // kept running (never restarted) since an earlier session, then seeking,
  // reproduced the identical failure signature (`CDVDDemuxFFmpeg::SeekTime`
  // landing on a garbage time near the MPEG-TS 33-bit PTS wraparound point)
  // from a plain -20s relative seek. Kodi's own demuxer genuinely can't
  // reliably seek backward into buffer content *this* demuxer instance
  // hasn't itself read forward through this session -- true regardless of
  // whether that content is part of one continuous, never-restarted
  // encoder run, which rules out "just don't restart the buffer" as a
  // complete fix on its own. The trim step below (discarding everything
  // except a small trailing window before this method returns) is what
  // actually restores correct seeking while still not touching the
  // server-side buffer -- see its own comment for the full reasoning and
  // what this means for the "join a running buffer and rewind into its
  // pre-join history" feature this was investigated alongside (not
  // achievable, confirmed by the same test).
  //
  // Buffer cleanup when a viewer stops watching is now the plugin's own
  // job via viewer reference counting, not something Open()/Close() force
  // by unconditionally stopping the buffer (see LiveTimeshiftStreamState::
  // viewerId's own comment and StopTimeshiftBuffer()'s for the mechanism).
  // A fresh viewer_id here registers this Open() as one of the buffer's
  // viewers; CloseLiveTimeshiftStream() deregisters it, and the plugin
  // only actually stops the buffer once no registered viewers remain --
  // safe with any number of concurrent viewers, and fast (no need to wait
  // out the plugin's own heartbeat idle-timeout, which stays only as a
  // backstop for a viewer that disappears without cleanly closing, e.g. a
  // crash). See docs/TIMESHIFT.md's "Concurrent viewers" section: an
  // earlier version of this fix relied on that idle-timeout alone for all
  // cleanup, which turned out to be a real problem of its own -- a
  // provider's own concurrent-stream limit stayed exhausted by an
  // orphaned buffer for up to that timeout's duration after its only real
  // viewer stopped, blocking a *different* channel from opening at all in
  // the meantime.
  m_liveTimeshiftStream.viewerId = GenerateViewerId();

  std::string unusedPlaylistUrl;
  if (!StartTimeshiftBuffer(channelUuid, unusedPlaylistUrl, error))
    return false;

  m_liveTimeshiftStream.open = true;
  m_liveTimeshiftStream.channelUuid = channelUuid;

  // Cold-start grace period: RefreshLiveManifest() can legitimately fail
  // here with "live playlist not found" for several real seconds after
  // StartTimeshiftBuffer() returns -- ffmpeg needs to connect to
  // Dispatcharr's live proxy and produce a full first segment
  // (segment_seconds, 6s by default) before there's anything to report.
  // Only a genuinely first-ever start of a channel's buffer (no earlier
  // viewer already has one running) hits this at all now that Open() no
  // longer force-restarts an existing buffer -- reattaching to one already
  // producing segments succeeds on this loop's very first attempt, same as
  // before this addon ever force-restarted anything. Kept from an earlier
  // version of this addon that *did* force-restart on every Open() (where
  // every single reopen paid this cold-start cost, and a failed attempt's
  // own since-removed restart-on-retry made a slow cold start actively
  // worse by repeatedly killing the previous attempt moments before it
  // would have finished) -- retry for a real cold start's worth of time
  // instead of failing on the first check.
  // fatal (as opposed to a plain retry-worthy failure) means the plugin
  // has confirmed ffmpeg already exited and this buffer will never
  // produce a segment on its own -- most commonly an upstream provider's
  // own concurrent-stream limit refusing the connection, confirmed live
  // via a genuine 4-buffers-at-a-provider's-3-stream-cap test. Breaking
  // out immediately here, instead of burning the full retry budget below
  // (~15s) against something that can't recover, is what actually fixes
  // the slow, unclear failure that test surfaced: without this, a
  // brand-new channel open that happens to be genuinely at the provider's
  // limit (not a race with something about to free up, which
  // CloseLiveTimeshiftStream()'s own synchronous stop already handles --
  // this is the "nothing is closing, the limit is just standing" case)
  // still took the full ~15s to fail instead of a couple hundred
  // milliseconds, with no indication in the error of why.
  constexpr int kColdStartMaxAttempts = 30;
  constexpr int kColdStartSleepMs = 500;
  bool manifestReady = false;
  for (int attempt = 0; attempt < kColdStartMaxAttempts; ++attempt)
  {
    bool fatal = false;
    if (RefreshLiveManifest(/*force=*/true, error, &fatal))
    {
      manifestReady = true;
      break;
    }
    if (fatal)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kColdStartSleepMs));
  }
  if (!manifestReady)
  {
    m_liveTimeshiftStream = LiveTimeshiftStreamState();
    return false;
  }

  // Discard everything except the trailing kLiveEdgeMarginSegments worth of
  // segments from what the cold-start fetch above just returned, rebasing
  // the kept ones so the oldest of them becomes local byte 0 -- **confirmed
  // live** this is necessary even though the buffer above was never
  // stopped/restarted: reattaching to a channel whose buffer had been
  // running a while (this addon's own local state spanning everything the
  // plugin still had, tens of MB/several minutes) and then seeking into the
  // *older* part of that history reproduced the exact failure this addon's
  // Stop/reopen seeking fix (elsewhere in this file) was written to
  // prevent -- `CDVDDemuxFFmpeg::SeekTime` landing on a garbage time near
  // the MPEG-TS 33-bit PTS wraparound point (~26.5h) instead of anywhere
  // near the requested target, from a plain -20s relative seek, no
  // multi-minute rewind involved. Kodi's own demuxer, it turns out, still
  // can't reliably seek backward into buffer content *this* demuxer
  // instance hasn't itself read forward through this session, regardless of
  // whether that content is part of one continuous, never-restarted
  // encoder run -- continuity alone doesn't fix it, only trimming this
  // addon's own exposed address space down to what a genuinely fresh
  // session would have does. See docs/TIMESHIFT.md's "Concurrent viewers"
  // section for the full account, including why this also answers (in the
  // negative) whether a viewer can join an already-running buffer and
  // rewind into history from before they joined.
  constexpr size_t kLiveEdgeMarginSegments = 3;
  if (m_liveTimeshiftStream.segments.size() > kLiveEdgeMarginSegments)
  {
    size_t dropCount = m_liveTimeshiftStream.segments.size() - kLiveEdgeMarginSegments;
    int64_t byteBase = m_liveTimeshiftStream.segments[dropCount].byteOffset;
    int64_t timeBase = m_liveTimeshiftStream.segments[dropCount].timeOffsetMs;
    m_liveTimeshiftStream.segments.erase(m_liveTimeshiftStream.segments.begin(),
                                          m_liveTimeshiftStream.segments.begin() + dropCount);
    for (auto& seg : m_liveTimeshiftStream.segments)
    {
      seg.byteOffset -= byteBase;
      seg.timeOffsetMs -= timeBase;
    }
    m_liveTimeshiftStream.totalBytes -= byteBase;
    m_liveTimeshiftStream.totalDurationMs -= timeBase;
  }

  // Start near the live edge, not the earliest content still known about in
  // the (now-trimmed) address space above -- position defaults to 0, which
  // without this would replay from the start of that trimmed window every
  // time a channel is (re)opened, rather than resuming at "now" the way a
  // plain live feed does. The OSD's rewind range reaches back to this
  // session's own local byte 0 (the trim above, not the buffer's true
  // beginning) via GetStreamTimes()/SeekLiveStream(); this only changes
  // where playback starts.
  //
  // Deliberately a few segments *behind* totalBytes, not exactly at it:
  // ffmpeg only exposes a segment once it's fully closed (segment_seconds
  // apart, 6s by default), so sitting exactly at the tail means there's
  // nothing to read until the next segment closes -- confirmed live, this
  // produced a periodic "stream stalled"/rebuffer cycle in Kodi tracking
  // segment_seconds almost exactly, independent of channel bitrate (a
  // fresh/small buffer has ~zero margin regardless of which channel it
  // is, which is what actually explained the earlier channel-to-channel
  // difference -- not a throughput problem, as first suspected). A few
  // segments' cushion gives the demuxer's own read-ahead something to
  // draw on between segment arrivals, while staying clearly "live" to the
  // viewer -- comparable to the inherent latency any real live-TV/DVR
  // service already has. Falls back to the true tail (0 margin) if fewer
  // than that many segments exist yet, e.g. right after a cold
  // StartTimeshiftBuffer() -- nothing to back up from yet in that case.
  // (After the trim above, segments.size() here is always <=
  // kLiveEdgeMarginSegments, so this always lands on the trimmed window's
  // own start -- kept in this same "few segments behind the tail" shape
  // rather than simplified to a flat 0, since a genuinely cold buffer with
  // fewer than kLiveEdgeMarginSegments segments still needs the tail
  // fallback below.)
  size_t segmentCount = m_liveTimeshiftStream.segments.size();
  size_t marginIndex =
      segmentCount > kLiveEdgeMarginSegments ? segmentCount - kLiveEdgeMarginSegments : 0;
  m_liveTimeshiftStream.position = marginIndex < segmentCount
                                        ? m_liveTimeshiftStream.segments[marginIndex].byteOffset
                                        : m_liveTimeshiftStream.totalBytes;
  return true;
}

int DispatcharrClient::ReadLiveTimeshiftStream(uint8_t* buffer, unsigned int size)
{
  if (!m_liveTimeshiftStream.open || size == 0)
    return 0;

  // Caught up to the tail: give the buffer a bounded chance to grow rather
  // than reporting EOF immediately, which Kodi would read as "this live
  // stream just ended". A fixed 8 attempts * 250ms (2s total) here used to
  // be *far* short of the real gap between segments (confirmed live via
  // timing instrumentation: with the plugin's segment_seconds default of
  // 6s, this loop gave up almost every time, Kodi immediately retried the
  // read, landed right back in this same loop, and repeated -- 2-4 full
  // "gave up" cycles before a new segment actually existed was common,
  // turning what should be one ~6s wait into 12-16+ seconds. That's not
  // just wasted time during ordinary near-live playback (absorbed by
  // Kodi's own read-ahead cache most of the time, so not usually a visible
  // stall) -- it also directly padded out seek latency, since ffmpeg's own
  // internal seek probing routinely lands at/near the live edge for any
  // seek originating near "now", and each such probe paid this same cost.
  // Size the budget off the last known segment's own duration (with
  // margin) instead of a fixed guess, so it comfortably covers one real
  // gap between segments regardless of how segment_seconds is configured.
  if (m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes)
  {
    constexpr int kCatchUpSleepMs = 250;

    // A read landing at the tail shortly after a seek is far more likely
    // to be one of ffmpeg's own internal probes (its generic mpegts seek
    // does a real multi-step search, confirmed live via SeekLiveTimeshiftStream
    // tracing -- several probes in quick succession, one of which commonly
    // overshoots right up to the current tail while estimating) than a
    // genuine "caught up to live, please wait" read. Blocking a probe for
    // a full segment interval was the single largest contributor to seek
    // latency measured live (a 4.2s wait out of one seek's total ~4.4s).
    // ffmpeg can usually just try an earlier candidate instead of getting
    // this exact byte -- so give it a quick "not there" rather than making
    // it wait, and reserve the full segment-duration budget below for
    // reads that aren't part of an active seek's own probing.
    // The time window alone isn't quite enough: normal decode reads that
    // happen to land at the tail right after a seek completes (not part of
    // its internal probing at all, just where playback settled) also fall
    // inside it and genuinely need the full wait -- confirmed live giving
    // those the short budget too caused visible playback pauses right
    // after a seek that landed near live. If we already gave up quickly at
    // this *exact* position, it's not a fresh probe candidate anymore --
    // escalate to the full budget rather than repeating the short one
    // indefinitely against the same stuck position.
    constexpr auto kSeekProbeWindow = std::chrono::milliseconds(800);
    bool sameAsLastShortGiveUp =
        m_liveTimeshiftStream.position == m_liveTimeshiftStream.lastShortGiveUpPosition;
    bool likelySeekProbe =
        !sameAsLastShortGiveUp && m_liveTimeshiftStream.lastSeekTime.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - m_liveTimeshiftStream.lastSeekTime < kSeekProbeWindow;

    // Averaged over the last few segments, not just the single most recent
    // one: confirmed live that a lone segment's own duration is too noisy
    // a sample to size a retry budget off of. ffmpeg's segment cutter
    // targets segment_seconds but isn't exact (it cuts at the next
    // keyframe at/after the target, so real durations vary run to run) --
    // a real instance produced one segment just 151ms long against a 2s
    // target. With the single-segment approach that collapsed this budget
    // to 2 attempts / 0.5s, nowhere near enough margin: it gave up for
    // real, repeatedly, until ffmpeg's own demuxer read that as genuine
    // end-of-stream and closed playback outright -- confirmed live, not
    // theoretical (VideoPlayer: eof, waiting for queues to empty,
    // immediately followed by Kodi kicking back to the main menu).
    // Averaging the last few segments smooths out exactly this kind of
    // one-off outlier while still adapting to whatever segment_seconds is
    // actually configured.
    constexpr size_t kSegmentDurationSampleCount = 5;
    int64_t avgSegmentDurationMs = 6000;
    {
      const auto& segs = m_liveTimeshiftStream.segments;
      size_t sampleCount = std::min(kSegmentDurationSampleCount, segs.size());
      if (sampleCount > 0)
      {
        int64_t sumMs = m_liveTimeshiftStream.totalDurationMs -
                         segs[segs.size() - sampleCount].timeOffsetMs;
        if (sumMs > 0)
          avgSegmentDurationMs = sumMs / static_cast<int64_t>(sampleCount);
      }
    }
    // Defense in depth on top of the averaging above: never let a budget
    // this consequential (getting it wrong ends live playback outright,
    // not just a slower catch-up) shrink below a sane floor, regardless of
    // what the segments so far report -- covers a fresh buffer's first few,
    // still-warming-up segments too, not just an established stream's rare
    // outlier.
    constexpr int64_t kMinSegmentDurationMs = 1500;
    int64_t segmentDurationEstimateMs = std::max(avgSegmentDurationMs, kMinSegmentDurationMs);

    // 3x, not the 1.5x this originally used: confirmed live against a real
    // instance that 1.5x runs far thinner than intended in practice --
    // ordinary, non-error catch-up cycles were routinely using 60-95% of
    // that budget just to catch up under normal jitter (network latency to
    // the plugin's server, its own manifest-refresh request time eating
    // into the sleep-based budget, real variance in when the server
    // actually finishes a segment), not just in a genuine outage. That
    // left too little real margin before "gave up" -- confirmed live to
    // fully exhaust the 1.5x budget and report a genuine stalled read to
    // Kodi. A separate server-side bug (see the timeshift_buffer plugin's
    // own history) was the dominant cause of the *worst*, multi-second
    // stalls, but this margin was measurably too tight even independent of
    // that.
    int catchUpAttempts =
        likelySeekProbe
            ? 1
            : static_cast<int>((segmentDurationEstimateMs * 3) / kCatchUpSleepMs) + 1;

    auto catchUpStart = std::chrono::steady_clock::now();
    int attemptsUsed = 0;
    for (int attempt = 0; attempt < catchUpAttempts &&
                           m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes;
         ++attempt)
    {
      attemptsUsed = attempt + 1;
      std::string refreshError;
      RefreshLiveManifest(/*force=*/true, refreshError); // best-effort; try again next attempt/call on failure
      if (m_liveTimeshiftStream.position < m_liveTimeshiftStream.totalBytes)
        break;
      // Don't sleep after the last attempt -- nothing left to wait for
      // before giving up, and for a likely seek probe (catchUpAttempts==1)
      // this is what makes the "not there yet" response fast instead of
      // paying a pointless 250ms before reporting it.
      if (attempt + 1 < catchUpAttempts)
        std::this_thread::sleep_for(std::chrono::milliseconds(kCatchUpSleepMs));
    }
    bool caughtUp = m_liveTimeshiftStream.position < m_liveTimeshiftStream.totalBytes;
    m_liveTimeshiftStream.lastShortGiveUpPosition =
        (likelySeekProbe && !caughtUp) ? m_liveTimeshiftStream.position : -1;

    double elapsedSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - catchUpStart).count();
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharrai: ReadLiveTimeshiftStream: catch-up-to-tail loop used %d/%d "
              "attempts, %.3fs (budget %.1fs off ~%lldms/segment estimate), position=%lld "
              "totalBytes=%lld -> %s",
              attemptsUsed, catchUpAttempts, elapsedSec, catchUpAttempts * kCatchUpSleepMs / 1000.0,
              static_cast<long long>(segmentDurationEstimateMs),
              static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(m_liveTimeshiftStream.totalBytes),
              caughtUp ? "caught up" : "gave up");
  }
  if (m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes)
    return 0; // genuinely nothing new yet

  const LiveTimeshiftSegmentInfo* seg = nullptr;
  for (const auto& s : m_liveTimeshiftStream.segments)
  {
    if (m_liveTimeshiftStream.position >= s.byteOffset &&
        m_liveTimeshiftStream.position < s.byteOffset + s.byteSize)
    {
      seg = &s;
      break;
    }
  }
  if (!seg)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharrai: ReadLiveTimeshiftStream: position=%lld is a gap (totalBytes=%lld, "
              "segments=%zu, first seg byteOffset=%lld, last seg end=%lld)",
              static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(m_liveTimeshiftStream.totalBytes),
              m_liveTimeshiftStream.segments.size(),
              m_liveTimeshiftStream.segments.empty()
                  ? -1LL
                  : static_cast<long long>(m_liveTimeshiftStream.segments.front().byteOffset),
              m_liveTimeshiftStream.segments.empty()
                  ? -1LL
                  : static_cast<long long>(m_liveTimeshiftStream.segments.back().byteOffset +
                                            m_liveTimeshiftStream.segments.back().byteSize));
    return 0; // position points into a gap/rolled-off region -- nothing safely readable here
  }

  int64_t offsetInSegment = m_liveTimeshiftStream.position - seg->byteOffset;
  int64_t available = seg->byteSize - offsetInSegment;
  unsigned int wantSize = static_cast<unsigned int>(std::min<int64_t>(size, available));

  int64_t rangeEnd = offsetInSegment + static_cast<int64_t>(wantSize) - 1;
  std::string range = std::to_string(offsetInSegment) + "-" + std::to_string(rangeEnd);
  std::string url = m_liveTimeshiftStream.segmentBaseUrl + seg->filename;

  CURL* curl = static_cast<CURL*>(m_liveTimeshiftStream.curl);
  if (!curl)
  {
    curl = curl_easy_init();
    if (!curl)
      return -1;
    m_liveTimeshiftStream.curl = curl;
  }

  // No auth header: the plugin's own file server is deliberately
  // unauthenticated (see plugin.py's module docstring).
  FixedBufferSink sink{buffer, wantSize, 0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(GetCurlShare()));

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  if (res != CURLE_OK)
  {
    // Same "reused connection went stale" handling as ReadRecordingStream().
    curl_easy_cleanup(curl);
    m_liveTimeshiftStream.curl = nullptr;
    return -1;
  }
  if (httpCode == 404)
  {
    // Segment got recycled between our manifest fetch and this read -- a
    // real, expected race for a rolling buffer (the same one plugin.py's
    // own do_GET/_create_snapshot already tolerate). Treat as "nothing
    // readable here" rather than a hard error.
    return 0;
  }
  if (httpCode != 200 && httpCode != 206)
    return -1;

  m_liveTimeshiftStream.position += static_cast<int64_t>(sink.written);
  return static_cast<int>(sink.written);
}

int64_t DispatcharrClient::SeekLiveTimeshiftStream(int64_t position, int whence)
{
  if (!m_liveTimeshiftStream.open)
    return -1;

  m_liveTimeshiftStream.lastSeekTime = std::chrono::steady_clock::now();

  if (whence == SEEK_END)
  {
    // "End" for a growing stream means the current known tail -- refresh
    // first so a seek-to-live lands as close to the real live edge as
    // possible rather than wherever we last happened to know about.
    std::string refreshError;
    RefreshLiveManifest(/*force=*/true, refreshError);
  }

  int64_t newPos;
  switch (whence)
  {
    case SEEK_SET:
      newPos = position;
      break;
    case SEEK_CUR:
      newPos = m_liveTimeshiftStream.position + position;
      break;
    case SEEK_END:
      newPos = m_liveTimeshiftStream.totalBytes + position;
      break;
    default:
      return -1;
  }
  bool clampedNegative = newPos < 0;
  if (clampedNegative)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharrai: SeekLiveTimeshiftStream(position=%lld, whence=%d) from "
              "current=%lld -> computed newPos=%lld < 0, failing",
              static_cast<long long>(position), whence,
              static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(newPos));
    return -1;
  }
  // Clamp forward seeks to the known tail -- there's nothing to seek ahead
  // of yet for a genuinely live buffer. Deliberately backed off by roughly
  // one segment's worth of bytes rather than landing exactly on the
  // absolute tail: confirmed live that landing precisely at totalBytes
  // leaves zero read-ahead margin, so playback resumes, immediately
  // re-catches-up to the (still-)tail within a couple of seconds of real
  // playback, and has to wait through a full segment-production cycle a
  // second time -- long enough (up to one whole segment interval, ~5-7.5s
  // in this instance) to exceed Kodi's own stall tolerance and trigger a
  // visible rebuffer right after what looked like a completed seek. One
  // segment of backoff means at least that much is already available to
  // play immediately, the same "live edge minus a little" margin real-world
  // live players (HLS, DASH) keep for exactly this reason -- imperceptibly
  // behind true live, but enough to absorb normal segment-to-segment
  // timing jitter instead of stuttering on essentially every seek-to-live.
  int64_t liveBackoffBytes = m_liveTimeshiftStream.segments.empty()
                                 ? 0
                                 : m_liveTimeshiftStream.segments.back().byteSize;
  int64_t tailTarget = std::max<int64_t>(0, m_liveTimeshiftStream.totalBytes - liveBackoffBytes);
  bool clampedToTail = newPos > tailTarget;
  if (clampedToTail)
    newPos = tailTarget;

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharrai: SeekLiveTimeshiftStream(position=%lld, whence=%d) from "
            "current=%lld, totalBytes=%lld -> newPos=%lld%s",
            static_cast<long long>(position), whence,
            static_cast<long long>(m_liveTimeshiftStream.position),
            static_cast<long long>(m_liveTimeshiftStream.totalBytes),
            static_cast<long long>(newPos), clampedToTail ? " (clamped to tail)" : "");

  m_liveTimeshiftStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetLiveTimeshiftStreamLength()
{
  if (!m_liveTimeshiftStream.open)
    return -1;
  std::string refreshError;
  RefreshLiveManifest(/*force=*/false, refreshError); // throttled, cheap to call often
  return m_liveTimeshiftStream.totalBytes;
}

int64_t DispatcharrClient::GetLiveTimeshiftStreamDurationMs()
{
  if (!m_liveTimeshiftStream.open)
    return 0;
  std::string refreshError;
  RefreshLiveManifest(/*force=*/false, refreshError);
  return m_liveTimeshiftStream.totalDurationMs;
}

void DispatcharrClient::CloseLiveTimeshiftStream()
{
  // Tells the plugin this viewer is done, via StopTimeshiftBuffer() --
  // NOT the unconditional "kill the buffer" it used to be. Two earlier
  // designs were both tried and confirmed live to be real bugs, not just
  // theoretical concerns (see docs/TIMESHIFT.md's "Concurrent viewers"
  // section for the full account of both):
  //
  // 1. Unconditionally stopping the buffer here (the original design):
  //    this Close() had no way to know whether *another* viewer was still
  //    actively reading the same buffer, so it could (and did) kill a
  //    second viewer's playback that had only just started.
  // 2. Not stopping anything at all here, relying purely on the plugin's
  //    own heartbeat-driven idle-timeout reaper to eventually notice no
  //    one's fetching anymore (2 minutes by default): safe with multiple
  //    viewers, but left a since-abandoned buffer occupying one of a
  //    provider's own concurrent-stream slots for up to that timeout --
  //    confirmed live as a real problem, not just a slow cleanup: with a
  //    3-stream provider limit, 2 recordings in progress, and a live
  //    channel watched then stopped and immediately switched to a
  //    *different* channel, the new channel failed to start at all
  //    (Dispatcharr still showed the old channel's now-abandoned buffer
  //    as the active 3rd stream) until that timeout finally elapsed.
  //
  // The fix: the plugin now reference-counts viewers per buffer
  // (registered by this viewer's own viewer_id at Open()/
  // StartTimeshiftBuffer() time -- see LiveTimeshiftStreamState::viewerId's
  // own comment). This call deregisters just this viewer; the plugin only
  // actually stops the underlying ffmpeg process once no registered
  // viewers remain, so it's safe to call unconditionally on every Close()
  // regardless of how many other viewers exist, and fast when this really
  // was the last one -- no need to wait out the idle-timeout reaper, which
  // remains only as a backstop for a viewer that disappears without
  // cleanly closing (a crash, a network drop).
  //
  // Deliberately SYNCHRONOUS, not a detached background thread (an earlier
  // version of this fix used one, matching how this call worked before
  // reference counting existed) -- confirmed live this was itself a real
  // bug: switching channels (Kodi calls this Close() then OpenLiveStream()
  // for the new channel, back to back, without waiting on anything of this
  // addon's own) raced a detached call here against the new channel's own
  // Open()/StartTimeshiftBuffer(). With a provider's own concurrent-stream
  // limit already fully used (2 recordings + the channel being switched
  // away from), the new channel's own upstream connection attempt reached
  // Dispatcharr *before* this detached call had actually freed the old
  // channel's slot -- confirmed live: the old channel's stream visibly
  // went down in Dispatcharr's own status a few seconds *after* the new
  // channel had already failed to start and Kodi had already given up and
  // returned to the main menu, not before. `_stop_ffmpeg()` in plugin.py
  // (the thing that actually happens once this call determines no viewers
  // remain) already blocks until the ffmpeg process is confirmed dead
  // (SIGTERM, poll, escalate to SIGKILL after 2s) before its own HTTP
  // response returns -- so calling it synchronously here, and letting
  // Kodi's own sequential Close()-then-Open() calling convention do the
  // rest, is what actually guarantees the old slot is free before the new
  // channel's own Open() ever asks the provider for one. Kodi's calling
  // thread already blocks synchronously on comparable network I/O
  // elsewhere in this same class (every live-timeshift read/seek/manifest
  // call), so this isn't a new category of blocking for it, just this one
  // call site catching up to that same pattern.
  if (m_liveTimeshiftStream.open && !m_liveTimeshiftStream.channelUuid.empty())
  {
    std::string stopError;
    StopTimeshiftBuffer(m_liveTimeshiftStream.channelUuid, m_liveTimeshiftStream.viewerId, stopError);
  }

  if (m_liveTimeshiftStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_liveTimeshiftStream.curl));
  m_liveTimeshiftStream = LiveTimeshiftStreamState();
}

} // namespace dispatcharr
