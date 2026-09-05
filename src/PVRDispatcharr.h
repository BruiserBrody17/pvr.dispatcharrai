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
#include "XmlTvParser.h"

class PVRDispatcharr : public kodi::addon::CInstancePVRClient
{
public:
  explicit PVRDispatcharr(const kodi::addon::IInstanceInfo& instance);
  ~PVRDispatcharr() override;

  // Called from CAddonDispatcharr::SetSetting() (addon.cpp) -- the
  // addon-base-level callback Kodi invokes once per changed setting when
  // the user edits addon settings via the GUI, without restarting Kodi.
  // Not a CInstancePVRClient override; there's no per-instance equivalent
  // wired into the PVR C++ API, only the addon-base one, so
  // CAddonDispatcharr forwards to whichever instance it created. Updates
  // the specific atomic member(s) named in settings.xml that this addon
  // can safely apply live; returns ADDON_STATUS_NEED_RESTART for anything
  // it can't (the Dispatcharr connection settings, which are baked into
  // DispatcharrClient's Config at construction, and
  // enable_realtime_updates, which would need to dynamically start/stop a
  // background thread -- deliberately left out of scope here).
  ADDON_STATUS OnAddonSettingChanged(const std::string& settingName,
                                     const kodi::addon::CSettingValue& settingValue);

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
  // Kodi's ffmpeg demuxer defaults its AVIO read buffer to a hardcoded 4096
  // bytes (CDVDDemuxFFmpeg::CreateDemuxer, DVDDemuxFFmpeg.cpp) unless the
  // PVR client overrides this -- and every read against our live-timeshift
  // or recording streams costs one full HTTP round trip to the timeshift
  // plugin's file server, so 4KB reads meant needing hundreds of requests
  // per second to sustain a high-bitrate channel, causing periodic
  // stall/rebuffer cycles on higher-bitrate channels. Applies to both live
  // (server-side timeshift mode) and recording playback, both of which go
  // through this same CInputStreamPVRBase-backed path.
  PVR_ERROR GetStreamReadChunkSize(int& chunksize) override;

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
  PVR_ERROR GetRecordingEdl(const kodi::addon::PVRRecording& recording,
                            std::vector<kodi::addon::PVREDLEntry>& edl) override;
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
  PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override;

private:
  static constexpr int kTimerTypeOneTime = 1;
  static constexpr int kTimerTypeSeries = 2;
  static constexpr int kTimerTypeOneTimeEpgBased = 3;
  static constexpr int kTimerTypeRecurring = 4;
  static constexpr int kLiveTimeshiftOff = 0;
  // Removed once server-side timeshift proved stable (one less choice, one
  // less separate-addon dependency for live playback), then reintroduced:
  // server-side turned out to have a hard requirement -- a real Dispatcharr
  // admin account -- that's a blanket restriction in Dispatcharr's own
  // plugin run/ API (see docs/TIMESHIFT.md's "permission requirement"
  // section), not something this addon or its companion plugin can loosen.
  // Local fills that gap for anyone who doesn't want to grant admin access:
  // real pause/rewind, entirely client-side via inputstream.ffmpegdirect's
  // own on-device buffer, no Dispatcharr-side cooperation at all. Value 1
  // was deliberately left unreused during the removal rather than
  // renumbering kLiveTimeshiftServer down to 1, specifically so it could be
  // safely reused here without an existing install's persisted `2` ever
  // meaning something different.
  static constexpr int kLiveTimeshiftLocal = 1;
  static constexpr int kLiveTimeshiftServer = 2;
  // ClientIndex namespace bits: series rules already use 0x40000000 (see
  // GetTimers()); recurring rules use a separate bit so a real Dispatcharr
  // id (unlike series rules, which have none and must be hashed) can be
  // used directly without colliding with either namespace.
  static constexpr unsigned int kRecurringRuleIndexFlag = 0x20000000u;
  // How many days ahead a recurring rule's Dispatcharr-side end_date is
  // kept, both at creation and via the periodic renewal below. Kodi's own
  // repeating-timer UI has no "last day" field to expose per-timer
  // (confirmed against kodi-dev-kit's PVR_TIMER_TYPE_SUPPORTS_* flags --
  // only SUPPORTS_FIRST_DAY exists, no equivalent for an end), but
  // Dispatcharr's serializer requires a real end_date on every create --
  // see CreateRecurringRule(). This used to be a flat 3-year end_date on
  // the theory that Dispatcharr only lazily materializes ~14 days ahead
  // regardless of how far out end_date sits, making a far-future value
  // "free". Confirmed live that theory was wrong: Dispatcharr eagerly
  // materializes *every* occurrence between start_date and end_date
  // synchronously, right when the rule is created (or its end_date is
  // changed) -- a single weekly rule with a 3-year end_date produced 157
  // real Recording rows immediately, which would show as 157 child timers
  // in Kodi's own timer list. A much shorter rolling window, kept topped
  // up by RenewRecurringRules() below, keeps that cost bounded while still
  // behaving like a "create once, forget about it" repeating timer from
  // the user's side -- deleting the timer in Kodi still ends it for good
  // at any point; there's no separate "stop repeating" concept to expose
  // beyond that.
  static constexpr int kRecurringRuleWindowDays = 30;
  // RenewRecurringRules() extends a rule's end_date once less than half
  // this window remains, rather than waiting until it's about to actually
  // run out -- keeps the periodic check (which runs on the existing
  // recording-refresh cadence, not a dedicated timer) cheap: most cycles
  // see a rule comfortably inside its window and do nothing at all.
  // Skips a rule with an occurrence currently recording or starting within
  // this many seconds, even though live testing confirmed Dispatcharr's
  // own regeneration on this kind of update already leaves an in-progress
  // or completed occurrence alone (see ExtendRecurringRuleEndDate()'s own
  // comment and docs/RECURRING_RULES.md) -- defense in depth rather than
  // relying solely on that server-side scoping.
  static constexpr int kRecurringRuleRenewalSafetyMarginSeconds = 3600;

  dispatcharr::Config LoadConfigFromSettings() const;

  // Returns the offset (minutes) to actually use for recurring-rule
  // timezone conversion right now. When recurring_rule_timezone is set to
  // one of the zones DispatcharrClient::ComputeKnownZoneOffsetMinutes()
  // recognizes, this is computed fresh on every call (not cached), so it
  // can never go stale across a DST transition the way a value computed
  // once at startup would -- the whole reason this exists instead of just
  // reading m_recurringRuleUtcOffsetMinutes directly. Falls back to that
  // plain manual setting when the zone is "manual" (the default) or
  // unrecognized. Deliberately not cached in a member: both call sites
  // (GetTimers(), AddTimer()/UpdateTimer()'s recurring-rule paths) are
  // synchronous PVR callbacks on Kodi's own thread, not a background loop,
  // so there's no thread-safety reason to cache this the way
  // m_recordingRefreshMinutes needs to for its own polling thread.
  int EffectiveRecurringRuleUtcOffsetMinutes() const;
  // Snapshot of LoadConfigFromSettings()'s result, taken once at
  // construction and updated only by OnAddonSettingChanged() itself below
  // -- exists purely so that method can tell a *genuine* change to a
  // connection setting (host/port/.../api_key, every one of which needs a
  // restart to take effect) apart from a spurious re-notification of the
  // same value. That second case is real, not hypothetical: Kodi has a
  // documented quirk where a settings-dialog save's terminal SetSetting()
  // call can arrive mislabeled with the name of the *last* setting defined
  // in settings.xml (api_key, here) even when nothing about that setting
  // actually changed -- confirmed live against a real CoreELEC install:
  // saving `debug_logging` alone, with nothing else touched, reliably
  // produced a spurious "api_key changed" notification and restarted the
  // PVR client instance every time, defeating live-apply for every
  // setting, not just the connection ones. Comparing against this snapshot
  // turns that spurious case into a no-op instead of an unwanted restart.
  dispatcharr::Config m_lastAppliedConfig;
  // Returns true if this call actually performed a fetch (cache was stale
  // or never loaded), false if it was a no-op (still fresh). The
  // background thread below uses this to know when to call
  // TriggerChannelUpdate()/TriggerEpgUpdate() -- Kodi's own calling
  // threads (GetChannels(), GetEPGForChannel(), ...) ignore it, since
  // they're already inside the callback that answers Kodi's question
  // either way.
  bool EnsureChannelsLoaded();
  bool EnsureEpgLoaded();
  const dispatcharr::Channel* FindChannelByUid(int uid) const;
  // Shared by AddTimer()/UpdateTimer() for kTimerTypeRecurring -- converts
  // Kodi's UTC-based weekday bitmask/start-end-time-of-day/first-day into
  // Dispatcharr's own representation (0-6 day list, its configured-system-
  // timezone-local time-of-day via recurring_rule_utc_offset_minutes, a
  // UTC-midnight start date). Returns false (with error set) only when no
  // weekday is selected at all -- everything else here is pure,
  // infallible conversion.
  bool ComputeRecurringRuleFields(const kodi::addon::PVRTimer& timer,
                                  std::vector<int>& daysOfWeekOut, int& startSecondsOut,
                                  int& endSecondsOut, time_t& startDateOut, std::string& error);

  dispatcharr::DispatcharrClient m_client;

  std::mutex m_dataMutex;
  std::vector<dispatcharr::Channel> m_channels;
  std::vector<dispatcharr::ChannelGroup> m_groups;
  // Keyed by the XMLTV <channel id="..."> value, which is the channel's
  // channel_number (not tvg_id -- see XmlTvParser.h).
  std::unordered_map<std::string, std::vector<dispatcharr::EpgEntry>> m_epgByChannelNumber;

  std::chrono::steady_clock::time_point m_channelsLoadedAt{};
  std::chrono::steady_clock::time_point m_epgLoadedAt{};
  // Every setting below is atomic rather than plain, and updated live by
  // OnAddonSettingChanged() (called via CAddonDispatcharr::SetSetting() in
  // addon.cpp, Kodi's own per-setting change notification) rather than
  // only read once at construction. Confirmed as a real bug otherwise, not
  // just a theoretical one: live_timeshift_mode changed via Kodi's addon
  // settings GUI had no effect on an already-running instance until Kodi
  // was fully restarted -- silently defeating the whole point of
  // switching to Off for a non-admin account, since the addon kept trying
  // the now-stale server-side/admin-only path in the meantime. Each of
  // these is read from more than one thread already (Kodi's own
  // PVR-calling threads plus this addon's background refresh threads), so
  // now that a write can also arrive at any time from whichever thread
  // Kodi delivers SetSetting() on, atomic is the correct minimum fix --
  // simpler and less invasive than threading a mutex through every read
  // site, and sufficient since none of these values have a cross-field
  // invariant that needs a single consistent snapshot.
  std::atomic<int> m_channelRefreshHours{12};
  std::atomic<int> m_epgRefreshHours{4};
  // 0 = off, 1 = local (inputstream.ffmpegdirect's own on-device buffer,
  // no Dispatcharr-side cooperation needed), 2 = server-side (this addon's
  // companion Dispatcharr plugin -- see dispatcharr-plugin/timeshift_buffer/
  // in this repo, see kLiveTimeshiftLocal's own comment for the history of
  // 1 being removed and then reintroduced). Off and Local both exist for
  // the account this addon is configured with NOT being (or the owner not
  // wanting it to be) a Dispatcharr admin -- the timeshift_buffer plugin's
  // run/ API requires that role; a plain stream or an on-device ffmpegdirect
  // buffer both need nothing beyond ordinary channel-browsing/streaming
  // permission. Defaults to Off, not server-side: OpenLiveStream() hard-fails
  // every live channel (see its own comment) when the account isn't admin or
  // the plugin isn't installed, which is exactly the state of a fresh
  // install before anyone's done that extra setup -- Off "just works" out
  // of the box, Local and server-side are both explicit opt-ins.
  std::atomic<int> m_liveTimeshiftMode{kLiveTimeshiftOff};
  std::atomic<bool> m_enableCatchupFfmpegdirectSeek{false};
  std::atomic<bool> m_debugLogging{false};
  // See recurring_rule_utc_offset_minutes in settings.xml/strings.po --
  // bridges Kodi's UTC-based timer times against Dispatcharr's own
  // recurring-rule scheduler, which interprets a rule's start/end
  // time-of-day using its configured (non-UTC-by-default) system
  // timezone with no server-side conversion available. Only actually used
  // as a fallback now, when recurring_rule_timezone is "manual" or an
  // unrecognized zone -- see EffectiveRecurringRuleUtcOffsetMinutes().
  std::atomic<int> m_recurringRuleUtcOffsetMinutes{0};

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
  // Keeps every enabled recurring rule's Dispatcharr-side end_date topped
  // up (see kRecurringRuleWindowDays's own comment for why this exists at
  // all) -- called from the recording-refresh thread's own cadence rather
  // than a dedicated thread/setting, since it needs to run periodically
  // regardless of local activity the same way that thread's existing
  // TriggerRecordingUpdate()/TriggerTimerUpdate() calls do. Best-effort:
  // a failed GetRecurringRules()/GetRecordings()/ExtendRecurringRuleEndDate()
  // call just tries again next cycle, same as the rest of this thread.
  void RenewRecurringRules();
  std::thread m_recordingRefreshThread;
  std::mutex m_recordingRefreshMutex;
  std::condition_variable m_recordingRefreshCv;
  std::atomic<bool> m_stopRecordingRefreshThread{false};
  // Atomic and re-read fresh every wait_for() cycle (see
  // StartRecordingRefreshThread()) -- OnAddonSettingChanged() updating
  // this takes effect on the thread's very next wake, no restart needed.
  std::atomic<int> m_recordingRefreshMinutes{5};

  // Channels/EPG were previously only ever loaded synchronously, on
  // whichever Kodi-owned thread first called GetChannels()/
  // GetEPGForChannel() after the cache went stale (EnsureChannelsLoaded()/
  // EnsureEpgLoaded() above did the actual blocking fetch inline). Fine
  // for a home server with one Dispatcharr instance and a moderate
  // channel count, but a real full XMLTV fetch+parse is genuinely slow
  // enough to be worth moving off Kodi's calling thread. This thread
  // proactively calls the same EnsureChannelsLoaded()/EnsureEpgLoaded()
  // (unchanged -- still the correctness fallback if this thread hasn't
  // caught up yet, e.g. the first moment after construction) on a much
  // shorter cadence than channel_refresh_hours/epg_refresh_hours actually
  // requires, so in steady state the cache is essentially always already
  // warm by the time Kodi asks and those calls become instant, cache-only
  // reads. When a check here does find the cache stale and refreshes it,
  // this thread also calls TriggerChannelUpdate()/
  // TriggerChannelGroupsUpdate()/TriggerEpgUpdate() itself, the same way
  // the recording refresh thread above does for recordings/timers --
  // Kodi's own periodic EPG re-poll would eventually pick this up anyway,
  // but there's no reason to wait for that when this addon already knows
  // the moment new data landed. Started in the constructor, joined in the
  // destructor.
  void StartChannelEpgRefreshThread();
  std::thread m_channelEpgRefreshThread;
  std::mutex m_channelEpgRefreshMutex;
  std::condition_variable m_channelEpgRefreshCv;
  std::atomic<bool> m_stopChannelEpgRefreshThread{false};
  static constexpr int kChannelEpgRefreshCheckMinutes = 10;

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
  // Atomic because OnAddonSettingChanged() now reads-then-writes this (to
  // detect a genuine change vs. the spurious-renotification quirk
  // documented on m_lastAppliedConfig above) on whatever thread Kodi
  // delivers SetSetting() on, not just the constructor's own thread.
  std::atomic<bool> m_enableRealtimeUpdates{false};
};
