#include "PVRDispatcharr.h"

#include "WebSocketClient.h"

#include <kodi/AddonBase.h>
#include <kodi/General.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <functional>
#include <thread>
#include <unordered_set>

using namespace dispatcharr;

namespace
{
constexpr int kAccessDeniedLogThrottle = 1; // placeholder for future rate-limited logging
}

dispatcharr::Config PVRDispatcharr::LoadConfigFromSettings() const
{
  Config config;
  config.host = kodi::addon::GetSettingString("host", "127.0.0.1");
  config.port = kodi::addon::GetSettingInt("port", 9191);
  config.useHttps = kodi::addon::GetSettingBoolean("use_https", false);
  config.username = kodi::addon::GetSettingString("username", "");
  config.password = kodi::addon::GetSettingString("password", "");
  config.verifySsl = kodi::addon::GetSettingBoolean("verify_ssl", true);
  config.timeoutSeconds = kodi::addon::GetSettingInt("timeout", 30);
  config.debugLogging = kodi::addon::GetSettingBoolean("debug_logging", false);
  config.channelSwitchDelaySeconds = kodi::addon::GetSettingInt("channel_switch_delay_seconds", 0);
  config.apiKey = kodi::addon::GetSettingString("api_key", "");
  return config;
}

PVRDispatcharr::PVRDispatcharr(const kodi::addon::IInstanceInfo& instance)
    : CInstancePVRClient(instance), m_client(LoadConfigFromSettings())
{
  m_channelRefreshHours = kodi::addon::GetSettingInt("channel_refresh_hours", 12);
  m_epgRefreshHours = kodi::addon::GetSettingInt("epg_refresh_hours", 4);
  m_channelSwitchDelaySeconds = kodi::addon::GetSettingInt("channel_switch_delay_seconds", 0);
  m_enableLiveTimeshift = kodi::addon::GetSettingBoolean("enable_live_timeshift", false);
  m_enableInProgressPlayback = kodi::addon::GetSettingBoolean("enable_inprogress_playback", false);
  m_recordingRefreshMinutes = kodi::addon::GetSettingInt("recording_refresh_minutes", 5);
  m_enableRealtimeUpdates = kodi::addon::GetSettingBoolean("enable_realtime_updates", false);
  m_debugLogging = kodi::addon::GetSettingBoolean("debug_logging", false);

  std::string error;
  if (!m_client.EnsureAuthenticated(error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: initial login failed: %s", error.c_str());
  }
  else if (!m_client.HasApiKey())
  {
    // Recording playback needs an API key (see GetRecordingStreamUrl()) --
    // a JWT would work too, but expires after 30 minutes, which is shorter
    // than most recordings. Generate one once and persist it so it isn't
    // silently regenerated (and any other use of this account's key
    // invalidated) on every addon restart.
    std::string key;
    if (m_client.GenerateApiKey(key, error))
      kodi::addon::SetSettingString("api_key", key);
    else
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to generate API key: %s", error.c_str());
  }

  StartRecordingRefreshThread();
  if (m_enableRealtimeUpdates)
    StartRealtimeUpdateThread();

  if (m_enableInProgressPlayback)
  {
    std::string playlistServerError;
    if (!m_playlistServer.Start(playlistServerError))
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to start local playlist server: %s",
                playlistServerError.c_str());

    // See m_pendingLiveModeRecordingId's comment: plain Play on an
    // in-progress recording goes straight to "Play from start" now, and
    // this is the only way left to get "Play live" for one.
    AddMenuHook(kodi::addon::PVRMenuhook(kMenuHookPlayLive, 30043, PVR_MENUHOOK_RECORDING));
  }
}

PVRDispatcharr::~PVRDispatcharr()
{
  {
    std::lock_guard<std::mutex> lock(m_recordingRefreshMutex);
    m_stopRecordingRefreshThread = true;
  }
  m_recordingRefreshCv.notify_all();
  if (m_recordingRefreshThread.joinable())
    m_recordingRefreshThread.join();

  {
    std::lock_guard<std::mutex> lock(m_realtimeUpdateMutex);
    m_stopRealtimeUpdateThread = true;
  }
  m_realtimeUpdateCv.notify_all();
  if (m_realtimeUpdateThread.joinable())
    m_realtimeUpdateThread.join();

  m_playlistServer.Stop();
}

void PVRDispatcharr::StartRecordingRefreshThread()
{
  m_recordingRefreshThread = std::thread([this]() {
    std::unique_lock<std::mutex> lock(m_recordingRefreshMutex);
    while (!m_stopRecordingRefreshThread)
    {
      bool stopped = m_recordingRefreshCv.wait_for(
          lock, std::chrono::minutes(m_recordingRefreshMinutes),
          [this]() { return m_stopRecordingRefreshThread.load(); });
      if (stopped)
        break;
      TriggerRecordingUpdate();
      TriggerTimerUpdate();
    }
  });
}

void PVRDispatcharr::HandleRealtimeUpdateMessage(const std::string& message)
{
  // Wire shape, confirmed by reading Dispatcharr's own consumers.py/utils.py:
  // send_websocket_update('updates', 'update', {..., "type": "<event>", ...})
  // is delivered to this socket as {"type": "update", "data": {..., "type":
  // "<event>", ...}}. Only react to the recording/timer-relevant event
  // names Dispatcharr actually sends (confirmed in apps/channels/tasks.py
  // and api_views.py) -- everything else on this shared "updates" channel
  // (EPG matching progress, M3U refresh, stream stats, ...) is irrelevant
  // here and should be silently ignored, not treated as an error.
  static const std::unordered_set<std::string> kRelevantEventTypes = {
      "recording_started",   "recording_ended",     "recording_stopped",
      "recording_extended",  "recording_updated",   "recording_cancelled",
      "recordings_refreshed"};
  try
  {
    nlohmann::json parsed = nlohmann::json::parse(message);
    if (!parsed.contains("data") || !parsed["data"].is_object())
      return;
    const nlohmann::json& data = parsed["data"];
    if (!data.contains("type") || !data["type"].is_string())
      return;
    std::string eventType = data["type"].get<std::string>();
    if (kRelevantEventTypes.count(eventType) == 0)
      return;

    if (m_debugLogging)
      kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: realtime update received: %s", eventType.c_str());
    TriggerRecordingUpdate();
    TriggerTimerUpdate();
  }
  catch (const nlohmann::json::exception&)
  {
    // Malformed/unexpected payload -- not this connection's problem to solve;
    // just ignore it and keep listening.
  }
}

void PVRDispatcharr::StartRealtimeUpdateThread()
{
  m_realtimeUpdateThread = std::thread([this]() {
    constexpr int kInitialBackoffSeconds = 2;
    constexpr int kMaxBackoffSeconds = 60;
    constexpr int kMessageReadTimeoutSeconds = 5; // bounds how quickly a stop request is noticed
    int backoffSeconds = kInitialBackoffSeconds;

    auto shouldStop = [this]() {
      std::lock_guard<std::mutex> lock(m_realtimeUpdateMutex);
      return m_stopRealtimeUpdateThread.load();
    };

    while (!shouldStop())
    {
      Config config = LoadConfigFromSettings();
      std::string token, error;
      if (m_client.GetAccessToken(token, error))
      {
        WebSocketClient ws;
        std::string pathAndQuery = "/ws/?token=" + token;
        if (ws.Connect(config.host, config.port, config.useHttps, pathAndQuery, config.verifySsl,
                       config.timeoutSeconds, error))
        {
          if (m_debugLogging)
            kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: realtime updates: connected");
          backoffSeconds = kInitialBackoffSeconds; // reset now that a connection actually worked

          while (!shouldStop())
          {
            std::string message;
            int result = ws.ReceiveTextMessage(message, kMessageReadTimeoutSeconds, error);
            if (result == 1)
              HandleRealtimeUpdateMessage(message);
            else if (result < 0)
            {
              if (m_debugLogging)
                kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: realtime updates: %s", error.c_str());
              break; // reconnect
            }
            // result == 0: just a read timeout with nothing new -- loop and
            // re-check shouldStop().
          }
          ws.Close();
        }
        else if (m_debugLogging)
        {
          kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: realtime updates: connect failed: %s",
                    error.c_str());
        }
      }
      else if (m_debugLogging)
      {
        kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: realtime updates: could not get an access token: %s",
                  error.c_str());
      }

      if (shouldStop())
        break;

      std::unique_lock<std::mutex> lock(m_realtimeUpdateMutex);
      bool stopped = m_realtimeUpdateCv.wait_for(lock, std::chrono::seconds(backoffSeconds),
                                                  [this]() { return m_stopRealtimeUpdateThread.load(); });
      if (stopped)
        break;
      backoffSeconds = std::min(backoffSeconds * 2, kMaxBackoffSeconds);
    }
  });
}

// ---------------------------------------------------------------------
// General
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  // TODO(verify): confirm exact setter names against your installed
  // <kodi/addon-instance/pvr/General.h> -- these names have been stable
  // for several Kodi PVR API generations but should be spot-checked.
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsChannelGroups(true);
  capabilities.SetSupportsRecordings(true);
  capabilities.SetSupportsRecordingsDelete(true);
  capabilities.SetSupportsTimers(true);
  capabilities.SetSupportsRecordingPlayCount(false);
  capabilities.SetSupportsRecordingEdl(false);
  capabilities.SetSupportsDescrambleInfo(false);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetBackendName(std::string& name)
{
  name = "Dispatcharr";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetBackendVersion(std::string& version)
{
  // Dispatcharr doesn't have a confirmed "server version" endpoint used
  // here; this reports the addon's own protocol expectations instead.
  version = "native-api-0.1";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetConnectionString(std::string& connection)
{
  connection = kodi::addon::GetSettingString("host", "127.0.0.1") + ":" +
               std::to_string(kodi::addon::GetSettingInt("port", 9191));
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Data loading / caching
// ---------------------------------------------------------------------

void PVRDispatcharr::EnsureChannelsLoaded()
{
  auto now = std::chrono::steady_clock::now();
  bool stale = m_channelsLoadedAt.time_since_epoch().count() == 0 ||
               now - m_channelsLoadedAt > std::chrono::hours(m_channelRefreshHours);
  if (!stale)
    return;

  std::vector<Channel> channels;
  std::vector<ChannelGroup> groups;
  std::string error;
  bool ok = m_client.GetChannels(channels, error);
  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to load channels: %s", error.c_str());
    return;
  }

  // Groups are best-effort: a channel list is still useful without them.
  std::string groupsError;
  if (!m_client.GetChannelGroups(groups, groupsError))
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to load channel groups: %s", groupsError.c_str());

  // Dispatcharr's /api/channels/groups/ returns every group that has ever
  // existed, regardless of whether it's currently enabled for any M3U
  // account -- "enabled" isn't even a property of the group itself, it's
  // per (group, account) pair, so there's no simple flag to check here.
  // Disabled groups' channels are already correctly excluded from the
  // channel list Dispatcharr just gave us, so drop any group with no
  // member channels left in it rather than trying to reconstruct
  // Dispatcharr's own enable/disable logic.
  std::unordered_set<int> groupIdsWithChannels;
  for (const auto& ch : channels)
    groupIdsWithChannels.insert(ch.groupId);
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                               [&](const ChannelGroup& g)
                               { return groupIdsWithChannels.find(g.id) == groupIdsWithChannels.end(); }),
               groups.end());

  std::lock_guard<std::mutex> lock(m_dataMutex);
  m_channels = std::move(channels);
  m_groups = std::move(groups);
  m_channelsLoadedAt = now;
}

void PVRDispatcharr::EnsureEpgLoaded()
{
  auto now = std::chrono::steady_clock::now();
  bool stale = m_epgLoadedAt.time_since_epoch().count() == 0 ||
               now - m_epgLoadedAt > std::chrono::hours(m_epgRefreshHours);
  if (!stale)
    return;

  std::string xml, error;
  if (!m_client.GetXmlTvGuide(xml, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to fetch XMLTV guide: %s", error.c_str());
    return;
  }

  std::unordered_map<std::string, std::vector<EpgEntry>> parsed;
  if (!XmlTvParser::Parse(xml, parsed, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to parse XMLTV guide: %s", error.c_str());
    return;
  }

  std::lock_guard<std::mutex> lock(m_dataMutex);
  m_epgByChannelNumber = std::move(parsed);
  m_epgLoadedAt = now;
}

const Channel* PVRDispatcharr::FindChannelByUid(int uid) const
{
  for (const auto& ch : m_channels)
  {
    if (ch.id == uid)
      return &ch;
  }
  return nullptr;
}

// ---------------------------------------------------------------------
// Channel groups
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetChannelGroupsAmount(int& amount)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  amount = static_cast<int>(m_groups.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR; // no radio support

  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  for (const auto& group : m_groups)
  {
    kodi::addon::PVRChannelGroup g;
    g.SetGroupName(group.name);
    g.SetIsRadio(false);
    results.Add(g);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                                  kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);

  int groupId = -1;
  for (const auto& g : m_groups)
  {
    if (g.name == group.GetGroupName())
    {
      groupId = g.id;
      break;
    }
  }
  if (groupId == -1)
    return PVR_ERROR_NO_ERROR;

  for (const auto& ch : m_channels)
  {
    if (ch.groupId != groupId)
      continue;
    kodi::addon::PVRChannelGroupMember member;
    member.SetGroupName(group.GetGroupName());
    member.SetChannelUniqueId(ch.id);
    member.SetChannelNumber(ch.channelNumber);
    results.Add(member);
  }
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetChannelsAmount(int& amount)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR;

  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  for (const auto& ch : m_channels)
  {
    kodi::addon::PVRChannel channel;
    channel.SetUniqueId(static_cast<unsigned int>(ch.id));
    channel.SetIsRadio(false);
    channel.SetChannelNumber(static_cast<unsigned int>(ch.channelNumber));
    channel.SetChannelName(ch.name);
    if (ch.logoId >= 0)
      channel.SetIconPath(m_client.GetChannelLogoUrl(ch.logoId));
    channel.SetIsHidden(false);
    results.Add(channel);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                                      std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  std::string streamUrl;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(channel.GetUniqueId()));
    if (!ch)
      return PVR_ERROR_INVALID_PARAMETERS;
    streamUrl = m_client.GetLiveStreamUrl(*ch);
  }

  // Off (0) by default. Added while diagnosing a "channel N+1 never plays"
  // failure on the theory that Dispatcharr's proxy needed a moment to
  // release the previous connection -- it didn't help (the real cause was
  // an unreachable IPv6 route to the Dispatcharr host, see
  // docs/API_NOTES.md), so don't expect this alone to fix that class of
  // symptom. Left available as a settings.xml option in case a genuinely
  // different setup needs it. Runs with m_dataMutex already released so it
  // doesn't block other addon calls in the meantime.
  if (m_channelSwitchDelaySeconds > 0)
    std::this_thread::sleep_for(std::chrono::seconds(m_channelSwitchDelaySeconds));

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
  // Dispatcharr's default proxy output is MPEG-TS; if you've configured an
  // HLS stream profile in Dispatcharr, override this in settings and adapt
  // GetLiveStreamUrl() accordingly.
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "video/mp2t");

  // Live pause/rewind ("timeshift"), delegated entirely to the separate
  // inputstream.ffmpegdirect addon rather than implemented here. Unlike the
  // catch-up case (see GetEPGTagStreamProperties() below for why that one
  // was reverted), this is exactly what ffmpegdirect's stream_mode:
  // timeshift is built for: a genuinely live, continuously arriving source
  // with no native pause/rewind of its own. It works independent of any
  // Dispatcharr-side support -- Dispatcharr has no concept matching
  // TVHeadend's server-side rolling live buffer, so this buffer instead
  // lives as a local recording on-disk on the Kodi device itself (managed
  // entirely by ffmpegdirect's own settings: buffer path, length limit,
  // etc.), not on the Dispatcharr server. See docs/API_NOTES.md.
  //
  // Off by default: requires inputstream.ffmpegdirect to actually be
  // installed, and unlike the catch-up case, getting this wrong here would
  // break live channel playback entirely, not just catch-up.
  if (m_enableLiveTimeshift)
  {
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
    properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
    properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "true");
  }
  if (m_debugLogging)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: GetChannelStreamProperties: returning %zu properties",
              properties.size());
    for (const auto& p : properties)
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai:   prop %s = %s", p.GetName().c_str(), p.GetValue().c_str());
  }
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// EPG
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetEPGForChannel(int channelUid,
                                            time_t start,
                                            time_t end,
                                            kodi::addon::PVREPGTagsResultSet& results)
{
  EnsureChannelsLoaded();
  EnsureEpgLoaded();

  std::lock_guard<std::mutex> lock(m_dataMutex);
  const Channel* ch = FindChannelByUid(channelUid);
  if (!ch || ch->channelNumber <= 0)
    return PVR_ERROR_NO_ERROR;

  // Confirmed against a live instance: Dispatcharr's XMLTV export keys
  // <channel id="..."> by channel_number, not tvg_id (see XmlTvParser.h).
  auto it = m_epgByChannelNumber.find(std::to_string(ch->channelNumber));
  if (it == m_epgByChannelNumber.end())
    return PVR_ERROR_NO_ERROR;

  for (const auto& entry : it->second)
  {
    if (entry.endTime < start || entry.startTime > end)
      continue;

    kodi::addon::PVREPGTag tag;
    // Broadcast ids only need to be unique per channel/addon, not globally;
    // combining channel id and start time is stable across refreshes.
    tag.SetUniqueBroadcastId(static_cast<unsigned int>((channelUid << 16) ^ (entry.startTime & 0xFFFF)));
    tag.SetUniqueChannelId(static_cast<unsigned int>(channelUid));
    tag.SetTitle(entry.title);
    tag.SetPlotOutline(entry.subtitle);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    if (!entry.genre.empty())
      tag.SetGenreDescription(entry.genre);
    if (entry.seasonNumber > 0)
      tag.SetSeriesNumber(entry.seasonNumber);
    if (entry.episodeNumber > 0)
      tag.SetEpisodeNumber(entry.episodeNumber);
    results.Add(tag);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable)
{
  isPlayable = false;
  std::lock_guard<std::mutex> lock(m_dataMutex);
  const Channel* ch = FindChannelByUid(static_cast<int>(tag.GetUniqueChannelId()));
  if (!ch || !ch->catchupEnabled || ch->catchupDays <= 0)
    return PVR_ERROR_NO_ERROR;

  time_t now = time(nullptr);
  if (tag.GetStartTime() > now)
    return PVR_ERROR_NO_ERROR; // hasn't aired yet

  time_t oldestAllowed = now - static_cast<time_t>(ch->catchupDays) * 24 * 60 * 60;
  if (tag.GetStartTime() < oldestAllowed)
    return PVR_ERROR_NO_ERROR; // outside the provider's archive retention window

  isPlayable = true;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetEPGTagStreamProperties(
    const kodi::addon::PVREPGTag& tag, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  std::string channelUuid;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(tag.GetUniqueChannelId()));
    if (!ch)
      return PVR_ERROR_INVALID_PARAMETERS;
    channelUuid = ch->uuid;
  }

  int durationMinutes = static_cast<int>((tag.GetEndTime() - tag.GetStartTime()) / 60);
  std::string playbackUrl, error;
  if (!m_client.CreateCatchupSession(channelUuid, tag.GetStartTime(), durationMinutes, playbackUrl, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to create catch-up session: %s", error.c_str());
    return PVR_ERROR_FAILED;
  }

  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, playbackUrl);
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
  properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "video/mp2t");

  // PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE was tried here too (a plain
  // Kodi-core flag, unrelated to ffmpegdirect) to make the OSD feel more
  // like live TV. Reverted: setting it makes Kodi re-route playback through
  // GetChannelStreamProperties() -- the *live-channel* path -- instead of
  // just using the catch-up URL returned here, which isn't what a static,
  // already-complete archived file needs and broke seeking further.
  //
  // inputstream.ffmpegdirect was tried here (both "timeshift" and "catchup"
  // stream_mode) to address unreliable seeking, then reverted after
  // confirming via its actual source (src/stream/TimeshiftBuffer.cpp,
  // src/stream/FFmpegCatchupStream.cpp) that neither mode's seek model
  // matches how Dispatcharr's catch-up API actually works:
  //   - "timeshift" mode locally records and segments what it assumes is a
  //     *live*, continuously-arriving source, then seeks only within what
  //     it has already recorded itself -- our catch-up URL is instead a
  //     single, already-complete archived file.
  //   - "catchup" mode seeks by reconstructing a *new* URL for the exact
  //     wall-clock time being sought to (FFmpegCatchupStream::
  //     SeekCatchupStream -> GetUpdatedCatchupUrl()), which requires the
  //     backend to support starting playback from an arbitrary in-programme
  //     timestamp. Dispatcharr's own docs are explicit that its catch-up
  //     `start` parameter only selects *which programme* to fetch, not a
  //     time within it -- in-programme seeking is meant to happen via plain
  //     HTTP Range on the byte stream, which is exactly what Kodi's default
  //     player already does (see docs/API_NOTES.md for why that's still
  //     imprecise for raw MPEG-TS, and why this was worth investigating).
  // Confirmed live with a real install: "timeshift" mode didn't just fail
  // to improve seeking, it broke it entirely (no seeking at all), which
  // fits -- it isn't merely suboptimal for this URL shape, it's the wrong
  // mechanism for it.
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Recordings
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetRecordingsAmount(bool deleted, int& amount)
{
  if (deleted)
  {
    amount = 0; // Dispatcharr recording trash/undelete not implemented here
    return PVR_ERROR_NO_ERROR;
  }
  std::vector<Recording> recordings;
  std::string error;
  if (!m_client.GetRecordings(recordings, error))
    return PVR_ERROR_SERVER_ERROR;
  // In-progress recordings belong here too, not just upcoming/scheduled
  // ones excluded below -- Kodi's own CPVRRecording::IsInProgress() cross-
  // references GetRecordings() against the active timer list by
  // channel+time overlap to decide whether a *listed recording* is still
  // being written, and that's also what makes it clickable/playable while
  // recording. Omitting in-progress ones here (as an earlier version of
  // this code did) made them show up only as an uneditable timer entry,
  // with nothing to actually click and play.
  amount = static_cast<int>(std::count_if(
      recordings.begin(), recordings.end(), [](const Recording& r) { return !r.isUpcoming; }));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  if (deleted)
    return PVR_ERROR_NO_ERROR;

  std::vector<Recording> recordings;
  std::string error;
  if (!m_client.GetRecordings(recordings, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to load recordings: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }

  for (const auto& rec : recordings)
  {
    // Only a not-yet-started recording has nothing to play at all; skip
    // that case. In-progress ones belong here too (see
    // GetRecordingsAmount() above for why) -- must match its filter.
    if (rec.isUpcoming)
      continue;
    kodi::addon::PVRRecording recording;
    recording.SetRecordingId(std::to_string(rec.id));
    recording.SetTitle(rec.title);
    recording.SetEpisodeName(rec.subtitle);
    recording.SetPlot(rec.description);
    recording.SetChannelUid(rec.channelId > 0 ? rec.channelId : PVR_CHANNEL_INVALID_UID);
    recording.SetRecordingTime(rec.startTime);
    recording.SetDuration(rec.durationSeconds);
    recording.SetIsDeleted(false);
    results.Add(recording);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording,
                                                        std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  // The claim this used to make here -- that Kodi never actually consults
  // STREAMURL for a pvr://recordings/... item, always routing through
  // CInputStreamPVRRecording's OpenRecordedStream()/ReadRecordedStream()/etc.
  // below instead -- turned out to be wrong in practice: a live kodi.log
  // from a real failed playback showed Kodi's generic CCurlFile opening
  // this exact pipe-delimited STREAMURL directly (bypassing our addon's
  // OpenRecordedStream() entirely, including its 401-retry/API-key-regen
  // logic, which never even ran). Leave STREAMURL unset for a completed
  // recording for that reason, so Kodi has no choice but to use the addon
  // callbacks below.
  //
  // An in-progress recording is different: OpenRecordingStream() rejects it
  // outright (it's a growing HLS playlist, not a Range-seekable file), so
  // there's no existing behaviour to preserve by leaving STREAMURL unset.
  // If enabled (opt-in, off by default -- see settings.xml/strings.po: this
  // hasn't seen the real-world use the completed-recording path has, and
  // ffmpegdirect has already needed reverting twice elsewhere in this addon
  // for unrelated reasons), route it through inputstream.ffmpegdirect
  // instead, forced into its plain ffmpeg-native open mode so libavformat's
  // own HLS demuxer -- not Kodi's native one -- handles segment fetches,
  // propagating the X-API-Key header from the URL to every segment, not
  // just the manifest. See FetchInProgressPlaylistSnapshot()'s comment for
  // why this needs open_mode forced to "ffmpeg" specifically, why neither
  // of the stream_mode values already tried (and reverted) for live
  // TV/catch-up elsewhere in this addon apply here, and why STREAMURL below
  // points at this addon's own local loopback server rather than either
  // Dispatcharr's live playlist URL directly (confirmed live: libavformat
  // joins near the live edge instead of the true beginning that way,
  // regardless of is_realtime_stream) or a data: URI (confirmed live and
  // via source: Kodi's own CURL class can't parse one at all, since
  // CURL::Parse() hard-requires a "://" that a standards-compliant data:
  // URI never has).
  if (m_enableInProgressPlayback)
  {
    int id = std::atoi(recording.GetRecordingId().c_str());
    std::vector<Recording> recordings;
    std::string error;
    bool inProgress = false;
    if (m_client.GetRecordings(recordings, error))
    {
      for (const auto& rec : recordings)
      {
        if (rec.id == id)
        {
          inProgress = rec.isInProgress;
          break;
        }
      }
    }

    if (inProgress)
    {
      int playlistServerPort = m_playlistServer.GetPort();
      if (playlistServerPort == 0)
        return PVR_ERROR_SERVER_ERROR;

      // Seek and live-tailing are mutually exclusive here (see
      // FetchInProgressPlaylistSnapshot()'s and
      // FetchInProgressRecordingSeekableSnapshot()'s comments for the full
      // reasoning: Kodi won't offer seeking without a known total
      // duration, and the only way to get libavformat to report one is to
      // mark the playlist complete, which stops it from ever picking up
      // segments recorded after that point). Which one this session gets
      // is decided by m_pendingLiveModeRecordingId (see its comment) --
      // consumed here whether or not it actually matches this id, since
      // it's a one-shot arm/consume flag either way.
      bool useLiveMode = false;
      {
        std::lock_guard<std::mutex> lock(m_pendingLiveModeMutex);
        if (m_pendingLiveModeRecordingId == id)
          useLiveMode = true;
        m_pendingLiveModeRecordingId = -1;
      }

      if (!useLiveMode)
      {
        // "Play from start (seek)": a one-time, definite-VOD-shaped
        // snapshot as of right now, not a live-tailing one -- fetched
        // once here rather than via a provider callback, since
        // libavformat never reloads an ENDLIST-terminated playlist and
        // this will only ever be asked for once per session anyway.
        std::string keyBefore = m_client.GetApiKey();
        std::string seekError;
        std::string playlistText =
            m_client.FetchInProgressRecordingSeekableSnapshot(id, seekError);
        std::string keyAfter = m_client.GetApiKey();
        if (keyAfter != keyBefore)
          kodi::addon::SetSettingString("api_key", keyAfter);
        if (playlistText.empty())
          return PVR_ERROR_SERVER_ERROR;

        m_playlistServer.SetPlaylistProvider(
            id, [playlistText](int /*maxSegments*/) { return playlistText; });
      }
      else
      {
        // "Play live": registers a provider rather than fetching once
        // here -- libavformat calls back into this repeatedly over the
        // whole playback session (see LocalPlaylistServer.h and
        // FetchInProgressPlaylistSnapshot()'s comment for why a live
        // re-fetch on every call, not a one-time snapshot, is what lets a
        // single session keep tailing newly-recorded segments as the
        // recording grows).
        m_playlistServer.SetPlaylistProvider(id, [this, id](int maxSegments) {
          std::string keyBefore = m_client.GetApiKey();
          std::string playlistText;
          std::string fetchError;
          playlistText = m_client.FetchInProgressPlaylistSnapshot(id, maxSegments, fetchError);
          // May have just self-healed a stale key while building this.
          // Persist it the same way OpenRecordedStream()/ReadRecordedStream()
          // do, so a restart of this install doesn't immediately invalidate
          // it again. Only fixes up this addon's own future fetches, not the
          // X-API-Key header already baked into this STREAMURL for
          // ffmpegdirect's own segment fetches -- see
          // FetchInProgressPlaylistSnapshot()'s comment.
          std::string keyAfter = m_client.GetApiKey();
          if (keyAfter != keyBefore)
            kodi::addon::SetSettingString("api_key", keyAfter);
          if (m_debugLogging)
          {
            int lineCount = static_cast<int>(std::count(playlistText.begin(), playlistText.end(), '\n'));
            bool hasEndlist = playlistText.find("#EXT-X-ENDLIST") != std::string::npos;
            bool hasFirstSegment = playlistText.find("seg_00000.ts") != std::string::npos;
            kodi::Log(ADDON_LOG_INFO,
                      "pvr.dispatcharrai: playlist snapshot for recording %d: maxSegments=%d, "
                      "lines=%d, hasEndlist=%d, containsSeg00000=%d",
                      id, maxSegments, lineCount, hasEndlist ? 1 : 0, hasFirstSegment ? 1 : 0);
          }
          return playlistText;
        });
      }

      std::string streamUrl =
          "http://127.0.0.1:" + std::to_string(playlistServerPort) + "/playlist/" +
          std::to_string(id) + ".m3u8";
      // ffmpegdirect's header mapping (CDVDDemuxFFmpeg::GetFFMpegOptionsFromInput,
      // confirmed against its source and a live failed attempt: it logged
      // "ignoring header option 'X-API-Key'" without the prefix) only forwards
      // a fixed allowlist of standard HTTP header names as real headers --
      // anything else needs a literal "!" prefix, which it strips before using
      // the rest as the header name. This addon's own local playlist server
      // ignores this header entirely -- it's attached here only because
      // ffmpeg's HLS demuxer shares the same avio_opts dictionary across the
      // manifest fetch and every real segment sub-fetch to Dispatcharr,
      // regardless of which host the manifest itself came from.
      std::string apiKey = m_client.GetApiKey();
      if (!apiKey.empty())
        streamUrl += "|!X-API-Key=" + apiKey;

      // is_realtime_stream is deliberately "false", not "true": read from
      // ffmpegdirect's actual source (FFmpegStream::GetCapabilities()),
      // INPUTSTREAM_SUPPORTS_SEEK/PAUSE/ITIME are only advertised when this
      // is false, and Kodi only performs its normal "seek to start
      // position" on open when seeking is advertised as supported. It's
      // also arguably more correct here regardless: this is a rewritten,
      // definite-VOD-shaped snapshot by the time it reaches ffmpegdirect,
      // not a live stream.
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "application/x-mpegURL");
      properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
      properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
      properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
      return PVR_ERROR_NO_ERROR;
    }
  }

  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook,
                                                 const kodi::addon::PVRRecording& item)
{
  if (menuhook.GetHookId() != kMenuHookPlayLive)
    return PVR_ERROR_NOT_IMPLEMENTED;

  // PVR_MENUHOOK_RECORDING has no per-item visibility hook -- confirmed
  // against Kodi-core (PVRContextMenus.cpp's PVRClientMenuHook::IsVisible())
  // that a recording-category hook shows on every recording's context menu,
  // completed ones included. Silently no-op (with an explanatory
  // notification) rather than arming anything for one that isn't actually
  // in progress.
  int id = std::atoi(item.GetRecordingId().c_str());
  std::vector<Recording> recordings;
  std::string error;
  bool inProgress = false;
  if (m_client.GetRecordings(recordings, error))
  {
    for (const auto& rec : recordings)
    {
      if (rec.id == id)
      {
        inProgress = rec.isInProgress;
        break;
      }
    }
  }

  if (!inProgress)
  {
    kodi::QueueNotification(QUEUE_INFO, "", kodi::addon::GetLocalizedString(30046));
    return PVR_ERROR_NO_ERROR;
  }

  // See m_pendingLiveModeRecordingId's comment: a binary PVR addon can't
  // start playback itself, so this arms the next GetRecordingStreamProperties()
  // call for this id rather than opening anything directly -- the user
  // still has to press Play themselves right after.
  {
    std::lock_guard<std::mutex> lock(m_pendingLiveModeMutex);
    m_pendingLiveModeRecordingId = id;
  }
  kodi::QueueNotification(QUEUE_INFO, "", kodi::addon::GetLocalizedString(30045));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  if (!m_client.DeleteRecording(id, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to delete recording %d: %s", id, error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  TriggerRecordingUpdate();
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  std::string keyBefore = m_client.GetApiKey();
  if (!m_client.OpenRecordingStream(id, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to open recording %d: %s", id, error.c_str());
    return false;
  }
  // OpenRecordingStream() may have silently regenerated the API key (see its
  // comment) if another Kodi install using this same Dispatcharr account had
  // invalidated the one persisted here. Save the new one so a restart of
  // this install doesn't immediately invalidate it again.
  std::string keyAfter = m_client.GetApiKey();
  if (keyAfter != keyBefore)
    kodi::addon::SetSettingString("api_key", keyAfter);
  return true;
}

void PVRDispatcharr::CloseRecordedStream()
{
  m_client.CloseRecordingStream();
}

int PVRDispatcharr::ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
  // Same self-heal persistence as OpenRecordedStream(): the key can also be
  // invalidated mid-playback by another install, not just between opens.
  std::string keyBefore = m_client.GetApiKey();
  int result = m_client.ReadRecordingStream(buffer, size);
  std::string keyAfter = m_client.GetApiKey();
  if (keyAfter != keyBefore)
    kodi::addon::SetSettingString("api_key", keyAfter);
  return result;
}

int64_t PVRDispatcharr::SeekRecordedStream(int64_t position, int whence)
{
  return m_client.SeekRecordingStream(position, whence);
}

int64_t PVRDispatcharr::LengthRecordedStream()
{
  return m_client.GetRecordingStreamLength();
}

// ---------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
  // TODO(verify): PVR_TIMER_TYPE_* flag names against
  // <kodi/addon-instance/pvr/Timers.h> for your target Kodi version.
  kodi::addon::PVRTimerType oneTime;
  oneTime.SetId(kTimerTypeOneTime);
  oneTime.SetAttributes(PVR_TIMER_TYPE_IS_MANUAL | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                        PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME |
                        PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH);
  oneTime.SetDescription("One-time recording (manual)");
  types.push_back(oneTime);

  // Separate from the manual type above: Kodi's own "Record" button on an
  // EPG guide entry (CGUIDialogPVRGuideInfo::OnClickButtonRecord /
  // PVRContextMenus.cpp's StartRecording) creates a timer via
  // CPVRTimerInfoTag::CreateFromEpg(), which explicitly searches for a
  // timer type WITHOUT PVR_TIMER_TYPE_IS_MANUAL and WITHOUT
  // PVR_TIMER_TYPE_IS_REPEATING (confirmed in Kodi's own source,
  // xbmc/pvr/timers/PVRTimerInfoTag.cpp). The one-time type above has
  // IS_MANUAL set (needed for Kodi's separate "add a manual timer with no
  // EPG event" flow), and the series type below has IS_REPEATING set, so
  // neither one qualifies -- without this type, CreateFromEpg() always
  // returned null and "Record" from the guide could never create a real
  // timer. AddTimer()/DeleteTimer() already treat anything that isn't
  // kTimerTypeSeries as a one-time recording, so no other code needed to
  // change for this type to work.
  kodi::addon::PVRTimerType oneTimeEpg;
  oneTimeEpg.SetId(kTimerTypeOneTimeEpgBased);
  oneTimeEpg.SetAttributes(PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_TIME |
                           PVR_TIMER_TYPE_SUPPORTS_END_TIME |
                           PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH);
  oneTimeEpg.SetDescription("One-time recording (from guide)");
  types.push_back(oneTimeEpg);

  kodi::addon::PVRTimerType series;
  series.SetId(kTimerTypeSeries);
  series.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                       PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH |
                       PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES);
  series.SetDescription("Record series (via Dispatcharr series rule)");
  // Maps to Dispatcharr's SeriesRuleRequest.mode ("all" vs "new") --
  // confirmed against the live schema. Kodi shows this as a normal
  // per-timer setting when creating/editing a series rule.
  series.SetPreventDuplicateEpisodes({{0, "Record all episodes"}, {1, "Record only new episodes"}},
                                     0);
  types.push_back(series);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetTimersAmount(int& amount)
{
  std::vector<Recording> recordings;
  std::vector<TimerRule> rules;
  std::string error;
  m_client.GetRecordings(recordings, error);
  m_client.GetTimerRules(rules, error);
  int scheduled = static_cast<int>(std::count_if(
      recordings.begin(), recordings.end(),
      [](const Recording& r) { return r.isInProgress || r.isUpcoming; }));
  amount = scheduled + static_cast<int>(rules.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
  std::string error;

  std::vector<Recording> recordings;
  if (m_client.GetRecordings(recordings, error))
  {
    for (const auto& rec : recordings)
    {
      // Completed recordings are surfaced via GetRecordings(), not as timers.
      if (!rec.isInProgress && !rec.isUpcoming)
        continue;
      kodi::addon::PVRTimer timer;
      timer.SetClientIndex(static_cast<unsigned int>(rec.id));
      timer.SetTimerType(kTimerTypeOneTime);
      timer.SetTitle(rec.title);
      timer.SetClientChannelUid(rec.channelId);
      timer.SetStartTime(rec.startTime);
      timer.SetEndTime(rec.endTime);
      timer.SetState(rec.isInProgress ? PVR_TIMER_STATE_RECORDING : PVR_TIMER_STATE_SCHEDULED);
      results.Add(timer);
    }
  }

  std::vector<TimerRule> rules;
  if (m_client.GetTimerRules(rules, error))
  {
    for (const auto& rule : rules)
    {
      kodi::addon::PVRTimer timer;
      // Series rules have no numeric id at all in Dispatcharr's API
      // (confirmed against a real rule: {mode, title, tvg_id, channel_id,
      // title_mode, description, description_mode} -- nothing else), so
      // rule.id is always 0 and can't be used here -- every series rule
      // would collide on the same ClientIndex. Hash the (title, tvgId)
      // pair instead, the same identity DeleteSeriesRule() uses, masked
      // into the lower 30 bits so the series-rule flag bit above it is
      // never disturbed.
      std::size_t h = std::hash<std::string>()(rule.title + '\x1f' + rule.tvgId);
      timer.SetClientIndex((static_cast<unsigned int>(h) & 0x3FFFFFFFu) | 0x40000000);
      timer.SetTimerType(kTimerTypeSeries);
      timer.SetTitle(rule.title);
      timer.SetClientChannelUid(rule.channelId);
      timer.SetState(PVR_TIMER_STATE_SCHEDULED);
      timer.SetPreventDuplicateEpisodes(rule.recordNewOnly ? 1 : 0);
      results.Add(timer);
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::AddTimer(const kodi::addon::PVRTimer& timer)
{
  std::string error;
  bool ok;
  if (timer.GetTimerType() == kTimerTypeSeries)
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(timer.GetClientChannelUid()));
    std::string tvgId = ch ? ch->tvgId : "";
    ok = m_client.CreateSeriesRule(static_cast<int>(timer.GetClientChannelUid()), tvgId,
                                   timer.GetTitle(), timer.GetPreventDuplicateEpisodes() != 0,
                                   error);
  }
  else
  {
    ok = m_client.CreateOneTimeRecording(static_cast<int>(timer.GetClientChannelUid()),
                                         timer.GetStartTime(), timer.GetEndTime(),
                                         timer.GetTitle(), error);
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to create timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  TriggerTimerUpdate();
  // A one-time recording for a start time at or near "now" (or an already
  // in-progress EPG event, e.g. Kodi's "Record" button on a live guide
  // entry) may already be actively recording by the time this returns.
  // Without this, Kodi has no reason to re-poll GetRecordings() until its
  // own next periodic refresh -- confirmed: a freshly-created recording
  // did not appear under Recordings for several minutes without it, and a
  // full Kodi restart was what actually surfaced it. Harmless no-op for a
  // genuinely future recording or a series rule.
  TriggerRecordingUpdate();
  // Dispatcharr fills in the recording's real title (custom_properties.
  // program.title, see GetRecordings()) asynchronously, a moment after it
  // actually starts -- confirmed: right at creation, custom_properties is
  // still `{}`. The immediate TriggerRecordingUpdate() above fires before
  // that happens, so Kodi's first (and, confirmed, often *only* --
  // nothing else prompts it to ask again) fetch gets our "Recording <id>"
  // fallback and keeps showing it indefinitely, even after the recording
  // finishes. A second, delayed trigger gives Dispatcharr time to enrich
  // it first. Detached: AddTimer() shouldn't block Kodi's calling thread
  // for this.
  if (timer.GetTimerType() != kTimerTypeSeries)
  {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      TriggerRecordingUpdate();
    }).detach();
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  bool isSeries = (timer.GetClientIndex() & 0x40000000) != 0;
  std::string error;
  bool ok;
  if (isSeries)
  {
    // Series rules have no numeric id in Dispatcharr's API -- they're
    // deleted by title + tvg_id, the same identity AddTimer() used to
    // create them (see CreateSeriesRule() above).
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(timer.GetClientChannelUid()));
    std::string tvgId = ch ? ch->tvgId : "";
    ok = m_client.DeleteSeriesRule(timer.GetTitle(), tvgId, error);
  }
  else
  {
    int id = static_cast<int>(timer.GetClientIndex() & ~0x40000000u);
    // Confirmed against Kodi's own source (xbmc/pvr/timers/PVRTimers.cpp):
    // forceDelete is specifically how Kodi tells the addon "this timer is
    // still actively recording" -- both its dedicated "Stop Recording"
    // action and "Delete" on a timer it already knows is recording pass
    // it as true. That must route to Dispatcharr's dedicated stop
    // endpoint ("stop a recording early while retaining the partial
    // content for playback"), NOT the delete endpoint (removes the file
    // entirely) -- confirmed by testing: routing it to delete wiped out
    // an actively-recording file the user only meant to stop.
    ok = forceDelete ? m_client.StopRecording(id, error) : m_client.DeleteRecording(id, error);
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to delete timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  TriggerTimerUpdate();
  // Stopping a recording (forceDelete=true, see above) turns it into a
  // normal completed recording immediately, not just a future timer-list
  // change -- make sure Kodi's Recordings view picks that up too, same
  // reasoning as AddTimer()'s trigger.
  if (!isSeries)
    TriggerRecordingUpdate();
  return PVR_ERROR_NO_ERROR;
}
