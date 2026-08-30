#pragma once

// This targets the kodi-dev-kit C++ PVR API as it stands for Kodi 20
// (Nexus) through 22 (in development at the time of writing). The overall
// data flow (settings -> DispatcharrClient -> Kodi result-sets) is solid,
// but a handful of exact enum/method names on PVRTimerType and PVRCapabilities
// are marked TODO(verify) below and should be checked against your locally
// installed <kodi/addon-instance/PVR.h> on first build -- Kodi's binary PVR
// API is versioned and does drift slightly between releases.

#include <kodi/addon-instance/PVR.h>

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "DispatcharrClient.h"
#include "XmlTvParser.h"

class PVRDispatcharr : public kodi::addon::CInstancePVRClient
{
public:
  explicit PVRDispatcharr(const kodi::addon::IInstanceInfo& instance);
  ~PVRDispatcharr() override;

  // --- General ---
  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;
  PVR_ERROR GetConnectionString(std::string& connection) override;

  // --- Channel groups ---
  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                   kodi::addon::PVRChannelGroupMembersResultSet& results) override;

  // --- Channels ---
  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;
  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                       std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  // --- EPG ---
  PVR_ERROR GetEPGForChannel(int channelUid,
                             time_t start,
                             time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;
  // "Play from guide" for a past/currently-airing programme, backed by
  // Dispatcharr's catch-up/archive feature (see docs/API_NOTES.md) --
  // per-channel and dependent on the upstream provider's own archive, not
  // a continuous rolling live-timeshift buffer for every channel.
  PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable) override;
  PVR_ERROR GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag,
                                      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  // --- Recordings ---
  PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override;
  PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override;
  PVR_ERROR GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording,
                                         std::vector<kodi::addon::PVRStreamProperty>& properties) override;
  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override;

  // --- Timers ---
  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override;
  PVR_ERROR GetTimersAmount(int& amount) override;
  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override;
  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override;

private:
  static constexpr int kTimerTypeOneTime = 1;
  static constexpr int kTimerTypeSeries = 2;

  dispatcharr::Config LoadConfigFromSettings() const;
  void EnsureChannelsLoaded();
  void EnsureEpgLoaded();
  const dispatcharr::Channel* FindChannelByUid(int uid) const;

  dispatcharr::DispatcharrClient m_client;

  std::mutex m_dataMutex;
  std::vector<dispatcharr::Channel> m_channels;
  std::vector<dispatcharr::ChannelGroup> m_groups;
  // Keyed by the XMLTV <channel id="..."> value, which is the channel's
  // channel_number (not tvg_id -- see XmlTvParser.h).
  std::unordered_map<std::string, std::vector<dispatcharr::EpgEntry>> m_epgByChannelNumber;

  std::chrono::steady_clock::time_point m_channelsLoadedAt{};
  std::chrono::steady_clock::time_point m_epgLoadedAt{};
  int m_channelRefreshHours = 12;
  int m_epgRefreshHours = 4;
  int m_channelSwitchDelaySeconds = 0;
  bool m_useFfmpegDirectForCatchup = true;
};
