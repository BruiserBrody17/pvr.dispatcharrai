*(the project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs; check items off and add new ones here as they come up. The 1.0-era release-checklist history this file used to keep below -- version bumps, README/plugin-README rewrites, the four-platform smoke-test pass, release packaging -- was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history if ever needed. "Ongoing" below is the live section: add anything new there, regardless of which release it surfaces in.)*

# Open items

## Ongoing (more will likely come up)

- **Follow-up API survey: three more genuinely implementable findings,
  beyond the recording-management ones below (found 2026-09-08, not
  yet implemented).** Diffed all ~196 of Dispatcharr's real API paths
  (its live `/api/schema/`) against every endpoint this addon actually
  calls (grepped from `src/DispatcharrClient.cpp`), then checked the
  promising gaps against Dispatcharr's real source rather than guessing:
  (1) `GET /api/core/version/` -- public, no auth, `{version,
  timestamp}` -- closes `GetBackendVersion()`'s own documented "no
  confirmed server-version endpoint" gap directly; (2) `GET
  /api/core/timezones/` -- the full ~400+ real IANA timezone list vs.
  this addon's own curated ~25, could broaden `recurring_rule_timezone`
  coverage; (3) Dispatcharr's `SystemNotification` system (`GET
  /api/core/notifications/` etc.) is real and operationally relevant
  (version-update/setting-recommendation/warning/info, with a real
  priority field), not just dev chatter -- a plausible, secondary
  feature to surface high-priority ones as Kodi GUI notifications, no
  design work done beyond confirming it's real. Also checked and ruled
  low-value: `POST .../catchup/sessions/{id}/position/` is purely
  cosmetic for Dispatcharr's own admin dashboard, doesn't affect this
  addon's playback. Also noted: Dispatcharr has a whole separate VOD
  API (`/api/vod/*` -- movies/series/episodes/categories) this addon
  doesn't touch, but that's architecturally out of scope for a
  `kodi.pvrclient`-type addon, not a gap to close here. Full detail in
  `docs/API_NOTES.md`'s table and its new "System notifications" section.
  **Update: a fourth finding, from a broader "what Dispatcharr product
  features exist" pass rather than just an endpoint diff.** Dispatcharr
  supports named, curated channel subsets (`ChannelProfile`/
  `ChannelProfileMembership`, with user accounts assignable to one) --
  a real feature (e.g. a "Kids" or "Sports only" lineup) this addon has
  no way to let a user pick; `GetChannels()` always pulls every channel,
  unfiltered. Confirmed against the real `ChannelViewSet.get_queryset()`
  that this is opt-in filtering (`?channel_profile_id=`), not a
  server-enforced access boundary -- so not implementing it isn't a
  security gap, just a missed curation feature. `GET
  /api/channels/profiles/` lists what's available (empty on this
  particular single-user instance, but the mechanism is real). See
  `docs/API_NOTES.md`'s new "Channel profiles" section.
- **Three recording-management features TVHeadend has that this addon
  doesn't, all confirmed implementable against Dispatcharr's real API
  (found 2026-09-08, not yet implemented).** Recording rename/
  description edit (`POST /api/channels/recordings/{id}/update-metadata/`,
  writes into the exact `custom_properties.program.*` fields already
  read on the way in), recording file size (not a JSON field, but a
  `HEAD` request against `/api/channels/recordings/{id}/file/` gets it
  from `Content-Length`), and extending an in-progress recording (`POST
  .../extend/`, a real dedicated endpoint this addon currently doesn't
  call at all). Rename is the most user-visible and cleanest to add.
  Two other candidates (recording undelete, per-recording retention)
  were checked and ruled out -- Dispatcharr's `DELETE` is immediately
  destructive with no trash table, and there's no retention/lifetime
  concept anywhere in its API. A fourth (resume position/play count)
  has no server-side backing either; only implementable as a weaker,
  addon-local-only hack, not pursued. Full write-up, including how each
  was confirmed against Dispatcharr's actual source (not just its
  OpenAPI schema, which mis-describes this endpoint's request body) in
  `docs/RECORDINGS.md`'s "Recording-management feature gaps vs.
  TVHeadend" section.
- [x] **In-progress recording playback had the same seek-bar
  wall-clock-position bug live timeshift had -- fixed and live-confirmed
  (2026-09-08).** `GetStreamTimes()`'s in-progress-recording branch
  hardcoded `startTime=0`, the exact root cause PR #2 fixed for live
  timeshift. Fixed more simply than the live case: a recording's real
  start time is already known (`PVRRecording::GetRecordingTime()`,
  passed through `OpenRecordedStream()` into
  `OpenInProgressRecordingStream()`), no cold-start-trim anchor needed.
  Confirmed live: seeks in both directions landed at the exact expected
  position during an actual in-progress recording's playback. See
  `docs/TIMESHIFT.md`'s "PVR.TimeshiftProgress*"/seek bar section's
  second "Update" for the full account.
  **Update: also confirmed on CoreELEC/ODROID N2+ (2026-09-09).** Same
  seek-both-directions test, same clean result. Windows and CoreELEC
  now confirmed; macOS/Rocky Linux still untested for this specific
  fix.
- **A consistent ~89.4s audio-sync-error reading on fresh stream opens,
  harmless (2026-09-08).** Seen independently on Windows and Rocky
  Linux (addon 0.9.0 on both), clustered right around -89,400 to
  -89,500ms, only near the start of a stream and never recurring. No
  playback impact on either machine. Purely informational -- not
  chased further. The leading guess (the buffer's own configured
  visible-window duration, `visible_segments * segment_seconds`,
  coincidentally landing in this range) was checked live against
  `timeshift_buffer`'s real source (2026-09-09) and ruled out: that
  formula always reduces to just `buffer_minutes * 60`, three orders
  of magnitude too large for any realistic buffer setting to land near
  89-90s.
  **Update: mechanism found, via Kodi-core's own source (2026-09-09).**
  Reproduced a fourth time (Windows). `ActiveAE.cpp`'s `large audio
  sync error` warning logs the *raw, uncapped* clock-vs-PTS difference
  before it gets clamped to a sane bound for actual use -- right at
  stream open, before the audio clock locks onto a stable reference,
  that raw value can transiently read something enormous, then never
  recurs for the rest of that session. Addon-independent: this is
  Kodi-core's own audio clock bootstrap, not anything this addon does.
  See `docs/TIMESHIFT.md`'s section of the same name for the detail.
- ~~In-progress recording playback never received the catch-up-budget
  hardening that live timeshift already has~~ -- **Already fixed;
  this entry was stale.** When this was written (2026-09-08, after
  finding a substantial *uncommitted* refactor sitting on the Rocky
  Linux laptop's checkout and discarding it per instruction, since its
  authorship/testing history was unknown), the assumption was that the
  in-progress-recording path still had the old, unhardened
  single-segment/1.5x-margin catch-up math. It didn't -- that exact
  refactor (shared `EstimateSegmentDurationMs()`/
  `ComputeCatchUpAttempts()` helpers, 5-segment average, 1500ms floor,
  3x margin) had already been committed to `master` three days
  earlier, in `1ddf3d9` (2026-09-05), and live-verified there (a
  forced tail-seek showed the seek-probe path getting its fast
  1-attempt budget and a genuine catch-up wait getting the full 3x
  budget and succeeding). The Rocky Linux checkout that prompted this
  entry was evidently just behind `master` at the time. That commit
  never touched this file or `docs/TIMESHIFT.md`, which is why the
  gap looked open later -- corrected here, and the fix itself is now
  written up in `docs/TIMESHIFT.md`'s "Catch-up budget hardening
  shared between live and recording playback" section.
- **Recurring, non-fatal `Packet corrupt` on server-side live timeshift,
  post-1.0 (surfaced during 1.0.7 verification, 2026-09-07).** Confirmed
  live on macOS (Channel B 1080p): ffmpeg's mpegts demuxer logs `Packet
  corrupt` roughly once every ~2.5s throughout an otherwise-healthy
  4+ minute playback session (close to the plugin's 2s
  `segment_seconds`), with Kodi's own demuxer evidently resyncing
  cleanly each time -- no stall, no visible playback impact, real seeks
  worked fine. Ruled out as the *same* mechanism as 1.0.7's
  permanent-freeze fix (that fix's own diagnostic, which directly
  detects a segment-size disagreement, never fired once across 134
  occurrences of this). **Refined hypothesis (2026-09-07, still not
  confirmed, deliberately not pursued further):** ffmpeg's `-f segment`
  muxer opens a fresh `AVFormatContext` per segment file -- `-c copy`
  skips re-encoding, but the TS *muxing* layer (including each PID's
  continuity counter) is regenerated fresh per file, normal for
  independently-playable HLS segments but not for this addon's own
  design of concatenating them into one raw byte stream. Real
  corroborating precedent already in this codebase: `-reset_timestamps
  1` was deliberately removed for the exact same class of problem
  (per-segment muxer state resetting), just for PTS continuity instead
  of continuity counters. Deliberately left unfixed: no known ffmpeg
  flag suppresses the reset, and the alternative (binary-patching
  continuity counters at each splice in the live read path) is real
  complexity/risk for a symptom still confirmed cosmetic. See
  `docs/TIMESHIFT.md`'s "1.0.6 follow-up" section's second "Update" note
  for the full reasoning. Not blocking any release -- flagged so it
  doesn't get lost, not because it's currently causing visible harm.
  (Not the same bug as the next item below -- that one *did* trigger
  the size-disagreement diagnostic; this one still hasn't, across 134+
  occurrences.)
  **Update (2026-09-07): stopped being cosmetic once.** First observed
  real failure on macOS (a news channel, one mid-session channel
  switch): two `Stream stalled, start buffering` events, the second with
  a severe, sustained audio desync (`ActiveAE - large audio sync error`
  holding at -8278ms) and `[h264]` decode errors, ending in Kodi's own
  player tearing the stream down on its own (no permanent hang, no
  crash). Same underlying noise, same ~2-4s rate, still not the
  size-disagreement mechanism (zero diagnostic firings this session
  either) -- just the first time it's caused visible harm instead of a
  silent resync. One occurrence, not yet a reliable repro. This is now
  a real, needs-fixing signal rather than a purely theoretical
  "revisit if it stops being cosmetic" trigger. See `docs/TIMESHIFT.md`'s
  "The 'cosmetic' Packet corrupt noise stopped being cosmetic once"
  section. Next step: try to force a live repro deliberately rather
  than waiting on another incidental occurrence.
  **Update (2026-09-08): a 15-minute, 105-switch rapid-channel-cycling
  stress test (Windows, addon 0.9.0) did not reproduce it.** Same
  benign `Packet corrupt` rate as always, zero size-disagreement
  firings, zero errors/crashes, and every audio-sync/buffer-timeout
  warning landed within seconds of a channel switch (normal
  pipeline-reset noise), not a standalone problem. Rapid switching
  specifically doesn't appear to be the trigger -- the original report
  was continuous playback with only one switch. Next attempt should
  try long continuous dwell on a single channel instead. See
  `docs/TIMESHIFT.md`'s same section for the full test breakdown.
  **Update (2026-09-08): a 30-minute continuous Channel B dwell (the
  follow-up angle above) also did not reproduce it.** Same benign
  `Packet corrupt` rate, zero size-disagreement firings, zero audio
  desync, zero stalls, zero `catch-up-to-tail` "gave up" exhaustions
  across the full 30 minutes. Two different stress angles tried now
  (rapid switching and long dwell), neither reproduced it -- still a
  single, unreplicated occurrence. See `docs/TIMESHIFT.md`'s same
  section for the full breakdown.
  **Update (2026-09-08): a second, real occurrence -- this time
  self-recovered.** macOS, addon 0.9.0, ~20-hour continuous session
  (Channel A): a burst of three `Stream stalled` events in ~3.6s,
  audio desync peaking at -5064ms, but this time Kodi's own resync
  machinery pulled it back under threshold within about a second with
  no manual intervention and no user-visible interruption -- unlike the
  first occurrence, which required manually stopping the player. Same
  noise, same rate, zero size-disagreement firings across the full
  session. Two data points now with different outcomes (fatal vs.
  self-recovering), suggesting a second branch point beyond just
  cosmetic-vs-escalates. Still not enough signal to act on. See
  `docs/TIMESHIFT.md`'s same section for the full detail.
  **Update (2026-09-08): a reliable, on-demand trigger found --
  seeking to the live edge, reproduced 3-for-3 in one session
  (macOS).** Severity escalated across the three reproductions,
  peaking at 248,392ms (over 4 minutes) of desync and 439
  hardware-decoder failures in the third one, still not fully
  recovered when the user gave up waiting and restarted. Confirmed
  not a regression from PR #2 (its diff doesn't touch the seek/read
  path). This changes the situation from "wait for it to happen
  incidentally" to "can be deliberately tested" -- worth deciding
  whether to actually chase this now rather than keep just recording
  occurrences. See `docs/TIMESHIFT.md`'s same section for the full
  writeup and the reasoning for why live-edge seeks are a plausible
  trigger (landing on an arbitrary point with no guaranteed clean
  H.264 keyframe/SPS-PPS boundary, unlike a seek into already-settled
  earlier segment data).
  **Update: the segment-wraparound hypothesis is ruled out -- this is
  channel/stream-specific.** Real segment-file-reuse threshold for the
  affected instance is ~10 hours (`buffer_minutes=300`); both real
  incidents happened at 1-5 minutes of buffer age, nowhere close.
  Confirmed on Windows too: 6 live-edge seeks on a 2-minute buffer,
  zero corruption. Controlled trials then found the real variable:
  Channel B (1080p) resisted 4 escalating trials (up to 1,606 decode
  errors, zero audio-sync errors); Channel A (720p) reproduced the
  severe form instantly on the *mildest* method (240 decode errors,
  238 audio-sync errors, 109s peak desync, first attempt). Same
  method/client/machine -- only the channel changed. Next: more Channel A
  trials to confirm, plus a same-resolution comparison channel to
  isolate whether it's resolution or this specific stream's encode.
  **Update: likely root cause found.** A bare `ffprobe` probe of Channel A's raw Dispatcharr proxy stream -- no seek, no Kodi, no addon,
  just a cold TCP connection -- throws ~12 `non-existing PPS 0
  referenced`/`no frame!` errors per second, continuously, for the
  whole 10s test window; Channel B's same probe is essentially clean (1
  unrelated warning). Channel A's keyframe interval is a steady
  2.002s, exactly matching this instance's `segment_seconds=2` setting;
  Channel B's is a steady but unaligned 2.503s. Working theory: Channel A's
  segment-aligned GOP means Dispatcharr's segmenter cuts every segment
  exactly on a keyframe, and something in that exact-alignment path
  corrupts the PPS NAL unit at those cut points almost every time --
  explaining both the worse baseline noise and why a live-edge seek
  (an extra demuxer resync) tips it into the severe cascade so much
  more easily than on Channel B. Points at Dispatcharr's own segmenter for
  this channel's encode, not this addon/Kodi/OS. Not yet confirmed
  against a second segment-aligned channel or a second misaligned one.
  **Update: confirmed via real GUI keypresses, not just synthetic
  seeks.** Peer report -- `StepBack`x7 then `StepForward`x6 (real
  remote/keyboard input, same path as the original Channel B incidents)
  drove a live-edge seek on Channel A and reproduced the identical
  failure signature, peaking ~170s desync, recovering at real-time
  pace. Closes the GUI-vs-JSON-RPC confound. Also: this buffer was
  ~37 minutes old (not freshly opened), so reproducibility on this
  channel doesn't depend on buffer age either.
  **Update: mid-buffer seek control test says this is fixable
  client-side.** Same Channel A buffer, direct A/B: seeking to a
  genuine mid-buffer point produced only ordinary baseline noise (0
  audio-sync-error lines); seeking to the live edge moments later on
  that same buffer immediately produced 2,393 audio-sync-error lines.
  The cascade is specific to the live edge, not the corrupted stream
  in general -- reopens increasing `SeekLiveTimeshiftStream()`'s tail
  backoff margin (currently 1 segment) as a plausible, worth-trying
  client-side mitigation.
  **Update: implemented and tested -- backing off 3 segments instead
  of 1 eliminated the cascade across 12/12 trials on Channel A
  (Windows), vs. reproducing on the 2nd of 6 attempts pre-fix.** Zero
  `large audio sync error` lines in any of 12 attempts; ordinary
  baseline noise unchanged. Not proof it's fully eliminated (12 clean
  trials, not exhaustive), and not yet tested against an older buffer
  or on macOS/CoreELEC -- worth continued normal-use monitoring.
  **Update: macOS confirmation, clean pass.** 10/10 scripted attempts
  (same method as Windows) plus a separate 69-keypress real-keyboard
  session, both on Channel A -- zero `large audio sync error` lines
  in either. Two platforms, two input methods, the one channel that
  reproduced instantly pre-fix, all clean. Buffer age and CoreELEC
  still untested.
  **Update: CoreELEC/ODROID N2+ confirmation, clean pass -- and a
  stronger one than the desktop platforms got (2026-09-09).** 12/12
  live-edge-seek attempts on Channel B (real Channel B channel, not Channel
  A -- deliberately avoided this run), zero `large audio sync error`
  lines in any attempt. Notable: the baseline `non-existing PPS 0
  referenced` noise ran much higher here than Channel B's established
  clean baseline on Windows/macOS (10-269 per ~20s attempt window here
  vs. essentially none there) -- see the new note in
  `docs/TIMESHIFT.md`'s same section. Despite that elevated baseline
  noise, the fix still held with zero escalations, a more demanding
  condition than the earlier clean passes. Only the older-buffer
  condition remains untested now.
- [x] ~~A periodic, self-correcting `ActiveAE::SyncStream` error spike
  on a suspiciously exact ~8.6s cadence~~ -- **Closed: not actually
  periodic, confirmed on both platforms that ever saw it
  (2026-09-08/09).** Originally found by the macOS peer during what
  looked like ordinary steady-state playback. Tested on Windows (14
  min, zero seeking): one occurrence total, landing 4 seconds after
  that session's own ~89.4s large-sync-error transient at the same
  fresh `Player.Open` -- not periodic. Re-tested on macOS itself (21.6
  min, zero seeking, addon 0.9.1): same result, one occurrence ~3.9s
  after buffer open, nothing else. The peer also traced their original
  observation to a mislabeled sample -- those log lines actually landed
  shortly after a batch of scripted live-edge-seek attempts, not during
  clean steady-state; each clamped-to-tail seek forcing its own small
  demuxer-resync transient plausibly explains the "recurring" look
  without any genuine time-based periodicity. Same family as the
  ~89.4s transient above: a one-off clock-settling blip at stream
  open, not a recurring issue. See `docs/TIMESHIFT.md`'s "A periodic,
  self-correcting ~8.6s `ActiveAE::SyncStream` spike" section for the
  full account.
- [x] **A second, related `Packet corrupt`/freeze, confirmed root-caused
  and fixed, then re-verified live (2026-09-07).** Switching away from a
  channel and back could reproduce a real segment-size disagreement --
  this one *did* trigger the 1.0.7 diagnostic live. Root cause: the
  plugin's per-worker manifest cache wasn't invalidated across a buffer
  restart in a multi-worker deployment (a new ffmpeg process reuses the
  same segment filenames/sequence numbers as its predecessor). A first
  fix tied cache entries to the buffer's own process ID -- itself then
  confirmed live to have a gap under heavy testing churn (OS pids get
  recycled; a stale entry tagged with a since-reassigned pid passed the
  check it should have failed). Fixed properly by keying on the
  buffer's own access token instead (a fresh, random value per genuine
  instance, no reuse risk regardless of churn). Re-verified live on
  macOS across two independent repro conditions, including the specific
  churn pattern that broke the pid-based version -- both clean. Shipped
  in `1.0.7` / `timeshift_buffer` `1.0.5`. See `docs/TIMESHIFT.md`'s
  "1.0.7 follow-up #2" section for the full account.
- **All four platforms now have a completed smoke-test pass** (Windows,
  Rocky Linux, CoreELEC/ODROID, macOS) -- Local timeshift mode is
  confirmed live on all four, closing out what was the last real gap in
  platform coverage.
- ~~A genuine, unexplained anomaly from the macOS pass~~ -- **Likely
  already explained and fixed the next day, entry just never
  reconciled (2026-09-08 review).** Original note (2026-09-05):
  `kodi.log` showed a full `UpdateClients: Recreating PVR client`
  (clean DLL unload/reload, no crash, playback fine right after)
  immediately after dismissing a resume-prompt dialog, with no
  settings change involved -- ruling out the known beta.2
  spurious-restart bug (settings-write-triggered), and not
  reproducing on a second attempt. One day later (2026-09-06,
  `899e5a7`), a separate macOS session hit the identical log signature
  -- `UpdateClients: Recreating PVR client`, clean reload, no crash --
  this time while opening an in-progress recording, and that one *was*
  root-caused and fixed: `OpenRecordedStream()`/`ReadRecordedStream()`
  occasionally self-heal a near-expiry API key mid-call and persist it
  via `SetSettingString()`, which loops back through
  `OnAddonSettingChanged()` and (correctly, by that guard's own logic)
  requests a restart, tearing down the stream that had just opened.
  See `docs/RECORDINGS.md`'s "A self-heal API-key regeneration..."
  section for the full account. Dismissing a resume-prompt dialog for
  a recording leads directly into `OpenRecordedStream()` -- the exact
  vulnerable call site -- and "no settings change involved" is exactly
  how a silent, addon-internal self-heal write would look from a
  tester's side; not reproducing on a second attempt also fits, since
  the trigger depends on the API key's natural expiry timing rather
  than firing every time. Not provable with certainty (the original
  log's own API-key-regeneration line isn't preserved to check
  directly), but strong enough that this shouldn't still read as an
  open mystery. Left here, corrected, rather than deleted, for the
  same reason the original was kept: useful context if anything like
  it resurfaces.
- ~~Catch-up and recurring-timer creation have no clean way to exercise
  via Kodi's JSON-RPC alone~~ -- **Decided: counted as tested anyway, not
  worth building a dedicated driving mechanism for.** The underlying
  logic is platform-independent and already has real live-verification
  from earlier sessions; the gap is in this testing technique
  (JSON-RPC-driven, not GUI-driven), not in the feature.
