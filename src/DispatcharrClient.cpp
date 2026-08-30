#include "DispatcharrClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
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
constexpr const char* kChannelGroupsPath = "/api/channels/channel-groups/"; // assumed
constexpr const char* kEpgOutputPath = "/output/epg";
constexpr const char* kRecordingsPath = "/api/channels/recordings/";       // assumed CRUD base
constexpr const char* kSeriesRulesPath = "/api/channels/series-rules/";   // confirmed to exist
constexpr const char* kLogosPath = "/api/channels/logos/";

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

} // namespace

DispatcharrClient::DispatcharrClient(Config config) : m_config(std::move(config)) {}

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

  m_accessToken = response.value("access", "");
  m_refreshToken = response.value("refresh", m_refreshToken);
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
  m_accessToken = response.value("access", "");
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
    ch.id = item.value("id", 0);
    // Field name assumed as "uuid" (confirmed to exist on channels in some
    // form -- it's what appears in /proxy/ts/stream/{uuid} URLs -- but the
    // exact JSON key wasn't independently verified).
    ch.uuid = item.value("uuid", "");
    ch.name = item.value("name", "");
    ch.channelNumber = item.value("channel_number", item.value("channel_num", 0));
    ch.logoUrl = item.value("logo_url", "");
    // Channel group may be a nested object or a bare id depending on the
    // serializer; handle both.
    if (item.contains("channel_group") && item["channel_group"].is_object())
    {
      ch.groupId = item["channel_group"].value("id", -1);
      ch.groupName = item["channel_group"].value("name", "");
    }
    else
    {
      ch.groupId = item.value("channel_group", item.value("channel_group_id", -1));
    }
    // EPG linkage: try a nested epg_data object first, then a flat field.
    if (item.contains("epg_data") && item["epg_data"].is_object())
      ch.tvgId = item["epg_data"].value("tvg_id", "");
    else
      ch.tvgId = item.value("tvg_id", "");

    out.push_back(std::move(ch));
  }
  return true;
}

bool DispatcharrClient::GetChannelGroups(std::vector<ChannelGroup>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kChannelGroupsPath, json(), response, error))
    return false;

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
    g.id = item.value("id", 0);
    g.name = item.value("name", "");
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
  return BaseUrl() + "/proxy/ts/stream/" + channel.uuid;
}

std::string DispatcharrClient::GetChannelLogoUrl(int channelId) const
{
  return BaseUrl() + kLogosPath + std::to_string(channelId) + "/cache/";
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

  out.clear();
  for (const auto& item : list)
  {
    Recording r;
    r.id = item.value("id", 0);
    r.title = item.value("title", item.value("name", ""));
    r.subtitle = item.value("subtitle", "");
    r.description = item.value("description", item.value("summary", ""));
    r.channelId = item.value("channel", item.value("channel_id", 0));
    r.isInProgress = item.value("in_progress", item.value("is_recording", false));
    // start_time is assumed ISO-8601; a full implementation should parse
    // this properly (e.g. with a small strptime wrapper) rather than
    // leaving it at 0 -- left as a follow-up, see docs/API_NOTES.md.
    r.startTime = 0;
    r.durationSeconds = item.value("duration", 0);
    out.push_back(std::move(r));
  }
  return true;
}

std::string DispatcharrClient::GetRecordingStreamUrl(int recordingId) const
{
  return BaseUrl() + kRecordingsPath + std::to_string(recordingId) + "/file/";
}

bool DispatcharrClient::DeleteRecording(int recordingId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/";
  return Request("DELETE", path, json(), response, error);
}

bool DispatcharrClient::GetTimerRules(std::vector<TimerRule>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kSeriesRulesPath, json(), response, error))
    return false;

  const json& list = response.contains("results") ? response["results"] : response;
  if (!list.is_array())
  {
    error = "Unexpected series-rules response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
  {
    TimerRule t;
    t.id = item.value("id", 0);
    t.channelId = item.value("channel", item.value("channel_id", 0));
    t.tvgId = item.value("tvg_id", "");
    t.title = item.value("title", item.value("title_pattern", ""));
    t.titlePattern = item.value("title_pattern", "");
    t.isSeries = true;
    out.push_back(std::move(t));
  }
  return true;
}

bool DispatcharrClient::CreateOneTimeRecording(
    int channelId, time_t start, time_t end, const std::string& title, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Payload shape is a best-effort guess based on Django REST Framework
  // conventions and public issue reports; verify field names against your
  // instance's /swagger/ before relying on this (see docs/API_NOTES.md).
  json body = {
      {"channel", channelId},
      {"start_time", IsoFromTime(start)},
      {"end_time", IsoFromTime(end)},
      {"name", title},
  };
  json response;
  return Request("POST", kRecordingsPath, body, response, error);
}

bool DispatcharrClient::CreateSeriesRule(
    int channelId, const std::string& tvgId, const std::string& titlePattern, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json body = {
      {"channel", channelId},
      {"tvg_id", tvgId},
      {"title_pattern", titlePattern},
  };
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

bool DispatcharrClient::DeleteTimerRule(int ruleId, bool isSeries, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  const char* base = isSeries ? kSeriesRulesPath : kRecordingsPath;
  std::string path = std::string(base) + std::to_string(ruleId) + "/";
  return Request("DELETE", path, json(), response, error);
}

} // namespace dispatcharr
