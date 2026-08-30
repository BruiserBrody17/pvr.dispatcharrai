#include "PVRDispatcharr.h"

#include <kodi/AddonBase.h>

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
  return config;
}

PVRDispatcharr::PVRDispatcharr(const kodi::addon::IInstanceInfo& instance)
    : CInstancePVRClient(instance), m_client(LoadConfigFromSettings())
{
  m_channelRefreshHours = kodi::addon::GetSettingInt("channel_refresh_hours", 12);
  m_epgRefreshHours = kodi::addon::GetSettingInt("epg_refresh_hours", 4);
  m_channelSwitchDelaySeconds = kodi::addon::GetSettingInt("channel_switch_delay_seconds", 0);
  m_enableLiveTimeshift = kodi::addon::GetSettingBoolean("enable_live_timeshift", false);
  m_debugLogging = kodi::addon::GetSettingBoolean("debug_logging", false);

  std::string error;
  if (!m_client.EnsureAuthenticated(error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: initial login failed: %s", error.c_str());
  }
}

PVRDispatcharr::~PVRDispatcharr() = default;

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
  amount = static_cast<int>(
      std::count_if(recordings.begin(), recordings.end(),
                    [](const Recording& r) { return !r.isInProgress && !r.isUpcoming; }));
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
    // Scheduled and in-progress recordings are surfaced via GetTimers()
    // instead; only fully-finished ones belong here (must match
    // GetRecordingsAmount()'s filter above).
    if (rec.isInProgress || rec.isUpcoming)
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
  int id = std::atoi(recording.GetRecordingId().c_str());
  properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, m_client.GetRecordingStreamUrl(id));
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
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
  oneTime.SetDescription("One-time recording");
  types.push_back(oneTime);

  kodi::addon::PVRTimerType series;
  series.SetId(kTimerTypeSeries);
  series.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                       PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH);
  series.SetDescription("Record series (all episodes, via Dispatcharr series rule)");
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
      // Offset series-rule ids away from recording ids so client indices
      // stay unique across both sources.
      timer.SetClientIndex(static_cast<unsigned int>(rule.id) | 0x40000000);
      timer.SetTimerType(kTimerTypeSeries);
      timer.SetTitle(rule.title);
      timer.SetClientChannelUid(rule.channelId);
      timer.SetState(PVR_TIMER_STATE_SCHEDULED);
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
                                   timer.GetTitle(), error);
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
    ok = m_client.DeleteRecording(id, error);
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharrai: failed to delete timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  TriggerTimerUpdate();
  return PVR_ERROR_NO_ERROR;
}
