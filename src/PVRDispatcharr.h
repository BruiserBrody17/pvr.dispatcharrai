#pragma once

// This targets the kodi-dev-kit C++ PVR API as it stands for Kodi 20
// (Nexus) through 22 (in development at the time of writing).

#include <kodi/addon-instance/PVR.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "DispatcharrClient.h"
#include "LocalPlaylistServer.h"
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

  // Server-side timeshift's actual playback path: GetChannelStreamProperties()
  // leaves STREAMURL unset for that mode specifically so Kodi calls these
  // instead of routing through inputstream.ffmpegdirect -- see its own
  // comment and docs/TIMESHIFT.md. Mirrors OpenRecordedStream()/
  // ReadRecordedStream()/SeekRecordedStream()/LengthRecordedStream() below
  // almost exactly, just against DispatcharrClient's live-timeshift-stream
  // methods instead of its recording-stream ones.
  bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override;
  void CloseLiveStream() override;
  int ReadLiveStream(unsigned char* buffer, unsigned int size) override;
  int64_t SeekLiveStream(int64_t position, int whence) override;
  int64_t LengthLiveStream() override;
  bool CanPauseStream() override;
  bool CanSeekStream() override;
  bool IsRealTimeStream() override;
  PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override;

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
  // Backs the "Play live" context-menu entry on in-progress recordings --
  // see m_pendingLiveModeRecordingId's comment for why this exists instead
  // of a mode-choice dialog at Play time.
  PVR_ERROR CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook,
                                  const kodi::addon::PVRRecording& item) override;
  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override;
  // Kodi always demuxes pvr://recordings/... via CInputStreamPVRRecording,
  // which serves the generic FFmpeg demuxer through these -- confirmed
  // against Kodi's own source that it never resolves
  // PVR_STREAM_PROPERTY_STREAMURL from GetRecordingStreamProperties() for
  // this path the way live channels and catch-up work. See
  // DispatcharrClient::OpenRecordingStream().
  bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override;
  void CloseRecordedStream() override;
  int ReadRecordedStream(unsigned char* buffer, unsigned int size) override;
  int64_t SeekRecordedStream(int64_t position, int whence) override;
  int64_t LengthRecordedStream() override;

  // --- Timers ---
  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override;
  PVR_ERROR GetTimersAmount(int& amount) override;
  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override;
  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override;

private:
  static constexpr int kTimerTypeOneTime = 1;
  static constexpr int kTimerTypeSeries = 2;
  static constexpr int kTimerTypeOneTimeEpgBased = 3;
  static constexpr unsigned int kMenuHookPlayLive = 1;
  static constexpr int kLiveTimeshiftOff = 0;
  static constexpr int kLiveTimeshiftLocal = 1;
  static constexpr int kLiveTimeshiftServer = 2;

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
  // 0 = off, 1 = local (inputstream.ffmpegdirect's own on-device buffer),
  // 2 = server-side (this addon's companion Dispatcharr plugin -- see
  // dispatcharr-plugin/timeshift_buffer/ in this repo). Was a plain
  // enable_live_timeshift boolean before the server-side mode existed;
  // kept both live-timeshift.xml/strings ids and this field name for the
  // "off"/"local" cases to stay recognisable, but this is a breaking
  // settings change for anyone with the old boolean already saved -- it
  // just falls back to the new default (0/off) rather than silently
  // mapping true to something (which of the two non-off modes an existing
  // "true" should mean isn't knowable).
  int m_liveTimeshiftMode = 0;
  bool m_enableInProgressPlayback = false;
  bool m_enableCatchupFfmpegdirectSeek = false;
  bool m_debugLogging = false;

  // Backs in-progress recording playback's rewritten-playlist snapshot --
  // see DispatcharrClient::GetInProgressRecordingStreamUrl()'s comment and
  // LocalPlaylistServer.h for why STREAMURL can't just be a data: URI.
  // Started in the constructor whenever m_enableInProgressPlayback is true
  // (not unconditionally -- no reason to hold a loopback listening socket
  // open for installs that never use this feature), stopped in the
  // destructor.
  dispatcharr::LocalPlaylistServer m_playlistServer;

  // Pressing Play on an in-progress recording used to show a blocking
  // "Play live" vs. "Play from start" dialog before returning stream
  // properties -- replaced because cancelling it (Back/Cancel) always made
  // Kodi show its own "Playback failed" dialog on top: confirmed via
  // Kodi-core source that CPVRPlaybackState::StartPlayback() never checks
  // GetRecordingStreamProperties()'s PVR_ERROR return at all, only whether
  // any properties were set, so a deliberate "nothing to play" response is
  // indistinguishable from a genuine failure once it reaches
  // CVideoPlayer::CloseFile(). Plain Play now goes straight to "Play from
  // start" with no prompt; "Play live" moved to a PVR_MENUHOOK_RECORDING
  // context-menu entry (CallRecordingMenuHook()) instead. A binary PVR
  // addon has no API to itself start playback, though, so the hook can't
  // just open the item live directly -- it arms this instead (the
  // recording id it should apply to, or -1 when nothing is armed) and asks
  // the user to press Play right after; GetRecordingStreamProperties()
  // consumes it (one-shot -- reset to -1 whether or not it matched) the
  // next time it's called for that same id, and falls back to "Play from
  // start" for every other case (id mismatch, or nothing armed at all).
  std::mutex m_pendingLiveModeMutex;
  int m_pendingLiveModeRecordingId = -1;

  // Same one-shot arm/consume pattern as m_pendingLiveModeRecordingId
  // above, for the analogous problem on the channel (not recording) side:
  // Recordings/timers only ever get re-fetched by Kodi when this addon
  // calls TriggerRecordingUpdate()/TriggerTimerUpdate() -- unlike channels/
  // EPG above, there's no lazy "check staleness next time Kodi asks"
  // option, since Kodi only asks again once told to. Every existing call
  // site is reactive (right after this addon's own AddTimer()/
  // DeleteTimer()/etc.), so a change that happens with no local Kodi
  // action to react to -- another Kodi install's action, a change made
  // directly against Dispatcharr's API, a scheduled recording finishing on
  // its own -- had no way to ever surface short of restarting Kodi.
  // Confirmed: a recording deleted directly via Dispatcharr's API (not
  // through this addon) kept showing in Kodi indefinitely until restarted.
  // This background thread closes that gap by triggering periodically
  // regardless of local activity. Started in the constructor, joined in
  // the destructor.
  void StartRecordingRefreshThread();
  std::thread m_recordingRefreshThread;
  std::mutex m_recordingRefreshMutex;
  std::condition_variable m_recordingRefreshCv;
  std::atomic<bool> m_stopRecordingRefreshThread{false};
  int m_recordingRefreshMinutes = 5;

  // Real-time alternative/complement to the polling thread above: connects
  // to Dispatcharr's own WebSocket push (ws(s)://host:port/ws/?token=<JWT>,
  // the exact channel its own frontend uses -- no plugin or server-side
  // change needed, confirmed by reading Dispatcharr's own source) and
  // triggers a Kodi refresh the moment a relevant recording/timer event
  // actually happens, instead of waiting out the polling interval. Kept
  // as an addition to, not a replacement for, the polling thread: if the
  // connection can't be established or drops and stays down (a firewall,
  // an older Dispatcharr version without this channel, a network blip
  // outlasting the reconnect backoff), the poll above still gets there
  // eventually. Opt-in (enable_realtime_updates, off by default) since
  // this is genuinely new, non-trivial networking code (a hand-rolled
  // RFC 6455 client -- see WebSocketClient.h for why it isn't just
  // curl's own WebSocket support) that hasn't seen the real-world use
  // everything else in this addon has.
  void StartRealtimeUpdateThread();
  void HandleRealtimeUpdateMessage(const std::string& message);
  std::thread m_realtimeUpdateThread;
  std::mutex m_realtimeUpdateMutex;
  std::condition_variable m_realtimeUpdateCv;
  std::atomic<bool> m_stopRealtimeUpdateThread{false};
  bool m_enableRealtimeUpdates = false;
};
