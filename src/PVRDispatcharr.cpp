#include "PVRDispatcharr.h"

#include "WebSocketClient.h"

#include <kodi/AddonBase.h>
#include <kodi/General.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <thread>
#include <unordered_set>

using namespace dispatcharr;

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
  config.apiKey = kodi::addon::GetSettingString("api_key", "");
  return config;
}

int PVRDispatcharr::EffectiveRecurringRuleUtcOffsetMinutes() const
{
  std::string zone = kodi::addon::GetSettingString("recurring_rule_timezone", "manual");
  if (zone != "manual")
  {
    int computed = 0;
    if (DispatcharrClient::ComputeKnownZoneOffsetMinutes(zone, time(nullptr), computed))
      return computed;
  }
  return m_recurringRuleUtcOffsetMinutes;
}

PVRDispatcharr::PVRDispatcharr(const kodi::addon::IInstanceInfo& instance)
    : CInstancePVRClient(instance), m_lastAppliedConfig(LoadConfigFromSettings()),
      m_client(LoadConfigFromSettings())
{
  m_channelRefreshHours = kodi::addon::GetSettingInt("channel_refresh_hours", 12);
  m_epgRefreshHours = kodi::addon::GetSettingInt("epg_refresh_hours", 4);
  m_liveTimeshiftMode = kodi::addon::GetSettingInt("live_timeshift_mode", kLiveTimeshiftServer);
  m_enableCatchupFfmpegdirectSeek =
      kodi::addon::GetSettingBoolean("enable_catchup_ffmpegdirect_seek", false);
  m_recordingRefreshMinutes = kodi::addon::GetSettingInt("recording_refresh_minutes", 5);
  m_recurringRuleUtcOffsetMinutes = kodi::addon::GetSettingInt("recurring_rule_utc_offset_minutes", 0);
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

  // Dispatcharr's recording pre/post padding is genuinely global-only --
  // no per-timer override exists server-side (confirmed against its own
  // source) -- so rather than a Kodi per-timer margin UI, which would
  // misleadingly imply a per-timer effect Dispatcharr doesn't have, this
  // is surfaced as a plain settings-screen value that mirrors
  // Dispatcharr's real global setting directly. Synced FROM Dispatcharr
  // on every startup, not just once, so Kodi's display never goes stale
  // relative to a change made another way (Dispatcharr's own web UI, a
  // different Kodi install sharing the account) -- same self-heal
  // reasoning as the API key above, just reading instead of generating.
  // Only actually rewrites Kodi's own persisted setting if the value is
  // genuinely different, so a normal restart with nothing changed
  // doesn't churn OnAddonSettingChanged() for no reason.
  {
    int pre = 0, post = 0;
    std::string offsetError;
    if (m_client.GetDvrOffsetMinutes(pre, post, offsetError))
    {
      if (kodi::addon::GetSettingInt("recording_pre_offset_minutes", -1) != pre)
        kodi::addon::SetSettingInt("recording_pre_offset_minutes", pre);
      if (kodi::addon::GetSettingInt("recording_post_offset_minutes", -1) != post)
        kodi::addon::SetSettingInt("recording_post_offset_minutes", post);
    }
    else if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharrai: could not read Dispatcharr's DVR padding settings: %s",
                offsetError.c_str());
    }
  }

  // Same self-heal-on-every-startup reasoning as the padding sync above.
  // Dispatcharr's own configured IANA timezone name (e.g.
  // "America/Chicago") is always surfaced as a read-only reference. When
  // that zone is one of the short list
  // DispatcharrClient::ComputeKnownZoneOffsetMinutes() knows the DST rules
  // for, recurring_rule_timezone is auto-selected to match it, the same
  // authoritative "Dispatcharr's real value wins" way the padding settings
  // above already work -- not just a suggestion, since a stale manual
  // offset silently makes recurring timers fire at the wrong time. Note
  // this only picks the *zone*, not a numeric offset: the actual offset is
  // computed live wherever it's actually needed (see
  // EffectiveRecurringRuleUtcOffsetMinutes()), so it can never go stale
  // across a DST transition the way pre-computing it once here would --
  // that was a real gap in an earlier version of this sync. Any zone
  // outside the known list leaves recurring_rule_timezone at "manual" (the
  // default), falling back to the existing plain manual offset entry --
  // this addon still can't derive an arbitrary zone's current offset
  // without bundling a real timezone database (see docs/RECURRING_RULES.md
  // for why that was deliberately ruled out).
  {
    std::string timeZone, tzError;
    if (m_client.GetSystemTimeZone(timeZone, tzError))
    {
      if (kodi::addon::GetSettingString("dispatcharr_timezone_info", "") != timeZone)
        kodi::addon::SetSettingString("dispatcharr_timezone_info", timeZone);

      int probeOffset = 0;
      bool known = DispatcharrClient::ComputeKnownZoneOffsetMinutes(timeZone, time(nullptr), probeOffset);
      std::string desiredZoneSetting = known ? timeZone : "manual";
      if (kodi::addon::GetSettingString("recurring_rule_timezone", "manual") != desiredZoneSetting)
      {
        kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: setting recurring_rule_timezone=%s (%s)",
                  desiredZoneSetting.c_str(),
                  known ? "known zone" : "unrecognized zone, falling back to manual offset entry");
        kodi::addon::SetSettingString("recurring_rule_timezone", desiredZoneSetting);
      }
    }
    else if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharrai: could not read Dispatcharr's system timezone: %s",
                tzError.c_str());
    }
  }

  StartRecordingRefreshThread();
  StartChannelEpgRefreshThread();
  if (m_enableRealtimeUpdates)
    StartRealtimeUpdateThread();
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
    std::lock_guard<std::mutex> lock(m_channelEpgRefreshMutex);
    m_stopChannelEpgRefreshThread = true;
  }
  m_channelEpgRefreshCv.notify_all();
  if (m_channelEpgRefreshThread.joinable())
    m_channelEpgRefreshThread.join();

  {
    std::lock_guard<std::mutex> lock(m_realtimeUpdateMutex);
    m_stopRealtimeUpdateThread = true;
  }
  m_realtimeUpdateCv.notify_all();
  if (m_realtimeUpdateThread.joinable())
    m_realtimeUpdateThread.join();
}

ADDON_STATUS PVRDispatcharr::OnAddonSettingChanged(const std::string& settingName,
                                                   const kodi::addon::CSettingValue& settingValue)
{
  if (settingName == "live_timeshift_mode")
  {
    m_liveTimeshiftMode = settingValue.GetInt();
  }
  else if (settingName == "channel_refresh_hours")
  {
    m_channelRefreshHours = settingValue.GetInt();
  }
  else if (settingName == "epg_refresh_hours")
  {
    m_epgRefreshHours = settingValue.GetInt();
  }
  else if (settingName == "enable_catchup_ffmpegdirect_seek")
  {
    m_enableCatchupFfmpegdirectSeek = settingValue.GetBoolean();
  }
  else if (settingName == "recording_refresh_minutes")
  {
    m_recordingRefreshMinutes = settingValue.GetInt();
    // Wake the thread immediately rather than leaving it asleep for up to
    // the *old* interval before it notices the new one -- wait_for() only
    // re-reads m_recordingRefreshMinutes when it actually wakes.
    m_recordingRefreshCv.notify_all();
  }
  else if (settingName == "recurring_rule_utc_offset_minutes")
  {
    m_recurringRuleUtcOffsetMinutes = settingValue.GetInt();
  }
  else if (settingName == "recording_pre_offset_minutes" ||
           settingName == "recording_post_offset_minutes")
  {
    // Global-only on Dispatcharr's side (see DispatcharrClient::
    // SetDvrOffsetMinutes()'s own comment) -- always push both current
    // values together regardless of which one actually changed, since
    // that's what Dispatcharr's own storage expects; whichever of the
    // two ISN'T the one that just changed is read back from Kodi's own
    // already-current settings rather than tracked separately here.
    int pre = settingName == "recording_pre_offset_minutes"
                  ? settingValue.GetInt()
                  : kodi::addon::GetSettingInt("recording_pre_offset_minutes", 0);
    int post = settingName == "recording_post_offset_minutes"
                   ? settingValue.GetInt()
                   : kodi::addon::GetSettingInt("recording_post_offset_minutes", 0);
    // Detached: this is a real network round-trip (unlike every other
    // branch here, a plain in-memory write), and there's no reason to
    // block whatever thread Kodi delivers SetSetting() on for it --
    // best-effort, with the constructor's own sync-from-Dispatcharr on
    // the next restart as a natural retry if this particular push
    // silently fails.
    std::thread([this, pre, post]() {
      std::string offsetError;
      if (!m_client.SetDvrOffsetMinutes(pre, post, offsetError))
      {
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharrai: failed to update Dispatcharr's DVR padding: %s",
                  offsetError.c_str());
      }
    }).detach();
  }
  else if (settingName == "debug_logging")
  {
    m_debugLogging = settingValue.GetBoolean();
  }
  else if (settingName == "enable_realtime_updates")
  {
    // Deliberately not applied live -- would mean dynamically starting or
    // stopping m_realtimeUpdateThread outside its normal
    // constructor/destructor lifecycle, real added complexity for a
    // setting that's already documented experimental. Restart picks it up
    // the same way every setting used to work before this method existed.
    //
    // Guarded against Kodi's spurious-renotification quirk the same way
    // as the connection settings just below -- see m_lastAppliedConfig's
    // own comment for why this guard exists at all (confirmed live, not
    // theoretical: without it, saving *any* setting restarted the
    // instance every time).
    bool value = settingValue.GetBoolean();
    bool changed = value != m_enableRealtimeUpdates;
    m_enableRealtimeUpdates = value;
    return changed ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
  }
  else if (settingName == "host" || settingName == "port" || settingName == "use_https" ||
           settingName == "username" || settingName == "password" ||
           settingName == "verify_ssl" || settingName == "timeout" || settingName == "api_key")
  {
    // Baked into DispatcharrClient's Config at construction (see
    // LoadConfigFromSettings()) -- changing the connection this addon
    // talks to, or re-authenticating against it, isn't something to
    // attempt on a live instance.
    //
    // See m_lastAppliedConfig's own comment: compared against that cached
    // snapshot rather than unconditionally restarting on every
    // notification, since Kodi can (and, confirmed live, reliably does)
    // deliver a same-named, same-value notification here that has nothing
    // to do with this setting actually changing.
    bool changed = false;
    if (settingName == "host")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.host;
      m_lastAppliedConfig.host = std::move(value);
    }
    else if (settingName == "port")
    {
      int value = settingValue.GetInt();
      changed = value != m_lastAppliedConfig.port;
      m_lastAppliedConfig.port = value;
    }
    else if (settingName == "use_https")
    {
      bool value = settingValue.GetBoolean();
      changed = value != m_lastAppliedConfig.useHttps;
      m_lastAppliedConfig.useHttps = value;
    }
    else if (settingName == "username")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.username;
      m_lastAppliedConfig.username = std::move(value);
    }
    else if (settingName == "password")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.password;
      m_lastAppliedConfig.password = std::move(value);
    }
    else if (settingName == "verify_ssl")
    {
      bool value = settingValue.GetBoolean();
      changed = value != m_lastAppliedConfig.verifySsl;
      m_lastAppliedConfig.verifySsl = value;
    }
    else if (settingName == "timeout")
    {
      int value = settingValue.GetInt();
      changed = value != m_lastAppliedConfig.timeoutSeconds;
      m_lastAppliedConfig.timeoutSeconds = value;
    }
    else if (settingName == "api_key")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.apiKey;
      m_lastAppliedConfig.apiKey = std::move(value);
    }
    return changed ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
  }
  return ADDON_STATUS_OK;
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
      RenewRecurringRules();
      TriggerRecordingUpdate();
      TriggerTimerUpdate();
    }
  });
}

void PVRDispatcharr::RenewRecurringRules()
{
  std::vector<RecurringRule> rules;
  std::string error;
  if (!m_client.GetRecurringRules(rules, error))
    return;

  std::vector<Recording> recordings;
  bool haveRecordings = m_client.GetRecordings(recordings, error);

  time_t now = time(nullptr);
  for (const auto& rule : rules)
  {
    if (!rule.enabled)
      continue; // nothing materializes for a disabled rule anyway
    time_t daysLeft = (rule.endDate - now) / 86400;
    if (daysLeft >= kRecurringRuleWindowDays / 2)
      continue; // still comfortably inside the window

    // Skip a rule with an occurrence currently recording or about to
    // start soon -- see kRecurringRuleRenewalSafetyMarginSeconds's own
    // comment. If GetRecordings() itself failed, err toward skipping
    // rather than renewing blind.
    bool hasActiveOrImminentOccurrence = !haveRecordings;
    if (haveRecordings)
    {
      for (const auto& rec : recordings)
      {
        if (rec.recurringRuleId != rule.id)
          continue;
        if (rec.isInProgress ||
            (rec.isUpcoming &&
             rec.startTime - now < kRecurringRuleRenewalSafetyMarginSeconds))
        {
          hasActiveOrImminentOccurrence = true;
          break;
        }
      }
    }
    if (hasActiveOrImminentOccurrence)
      continue; // try again next cycle

    time_t newEndDate = now + static_cast<time_t>(kRecurringRuleWindowDays) * 86400;
    std::string extendError;
    if (!m_client.ExtendRecurringRuleEndDate(rule.id, newEndDate, extendError))
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to renew recurring rule %d: %s",
               rule.id, extendError.c_str());
    }
    else if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG,
               "pvr.dispatcharrai: renewed recurring rule %d end_date forward", rule.id);
    }
  }
}

void PVRDispatcharr::StartChannelEpgRefreshThread()
{
  m_channelEpgRefreshThread = std::thread([this]() {
    while (true)
    {
      // Checked (and, if stale, fetched) immediately on every wake,
      // starting with the very first one -- this is what actually
      // pre-warms the cache ahead of Kodi's own first GetChannels() call,
      // rather than only reacting after channel_refresh_hours/
      // epg_refresh_hours has already elapsed once.
      if (EnsureChannelsLoaded())
      {
        if (m_debugLogging)
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: background thread refreshed channels/groups");
        TriggerChannelGroupsUpdate();
        TriggerChannelUpdate();
      }
      if (EnsureEpgLoaded())
      {
        if (m_debugLogging)
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: background thread refreshed EPG");
        // No bulk/whole-guide equivalent exists in Kodi's PVR API --
        // TriggerEpgUpdate() is per-channel only (confirmed in
        // kodi-dev-kit's PVR.h). Channel/EPG refreshes are already coarse
        // (hours, not minutes), so iterating every known channel here
        // isn't a hot path.
        std::vector<int> channelUids;
        {
          std::lock_guard<std::mutex> lock(m_dataMutex);
          channelUids.reserve(m_channels.size());
          for (const auto& ch : m_channels)
            channelUids.push_back(ch.id);
        }
        for (int uid : channelUids)
          TriggerEpgUpdate(static_cast<unsigned int>(uid));
      }

      std::unique_lock<std::mutex> lock(m_channelEpgRefreshMutex);
      bool stopped = m_channelEpgRefreshCv.wait_for(
          lock, std::chrono::minutes(kChannelEpgRefreshCheckMinutes),
          [this]() { return m_stopChannelEpgRefreshThread.load(); });
      if (stopped)
        break;
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
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsChannelGroups(true);
  capabilities.SetSupportsRecordings(true);
  capabilities.SetSupportsRecordingsDelete(true);
  capabilities.SetSupportsTimers(true);
  capabilities.SetSupportsRecordingPlayCount(false);
  // Backed by this addon's companion recording_edl Dispatcharr plugin (see
  // GetRecordingEdl() below) -- safe to declare unconditionally the same
  // way SetHandlesInputStream() is: a recording with no comskip markers
  // (the common case) or with the plugin not installed just gets an empty
  // EDL back, not an error, so there's no reason to gate this on a
  // setting the way, say, enable_catchup_ffmpegdirect_seek gates a
  // genuinely optional dependency.
  capabilities.SetSupportsRecordingEdl(true);
  capabilities.SetSupportsDescrambleInfo(false);
  // For server-side timeshift's OpenLiveStream()/ReadLiveStream()/
  // SeekLiveStream() (see GetChannelStreamProperties()) and in-progress
  // recording playback's equivalents -- Kodi only actually calls these
  // when GetChannelStreamProperties()/GetRecordingStreamProperties() left
  // STREAMURL unset. Safe to declare unconditionally regardless of
  // live_timeshift_mode: with it set to Off, GetChannelStreamProperties()
  // sets STREAMURL instead, and Kodi simply never calls these for a live
  // channel in that case.
  capabilities.SetHandlesInputStream(true);
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

bool PVRDispatcharr::EnsureChannelsLoaded()
{
  auto now = std::chrono::steady_clock::now();
  bool stale = m_channelsLoadedAt.time_since_epoch().count() == 0 ||
               now - m_channelsLoadedAt > std::chrono::hours(m_channelRefreshHours);
  if (!stale)
    return false;

  std::vector<Channel> channels;
  std::vector<ChannelGroup> groups;
  std::string error;
  bool ok = m_client.GetChannels(channels, error);
  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to load channels: %s", error.c_str());
    return false;
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
  return true;
}

bool PVRDispatcharr::EnsureEpgLoaded()
{
  auto now = std::chrono::steady_clock::now();
  bool stale = m_epgLoadedAt.time_since_epoch().count() == 0 ||
               now - m_epgLoadedAt > std::chrono::hours(m_epgRefreshHours);
  if (!stale)
    return false;

  std::string xml, error;
  if (!m_client.GetXmlTvGuide(xml, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to fetch XMLTV guide: %s", error.c_str());
    return false;
  }

  std::unordered_map<std::string, std::vector<EpgEntry>> parsed;
  if (!XmlTvParser::Parse(xml, parsed, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to parse XMLTV guide: %s", error.c_str());
    return false;
  }

  std::lock_guard<std::mutex> lock(m_dataMutex);
  m_epgByChannelNumber = std::move(parsed);
  m_epgLoadedAt = now;
  return true;
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
    if (m_liveTimeshiftMode != kLiveTimeshiftServer)
      streamUrl = m_client.GetLiveStreamUrl(*ch);
  }

  // Live pause/rewind ("timeshift") is opt-in via live_timeshift_mode --
  // see docs/TIMESHIFT.md for the full history, including the earlier
  // local (inputstream.ffmpegdirect on-device buffer) mode this addon
  // used to also offer, removed once server-side proved stable and never
  // reintroduced (Off returned instead, once a real need for a
  // non-admin-account path came up -- see that setting's own help text).
  if (m_liveTimeshiftMode == kLiveTimeshiftServer)
  {
    // Server-side: deliberately leaves STREAMURL unset (confirmed elsewhere
    // in this addon, see GetRecordingStreamProperties()'s comment, that
    // Kodi uses STREAMURL directly via its generic CCurlFile when it's set,
    // bypassing addon stream callbacks entirely) so Kodi falls through to
    // this addon's own OpenLiveStream()/ReadLiveStream()/SeekLiveStream()
    // (PVRCapabilities::SetHandlesInputStream(), set in GetCapabilities())
    // instead of routing through inputstream.ffmpegdirect via a plain URL.
    // That's the whole point: ffmpegdirect's generic HLS seek is confirmed
    // broken for this addon's rolling server-side buffer (see
    // docs/TIMESHIFT.md's seek investigation), the same way it would be for
    // any plain STREAMURL here, so this addon demuxes it via Kodi's own
    // internal demuxer instead, the same proven pattern already used for
    // completed-recording playback (OpenRecordedStream() et al.) -- just
    // against the companion plugin's growing buffer instead of one
    // Dispatcharr-served file. The actual buffer-start call happens in
    // OpenLiveStream(), not here.
    properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
  }
  else
  {
    // Off: a plain live stream, no pause/rewind, no admin account or
    // companion plugin required -- Kodi's generic CCurlFile opens
    // streamUrl directly, no inputstream addon or addon-side stream
    // callback involved at all.
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamUrl);
    properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
    // Dispatcharr's default proxy output is MPEG-TS; if you've configured
    // an HLS stream profile in Dispatcharr, override this in settings and
    // adapt GetLiveStreamUrl() accordingly.
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "video/mp2t");
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

bool PVRDispatcharr::OpenLiveStream(const kodi::addon::PVRChannel& channel)
{
  // Only ever actually called for a server-side-timeshift channel -- see
  // GetChannelStreamProperties(), which is the only mode that leaves
  // STREAMURL unset. The mode check here is just defense in depth.
  if (m_liveTimeshiftMode != kLiveTimeshiftServer)
    return false;

  std::string channelUuid;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(channel.GetUniqueId()));
    if (!ch)
      return false;
    channelUuid = ch->uuid;
  }

  std::string error;
  if (!m_client.OpenLiveTimeshiftStream(channelUuid, error))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharrai: failed to open server-side timeshift stream for channel %s: "
              "%s (confirm the timeshift_buffer Dispatcharr plugin is installed and enabled, and "
              "that this addon's configured account is a Dispatcharr admin)",
              channelUuid.c_str(), error.c_str());
    return false;
  }
  return true;
}

void PVRDispatcharr::CloseLiveStream()
{
  m_client.CloseLiveTimeshiftStream();
}

int PVRDispatcharr::ReadLiveStream(unsigned char* buffer, unsigned int size)
{
  return m_client.ReadLiveTimeshiftStream(buffer, size);
}

int64_t PVRDispatcharr::SeekLiveStream(int64_t position, int whence)
{
  return m_client.SeekLiveTimeshiftStream(position, whence);
}

int64_t PVRDispatcharr::LengthLiveStream()
{
  return m_client.GetLiveTimeshiftStreamLength();
}

bool PVRDispatcharr::CanPauseStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

bool PVRDispatcharr::CanSeekStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

bool PVRDispatcharr::IsRealTimeStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

PVR_ERROR PVRDispatcharr::GetStreamTimes(kodi::addon::PVRStreamTimes& times)
{
  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharrai: GetStreamTimes called: liveTimeshiftOpen=%d inProgressOpen=%d "
            "durationMs=%lld",
            m_client.IsLiveTimeshiftStreamOpen() ? 1 : 0, m_client.IsInProgressRecordingStreamOpen() ? 1 : 0,
            static_cast<long long>(m_client.GetInProgressRecordingStreamDurationMs()));
  // startTime/ptsStart both zero: no meaningful wall-clock "show start" for
  // a growing buffer/recording the way a scheduled EPG programme would
  // have, so pts values here are purely self-relative rather than
  // UTC-anchored -- see kodi-dev-kit's own PVRStreamTimes doc comments.
  // ptsEnd is in microseconds and grows on every call as the growing
  // source's own manifest gets refreshed -- that growth, reported live, is
  // what gives real pause/rewind/live-follow instead of the
  // fixed-duration-or-nothing ffmpegdirect route this replaced (see
  // docs/TIMESHIFT.md and docs/RECORDINGS.md).
  //
  // CInputStreamPVRRecording extends the same CInputStreamPVRBase as
  // CInputStreamPVRChannel (confirmed in Kodi-core source), so this same
  // callback drives both a live-timeshift channel and an in-progress
  // recording -- only one of the two is ever open at once, so checking
  // both here is safe and simplest. Checking the *setting*
  // (m_liveTimeshiftMode == kLiveTimeshiftServer) here instead of actual
  // open state was a real bug, not just imprecision: that setting doesn't
  // change once a stream closes, so it stayed true while an in-progress
  // recording was playing with server-side timeshift also enabled,
  // permanently shadowing the recording branch below and reporting
  // GetLiveTimeshiftStreamDurationMs()'s 0 (no live stream open) as ptsEnd
  // instead -- confirmed live: canseek/totaltime stayed false/0 despite
  // GetInProgressRecordingStreamDurationMs() correctly growing every call.
  if (m_client.IsLiveTimeshiftStreamOpen())
  {
    times.SetStartTime(0);
    times.SetPTSStart(0);
    times.SetPTSBegin(0);
    times.SetPTSEnd(m_client.GetLiveTimeshiftStreamDurationMs() * 1000);
    return PVR_ERROR_NO_ERROR;
  }
  if (m_client.IsInProgressRecordingStreamOpen())
  {
    times.SetStartTime(0);
    times.SetPTSStart(0);
    times.SetPTSBegin(0);
    times.SetPTSEnd(m_client.GetInProgressRecordingStreamDurationMs() * 1000);
    return PVR_ERROR_NO_ERROR;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR PVRDispatcharr::GetStreamReadChunkSize(int& chunksize)
{
  // See the declaration comment in PVRDispatcharr.h -- without this, ffmpeg
  // reads 4KB at a time from our HTTP-backed live-timeshift/recording
  // streams, which measurably stalls higher-bitrate channels. 256KB cuts
  // that to a handful of requests per second even for a ~14 Mbps stream,
  // while staying well under a single timeshift segment's typical size so a
  // read still resolves in one HTTP request in the common case.
  chunksize = 256 * 1024;
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// EPG
// ---------------------------------------------------------------------

namespace
{

// Best-effort mapping from freeform XMLTV <category> text (Dispatcharr's own
// categories, or whatever its Schedules Direct/XMLTV EPG sources use --
// there's no fixed vocabulary) to Kodi's ETSI EN 300 468 content-mask genre
// types. Kodi's default skin colour-codes the EPG grid by genre type when
// it's one of these known masks rather than EPG_GENRE_USE_STRING, so this is
// what gets this addon's EPG grid looking like TVHeadend's rather than a
// flat single colour. Scans categories in order and returns the first
// keyword hit found across any of them (not just the first category), since
// Dispatcharr/Schedules Direct commonly lists a non-genre category like
// "Series" before the actually-descriptive ones.
bool MapCategoriesToGenreType(const std::vector<std::string>& categories, int& genreType)
{
  static const std::pair<const char*, int> kKeywordToMask[] = {
      {"movie", EPG_EVENT_CONTENTMASK_MOVIEDRAMA},
      {"film", EPG_EVENT_CONTENTMASK_MOVIEDRAMA},
      {"drama", EPG_EVENT_CONTENTMASK_MOVIEDRAMA},
      {"news", EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS},
      {"current affairs", EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS},
      {"sport", EPG_EVENT_CONTENTMASK_SPORTS},
      {"children", EPG_EVENT_CONTENTMASK_CHILDRENYOUTH},
      {"kids", EPG_EVENT_CONTENTMASK_CHILDRENYOUTH},
      {"youth", EPG_EVENT_CONTENTMASK_CHILDRENYOUTH},
      {"cartoon", EPG_EVENT_CONTENTMASK_CHILDRENYOUTH},
      {"music", EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE},
      {"ballet", EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE},
      {"dance", EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE},
      {"concert", EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE},
      {"arts", EPG_EVENT_CONTENTMASK_ARTSCULTURE},
      {"culture", EPG_EVENT_CONTENTMASK_ARTSCULTURE},
      {"politic", EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS},
      {"social", EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS},
      {"economic", EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS},
      {"documentary", EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE},
      {"science", EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE},
      {"education", EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE},
      {"nature", EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE},
      {"travel", EPG_EVENT_CONTENTMASK_LEISUREHOBBIES},
      {"cooking", EPG_EVENT_CONTENTMASK_LEISUREHOBBIES},
      {"hobbies", EPG_EVENT_CONTENTMASK_LEISUREHOBBIES},
      {"leisure", EPG_EVENT_CONTENTMASK_LEISUREHOBBIES},
      {"game show", EPG_EVENT_CONTENTMASK_SHOW},
      {"talk show", EPG_EVENT_CONTENTMASK_SHOW},
      {"reality", EPG_EVENT_CONTENTMASK_SHOW},
      {"variety", EPG_EVENT_CONTENTMASK_SHOW},
      {"comedy", EPG_EVENT_CONTENTMASK_SHOW},
  };

  for (const std::string& category : categories)
  {
    std::string lower = category;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    for (const auto& [keyword, mask] : kKeywordToMask)
    {
      if (lower.find(keyword) != std::string::npos)
      {
        genreType = mask;
        return true;
      }
    }
  }
  return false;
}

} // namespace

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
    tag.SetEpisodeName(entry.subtitle);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    if (!entry.iconPath.empty())
      tag.SetIconPath(entry.iconPath);
    if (!entry.cast.empty())
      tag.SetCast(entry.cast);
    if (!entry.director.empty())
      tag.SetDirector(entry.director);
    if (!entry.writer.empty())
      tag.SetWriter(entry.writer);
    if (entry.year > 0)
      tag.SetYear(entry.year);
    if (!entry.firstAired.empty())
      tag.SetFirstAired(entry.firstAired);

    if (!entry.categories.empty())
    {
      std::string joinedCategories;
      for (const std::string& category : entry.categories)
      {
        if (!joinedCategories.empty())
          joinedCategories += EPG_STRING_TOKEN_SEPARATOR;
        joinedCategories += category;
      }
      tag.SetGenreDescription(joinedCategories);

      int genreType = EPG_GENRE_USE_STRING;
      MapCategoriesToGenreType(entry.categories, genreType);
      tag.SetGenreType(genreType);
    }

    if (entry.seasonNumber > 0)
      tag.SetSeriesNumber(entry.seasonNumber);
    if (entry.episodeNumber > 0)
      tag.SetEpisodeNumber(entry.episodeNumber);

    unsigned int flags = EPG_TAG_FLAG_UNDEFINED;
    if (entry.isNew)
      flags |= EPG_TAG_FLAG_IS_NEW;
    if (entry.isPremiere)
      flags |= EPG_TAG_FLAG_IS_PREMIERE;
    if (entry.isLive)
      flags |= EPG_TAG_FLAG_IS_LIVE;
    bool categorySaysSeries = std::any_of(entry.categories.begin(), entry.categories.end(),
                                           [](const std::string& c) { return c.find("Series") != std::string::npos; });
    if (entry.seasonNumber > 0 || entry.episodeNumber > 0 || categorySaysSeries)
      flags |= EPG_TAG_FLAG_IS_SERIES;
    tag.SetFlags(flags);

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

  // Third attempt at improving catch-up seek reliability, after the two
  // documented further down (still kept here for history) were tried and
  // reverted -- this one deliberately sets neither ffmpegdirect property
  // either of those set. Confirmed via ffmpegdirect's own source
  // (StreamManager.cpp's Open()): leaving
  // "inputstream.ffmpegdirect.stream_mode" unset at all (neither "catchup"
  // nor "timeshift") makes it instantiate the plain FFmpegStream class
  // instead of FFmpegCatchupStream or TimeshiftStream -- the same base
  // class this addon already routes in-progress-recording playback
  // through. Its SeekTime() calls libavformat's own av_seek_frame() against
  // the mpegts demuxer directly, rather than the generic
  // CCurlFile-plus-bitrate-estimate seek Kodi-core falls back to on its own
  // (byte offset computed first from duration/filesize outside any
  // format-specific logic, then handed to FFmpeg to resync) when no
  // PVR_STREAM_PROPERTY_INPUTSTREAM is set at all -- the plain STREAMURL
  // path set above, still what plays when this setting is off.
  //
  // Requires the separate inputstream.ffmpegdirect addon to actually be
  // installed; if it isn't, this would fail to open the stream at all, so
  // it's opt-in (enable_catchup_ffmpegdirect_seek, default off) rather than
  // silently changed for everyone.
  //
  // Verified live against a real instance, both directions, several times:
  // seeks land precisely (within ~10-15s of the requested target, e.g. a
  // seek to 18:20 landing at 18:09, one to 5:00 landing at 5:15) and
  // playback resumes and continues normally afterward -- a real
  // improvement over the plain-STREAMURL path's known-imprecise byte-
  // estimation seeking.
  //
  // open_mode is deliberately left unset, deferring to ffmpegdirect's own
  // DEFAULT-mode detection, which lands on OpenMode::CURL for a plain
  // http:// URL with this mimetype. A companion session's macOS testing
  // found a credible explanation for this path's intermittent
  // multi-second-to-85+-second seek latency: OpenMode::CURL means
  // ffmpegdirect's I/O still goes through Kodi-core's own CCurlFile/
  // CFileCache rather than an independent connection, and a real macOS log
  // showed libavformat's mpegts demuxer's normal PCR-probe seek algorithm
  // (~15-30 probe-and-adjust reads, expected for a format with no real
  // index) paying CFileCache's own "cache completely reset for seek to
  // position X" cost on every single probe before the seek finally landed.
  // Forcing open_mode to "ffmpeg" was tried as the fix (matching the
  // in-progress-recording HLS path, for the same reasoning: bypass
  // Kodi-core's cache layer by having FFmpeg's own native http:// protocol
  // handler own the I/O instead) -- and made things measurably worse in
  // direct, patient live testing on Windows, not better. A forward seek
  // that would typically land within ~10-20s under CURL mode (worst case
  // observed: 85+s) instead sat completely unmoved for nearly 5 minutes
  // (280s) under forced "ffmpeg" mode before finally landing 68 seconds off
  // target (21:08 for a 20:00 request) -- both slower to resolve and less
  // precise once it did, confirmed via patient polling specifically
  // designed not to repeat the mistake of giving up too early (a first,
  // shorter attempt at this same test was called "stuck" after only 60s,
  // which in hindsight wasn't long enough to tell the difference between
  // "slow" and "actually stuck" -- a real lesson from this investigation:
  // this path's seeks need patience on the order of minutes, not seconds,
  // before concluding anything). Reverted for that reason and left here as
  // a documented dead end -- the *diagnosis* of why CURL mode is
  // occasionally slow is still credible, but this particular fix for it
  // isn't, so a future attempt shouldn't retry forcing "ffmpeg" mode
  // without knowing it was already tried and made things worse.
  if (m_enableCatchupFfmpegdirectSeek)
  {
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
    properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
  }

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
    // Groups recordings into a per-show folder in Kodi's own recordings UI.
    // rec.title is already the show name, not an episode-specific one --
    // confirmed against Dispatcharr's own source: the exact same
    // custom_properties.program.title read that populates this field is
    // also, verbatim, what Dispatcharr itself uses as the show-folder
    // path segment when it writes the file to disk (apps/channels/
    // tasks.py's _build_output_paths), so this always matches the real
    // on-disk layout rather than risking a second, possibly-divergent
    // opinion about what the "show" is. Never empty -- rec.title already
    // falls back to "Recording <id>" server-side when nothing else is
    // available, so a one-off/unmatched recording gets its own
    // single-item folder rather than an empty Directory, matching normal
    // Kodi PVR/video-library grouping conventions.
    recording.SetDirectory(rec.title);
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
  // Confirmed against a real failed playback (a live kodi.log showed
  // Kodi's generic CCurlFile opening a populated STREAMURL directly,
  // bypassing this addon's own OpenRecordedStream() entirely, including
  // its 401-retry/API-key-regen logic) that leaving STREAMURL unset is
  // what actually forces Kodi through CInputStreamPVRRecording's
  // OpenRecordedStream()/ReadRecordedStream()/etc. below -- true for a
  // completed recording, and, since the growing-buffer approach proven
  // for live-timeshift replaced the old ffmpegdirect-routed in-progress
  // mechanism, now equally true for an in-progress one: see
  // DispatcharrClient::OpenInProgressRecordingStream()'s comment for why
  // this no longer needs its own STREAMURL/inputstream.ffmpegdirect
  // properties at all.
  bool isRealTime = false;
  {
    int id = std::atoi(recording.GetRecordingId().c_str());
    std::vector<Recording> recordings;
    std::string error;
    if (m_client.GetRecordings(recordings, error))
    {
      for (const auto& rec : recordings)
      {
        if (rec.id == id)
        {
          // hlsDirStillPresent alongside isInProgress: see its own comment
          // in DispatcharrClient.h -- a just-stopped recording still needs
          // the growing-buffer path (and is therefore still "real-time" in
          // the sense Kodi cares about here) for the whole window until
          // Dispatcharr's own HLS-to-MKV concat actually finishes.
          isRealTime = rec.isInProgress || rec.hlsDirStillPresent;
          break;
        }
      }
    }
  }
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, isRealTime ? "true" : "false");
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

PVR_ERROR PVRDispatcharr::GetRecordingEdl(const kodi::addon::PVRRecording& recording,
                                          std::vector<kodi::addon::PVREDLEntry>& edl)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::vector<RecordingEdlEntry> entries;
  std::string error;
  if (!m_client.GetRecordingEdl(id, entries, error))
  {
    // Not installing the companion recording_edl plugin is an entirely
    // normal, expected configuration (unlike the timeshift plugin, this
    // one has no setting gating it, so most installs simply won't have
    // it) -- log at DEBUG rather than ERROR so declining to install an
    // optional plugin doesn't read as a real problem in the log.
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: no EDL for recording %d: %s", id, error.c_str());
    return PVR_ERROR_NO_ERROR; // empty edl -- not a failure, just nothing to show
  }
  for (const auto& entry : entries)
  {
    kodi::addon::PVREDLEntry e;
    e.SetStart(entry.startMs);
    e.SetEnd(entry.endMs);
    e.SetType(static_cast<PVR_EDL_TYPE>(entry.type));
    edl.emplace_back(std::move(e));
  }
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  std::string keyBefore = m_client.GetApiKey();

  // Check current in-progress status directly rather than trusting
  // GetRecordingStreamProperties()'s own check from moments earlier: a
  // recording that finishes in the gap between that call and this one
  // should still open correctly either way (both paths handle a
  // recording that finishes mid-session -- OpenRecordingStream() simply
  // isn't the right one to have started with if it was in progress right
  // now).
  bool inProgress = false;
  bool hlsDirStillPresent = false;
  {
    std::vector<Recording> recordings;
    std::string recError;
    if (m_client.GetRecordings(recordings, recError))
    {
      for (const auto& rec : recordings)
      {
        if (rec.id == id)
        {
          inProgress = rec.isInProgress;
          hlsDirStillPresent = rec.hlsDirStillPresent;
          break;
        }
      }
    }
  }

  // Route through the growing-buffer reader whenever the HLS directory is
  // still there, not just while Dispatcharr's own status still says
  // "recording" -- see hlsDirStillPresent's own comment in
  // DispatcharrClient.h for why those two go false at very different times
  // (status flips immediately on stop; the HLS-to-MKV concat that has to
  // finish before there's a real, stable file to byte-range against can
  // take real time afterward). Reported live: opening a just-stopped
  // recording during that window either errored outright (no file yet) or
  // played without seeking (a real file existed but was still being
  // actively written by the concat, an unstable Content-Length the
  // completed-recording path was never built to tolerate).
  bool useGrowingBuffer = inProgress || hlsDirStillPresent;

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharrai: OpenRecordedStream: rawId=%s parsedId=%d inProgress=%d "
            "hlsDirStillPresent=%d",
            recording.GetRecordingId().c_str(), id, inProgress ? 1 : 0, hlsDirStillPresent ? 1 : 0);
  bool opened = useGrowingBuffer ? m_client.OpenInProgressRecordingStream(id, error)
                                  : m_client.OpenRecordingStream(id, error);
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: OpenRecordedStream: opened=%d isInProgressStreamOpen=%d",
            opened ? 1 : 0, m_client.IsInProgressRecordingStreamOpen() ? 1 : 0);
  if (!opened)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to open recording %d: %s", id, error.c_str());
    return false;
  }
  // OpenRecordingStream()/OpenInProgressRecordingStream() may have silently
  // regenerated the API key (see OpenRecordingStream()'s comment) if
  // another Kodi install using this same Dispatcharr account had
  // invalidated the one persisted here. Save the new one so a restart of
  // this install doesn't immediately invalidate it again.
  std::string keyAfter = m_client.GetApiKey();
  if (keyAfter != keyBefore)
    kodi::addon::SetSettingString("api_key", keyAfter);
  return true;
}

void PVRDispatcharr::CloseRecordedStream()
{
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharrai: CloseRecordedStream: isInProgressStreamOpen=%d",
            m_client.IsInProgressRecordingStreamOpen() ? 1 : 0);
  if (m_client.IsInProgressRecordingStreamOpen())
    m_client.CloseInProgressRecordingStream();
  else
    m_client.CloseRecordingStream();
}

int PVRDispatcharr::ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
  if (m_client.IsInProgressRecordingStreamOpen())
    return m_client.ReadInProgressRecordingStream(buffer, size);

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
  if (m_client.IsInProgressRecordingStreamOpen())
    return m_client.SeekInProgressRecordingStream(position, whence);
  return m_client.SeekRecordingStream(position, whence);
}

int64_t PVRDispatcharr::LengthRecordedStream()
{
  if (m_client.IsInProgressRecordingStreamOpen())
    return m_client.GetInProgressRecordingStreamLength();
  return m_client.GetRecordingStreamLength();
}

// ---------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
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

  // Backed by Dispatcharr's own RecurringRecordingRule model/scheduler
  // (see DispatcharrClient::CreateRecurringRule()) -- a fixed weekly
  // time-of-day pattern, not EPG-title matching, so no
  // SUPPORTS_TITLE_EPG_MATCH here (unlike the series type above).
  // SUPPORTS_ENABLE_DISABLE is now wired up via UpdateTimer() ->
  // DispatcharrClient::UpdateRecurringRule() (a pre-existing gap when
  // this addon didn't implement UpdateTimer() at all -- see
  // docs/RECORDINGS.md's UpdateTimer entry for how that was closed);
  // deleting the timer (DeleteTimer()) remains the way to stop one for
  // good, rather than just disabling it.
  kodi::addon::PVRTimerType recurring;
  recurring.SetId(kTimerTypeRecurring);
  recurring.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                          PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME |
                          PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS | PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY |
                          PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE);
  recurring.SetDescription("Recurring recording (day-of-week)");
  types.push_back(recurring);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetTimersAmount(int& amount)
{
  std::vector<Recording> recordings;
  std::vector<TimerRule> rules;
  std::vector<RecurringRule> recurringRules;
  std::string error;
  m_client.GetRecordings(recordings, error);
  m_client.GetTimerRules(rules, error);
  m_client.GetRecurringRules(recurringRules, error);
  int scheduled = static_cast<int>(std::count_if(
      recordings.begin(), recordings.end(),
      [](const Recording& r) { return r.isInProgress || r.isUpcoming; }));
  amount = scheduled + static_cast<int>(rules.size()) + static_cast<int>(recurringRules.size());
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
      // Links this one occurrence back to its parent recurring rule (see
      // the recurring-rules loop below) as a Kodi PVR_TIMER child -- the
      // standard Kodi PVR convention for a repeating timer's individual
      // materialized instances (kodi-dev-kit's PVR_TIMER_NO_PARENT is 0,
      // SetParentClientIndex()'s own default, so this is a plain
      // stand-alone one-time timer when recurringRuleId is 0).
      if (rec.recurringRuleId != 0)
        timer.SetParentClientIndex(static_cast<unsigned int>(rec.recurringRuleId) |
                                   kRecurringRuleIndexFlag);
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

  std::vector<RecurringRule> recurringRules;
  if (m_client.GetRecurringRules(recurringRules, error))
  {
    int offsetSeconds = EffectiveRecurringRuleUtcOffsetMinutes() * 60;
    for (const auto& rule : recurringRules)
    {
      kodi::addon::PVRTimer timer;
      // Recurring rules have a real numeric id (unlike series rules
      // above), so it's used directly rather than hashed -- just needs
      // its own namespace bit so it can never collide with a plain
      // one-time recording's own id (kTimerTypeOneTime above) or a
      // hashed series-rule index.
      timer.SetClientIndex(static_cast<unsigned int>(rule.id) | kRecurringRuleIndexFlag);
      timer.SetTimerType(kTimerTypeRecurring);
      timer.SetTitle(rule.name.empty() ? ("Recurring recording " + std::to_string(rule.id))
                                       : rule.name);
      timer.SetClientChannelUid(rule.channelId);
      // Dispatcharr's days_of_week (0=Monday..6=Sunday) already matches
      // Kodi's own PVR_WEEKDAY_MONDAY=(1<<0)..SUNDAY=(1<<6) bit order --
      // see RecurringRule's own comment.
      unsigned int weekdays = PVR_WEEKDAY_NONE;
      for (int day : rule.daysOfWeek)
      {
        if (day >= 0 && day <= 6)
          weekdays |= (1u << day);
      }
      timer.SetWeekdays(weekdays);
      timer.SetFirstDay(rule.startDate);
      // rule.start/endTimeOfDaySeconds are Dispatcharr-local (its own
      // configured system timezone, not UTC) -- shift back to UTC before
      // combining with the (already-UTC) start date, the inverse of
      // AddTimer()'s own conversion below. Kodi only actually uses the
      // time-of-day portion of these for a repeating timer's display; the
      // date portion (rule.startDate) just needs to be *a* valid day, not
      // necessarily the exact next occurrence.
      timer.SetStartTime(rule.startDate + rule.startTimeOfDaySeconds - offsetSeconds);
      timer.SetEndTime(rule.startDate + rule.endTimeOfDaySeconds - offsetSeconds);
      timer.SetState(rule.enabled ? PVR_TIMER_STATE_SCHEDULED : PVR_TIMER_STATE_DISABLED);
      results.Add(timer);
    }
  }

  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::ComputeRecurringRuleFields(const kodi::addon::PVRTimer& timer,
                                                std::vector<int>& daysOfWeekOut,
                                                int& startSecondsOut, int& endSecondsOut,
                                                time_t& startDateOut, std::string& error)
{
  // Pure integer-arithmetic UTC day/time-of-day math -- every value here
  // (Kodi's GetStartTime()/GetEndTime()/GetFirstDay()) is already UTC
  // (this addon's convention throughout, confirmed consistent with how
  // one-time/series timers are already handled with no conversion), and
  // a UTC time_t's own modulo-86400 gives an exact, DST-free
  // calendar-day/time-of-day split with no gmtime/timegm round-trip
  // needed. The *only* place a real timezone enters is the explicit
  // EffectiveRecurringRuleUtcOffsetMinutes() shift below, bridging to
  // Dispatcharr's own (non-UTC-by-default) system timezone -- computed
  // live for a known zone (see recurring_rule_timezone), or falling back
  // to the plain manual recurring_rule_utc_offset_minutes setting
  // otherwise; see that method's own comment and RecurringRule's comment
  // in DispatcharrClient.h for why an arbitrary zone can't be handled
  // automatically.
  constexpr time_t kSecondsPerDay = 86400;
  auto floorMod = [](time_t a, time_t m) { return ((a % m) + m) % m; };
  auto utcMidnight = [&](time_t t) { return t - floorMod(t, kSecondsPerDay); };
  auto secondsSinceUtcMidnight = [&](time_t t)
  { return static_cast<int>(floorMod(t, kSecondsPerDay)); };

  int offsetSeconds = EffectiveRecurringRuleUtcOffsetMinutes() * 60;
  startSecondsOut = secondsSinceUtcMidnight(timer.GetStartTime()) + offsetSeconds;
  endSecondsOut = secondsSinceUtcMidnight(timer.GetEndTime()) + offsetSeconds;

  time_t firstDay = timer.GetFirstDay();
  if (firstDay <= 0)
    firstDay = time(nullptr); // Kodi didn't supply one -- default to "today"
  startDateOut = utcMidnight(firstDay + offsetSeconds);

  daysOfWeekOut.clear();
  unsigned int weekdays = timer.GetWeekdays();
  for (int day = 0; day <= 6; ++day)
  {
    if (weekdays & (1u << day))
      daysOfWeekOut.push_back(day);
  }
  if (daysOfWeekOut.empty())
  {
    error = "At least one day of the week must be selected";
    return false;
  }
  return true;
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
  else if (timer.GetTimerType() == kTimerTypeRecurring)
  {
    std::vector<int> daysOfWeek;
    int startSeconds = 0, endSeconds = 0;
    time_t startDate = 0;
    if (!ComputeRecurringRuleFields(timer, daysOfWeek, startSeconds, endSeconds, startDate, error))
    {
      ok = false;
    }
    else
    {
      time_t endDate = startDate + static_cast<time_t>(kRecurringRuleWindowDays) * 86400;
      ok = m_client.CreateRecurringRule(static_cast<int>(timer.GetClientChannelUid()),
                                        timer.GetTitle(), daysOfWeek, startSeconds, endSeconds,
                                        startDate, endDate, error);
    }
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
  // for this. Recurring rules are excluded the same way series rules
  // are: no Recording exists yet right after creation either -- the
  // first one only appears once Dispatcharr's own hourly scheduler task
  // materializes it, not synchronously here.
  if (timer.GetTimerType() != kTimerTypeSeries && timer.GetTimerType() != kTimerTypeRecurring)
  {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      TriggerRecordingUpdate();
    }).detach();
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  bool isSeries = (timer.GetClientIndex() & 0x40000000) != 0;
  bool isRecurring = (timer.GetClientIndex() & kRecurringRuleIndexFlag) != 0;
  std::string error;
  bool ok;
  if (isSeries)
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(timer.GetClientChannelUid()));
    std::string tvgId = ch ? ch->tvgId : "";
    // Upsert semantics, same call AddTimer() uses to create one --
    // confirmed against Dispatcharr's own source that re-POSTing with
    // the same identity (title + tvg_id) edits mode/title_mode/
    // description/etc. of the existing rule in place rather than
    // creating a duplicate; there is no PATCH-by-id route since series
    // rules have no id at all. If the title (or the channel, which
    // changes tvgId) genuinely changed from what this rule was
    // originally created with, this instead creates a *separate* rule
    // under the new identity -- the old one is left behind untouched,
    // not renamed. Not worked around here: Dispatcharr's own identity
    // key is title+tvg_id+epg_source_id by design, and Kodi's own series
    // timer dialog doesn't meaningfully support "rename this rule" as a
    // normal workflow to begin with.
    ok = m_client.CreateSeriesRule(static_cast<int>(timer.GetClientChannelUid()), tvgId,
                                   timer.GetTitle(), timer.GetPreventDuplicateEpisodes() != 0,
                                   error);
  }
  else if (isRecurring)
  {
    int ruleId = static_cast<int>(timer.GetClientIndex() & ~kRecurringRuleIndexFlag);
    std::vector<int> daysOfWeek;
    int startSeconds = 0, endSeconds = 0;
    time_t startDate = 0;
    if (!ComputeRecurringRuleFields(timer, daysOfWeek, startSeconds, endSeconds, startDate, error))
    {
      ok = false;
    }
    else
    {
      // This is also how Kodi's "enable/disable" timer action reaches a
      // recurring rule -- GetTimerTypes() declares
      // PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE for this type specifically
      // so that action is offered at all, and Kodi implements it by
      // calling UpdateTimer() with everything else unchanged and just
      // GetState() flipped, not a separate dedicated call.
      bool enabled = timer.GetState() != PVR_TIMER_STATE_DISABLED;
      ok = m_client.UpdateRecurringRule(ruleId, static_cast<int>(timer.GetClientChannelUid()),
                                        timer.GetTitle(), daysOfWeek, startSeconds, endSeconds,
                                        startDate, enabled, error);
    }
  }
  else
  {
    // One-time (manual or EPG-based) recording. Deliberately doesn't
    // touch title/custom_properties -- see UpdateOneTimeRecording()'s
    // own comment for why (a real crash risk on a bare partial PATCH,
    // and this mirrors CreateOneTimeRecording()'s own choice not to
    // stomp Dispatcharr's auto-enrichment).
    int id = static_cast<int>(timer.GetClientIndex());
    ok = m_client.UpdateOneTimeRecording(id, timer.GetStartTime(), timer.GetEndTime(), error);
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to update timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  TriggerTimerUpdate();
  // A rescheduled one-time recording (or a recurring rule's own edit,
  // which can add/remove materialized occurrences) changes what
  // GetRecordings() would return too, same reasoning as AddTimer()'s own
  // trigger -- series rules have no recording-list-visible effect from
  // an edit alone (evaluation is a separate, explicit step).
  if (!isSeries)
    TriggerRecordingUpdate();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  bool isSeries = (timer.GetClientIndex() & 0x40000000) != 0;
  // Only ever set on a recurring rule's own parent timer (see GetTimers())
  // -- one of its individual materialized child instances keeps a plain,
  // unflagged ClientIndex (just the underlying Recording's own id), so it
  // falls through to the ordinary one-time delete path below like any
  // other Recording.
  bool isRecurring = (timer.GetClientIndex() & kRecurringRuleIndexFlag) != 0;
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
  else if (isRecurring)
  {
    // Deleting the rule also purges its future materialized recordings
    // server-side (confirmed against Dispatcharr's source:
    // RecurringRecordingRuleViewSet.perform_destroy calls
    // purge_recurring_rule_impl), so the TriggerRecordingUpdate() below
    // (fires for anything other than a series rule) correctly reflects
    // those disappearing too, not just the rule itself.
    int ruleId = static_cast<int>(timer.GetClientIndex() & ~kRecurringRuleIndexFlag);
    ok = m_client.DeleteRecurringRule(ruleId, error);
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
