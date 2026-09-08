*(the project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs; check items off and add new ones here as they come up. Started life as a checklist scoped to the 1.0 release specifically -- that section is kept below as a closed-out historical record, not something to keep adding to. "Ongoing" at the bottom is the live section: add anything new there, regardless of which release it surfaces in.)*

# Open items

## 1.0 release checklist (historical -- closed out, all items done)

### Documentation

- [x] README cleaned up and made concise -- rewritten from 211 lines down
      to install/config/features only, narrative moved out (line count
      has moved some since, as later fixes added a sentence here and
      there -- the point was the rewrite, not a specific number to hold
      it to). Decided
      **not** to physically relocate the existing `docs/*.md` files --
      ~40+ source-code comments (C++ and the Dispatcharr plugins' own
      Python) point at their exact current paths, so moving them would be
      a large, error-prone churn for no real benefit. Instead added a
      one-line disclaimer at the top of `docs/API_NOTES.md` marking that
      whole family as engineering history, not user docs, with a pointer
      back to the main README.
- [x] `dispatcharr-plugin/timeshift_buffer/README.md` (from 186 lines) and
      `dispatcharr-plugin/recording_edl/README.md` (from 226 lines)
      rewritten concise (both have grown back a bit since, as later fixes
      added real content -- see `CHANGELOG.md`/git history for what).
      Their narrative wasn't already covered elsewhere the way
      the main addon's was, so first backfilled the missing history into
      `docs/TIMESHIFT.md` (already had it -- just pointed there) and
      `docs/RECORDING_EDL.md` (didn't yet cover the orphan-sidecar-cleanup
      or `.dvr_*_hls` diagnostics/delete features added this session --
      added those sections), then trimmed each plugin README to
      install/actions/safety-essentials with a pointer back for the "why."
      Kept the safety-critical facts in place (what each destructive
      action does and doesn't touch, the `.dvr_*_hls` classification
      table) rather than treating them as narrative to cut.

### Platform testing

Four platforms: Windows, Rocky Linux, CoreELEC/ODROID, macOS.

**Smoke-test pass, defined**: live playback, recording start/stop,
catch-up, timeshift seek, EPG, recurring timers -- run once per platform
right before release.

| Platform | Status |
|---|---|
| Windows | ✅ Pass -- full 6-item smoke test |
| Rocky Linux | ✅ Pass (catch-up/recurring-timer creation not exercised -- see [Ongoing](#ongoing-more-will-likely-come-up)) |
| CoreELEC / ODROID | ✅ Pass (same catch-up/recurring-timer caveat as Rocky Linux) |
| macOS | ✅ Pass (same catch-up/recurring-timer caveat; one unexplained anomaly noted below) |

#### Windows

Recording start/stop, catch-up, recurring timers, EPG, and live playback
(`Off`, `Local`, and `Server-side` modes) all verified this session against
a fresh build. Live-mode check also confirmed a real, practical thing: an
existing profile's persisted `live_timeshift_mode` value survives the
code-level default change unchanged (`default="true"` in settings.xml does
**not** get re-resolved to the addon's new default on load) -- server-side
kept working exactly as before for this account, Off was separately
confirmed clean (`STREAMURL` set, `canseek: false` as designed, stable
playback, zero errors), and `Local` (reintroduced this session -- see
`docs/TIMESHIFT.md`) was confirmed with an actual seek: backward and
forward both landed correctly (`demuxer seek to: ..., success`,
ffmpegdirect's own `TimeshiftBuffer::Seek` locating the right
segment/packet) and playback resumed cleanly both times. Timeshift *seek*
itself under **server-side** mode specifically wasn't independently
re-verified on Windows this session (only Local and the in-progress-
recording variant were) -- since verified directly on CoreELEC (below)
using the exact same platform-independent C++ path, this is a
documentation gap rather than a suspected regression.

#### Rocky Linux

**Full smoke-test pass completed this session**, against a fresh build of
current `master` (git checkout at `~/kodi-linux-build`, `git reset --hard
origin/master` then rebuilt -- unlike the Windows/CoreELEC trees, this
one's a real clone, no manual file-syncing needed) on real Rocky Linux
10.2 hardware on the local network, Kodi via the official `tv.kodi.Kodi`
Flatpak per `docs/BUILDING.md`. Same stale-build-skip issue as
Windows/CoreELEC hit first (`make: Nothing to be done for 'all'`) --
cleared by removing `.installed-native` and the ExternalProject's cached
`pvr.dispatcharrai-prefix` staging dir, then it rebuilt for real. Launching
Kodi over SSH needed the same display-environment workaround
`docs/BUILDING.md` already documents (`WAYLAND_DISPLAY=wayland-0`,
`DISPLAY=:0`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS` pulled from the
live desktop session). Rocky Linux's default firewall (firewalld) blocks
external access to Kodi's webserver port and modifying that is a
security-settings change this session won't make -- routed every JSON-RPC
call through the SSH connection instead (`curl` against `127.0.0.1:8080`
from the remote side), which needs no config changes at all. Results:
**live playback** -- all three modes (`Off`, `Local`, `Server-side`)
confirmed working, including a real backward seek under both `Local` and
`Server-side` (`demuxer seek to: ..., success`, clean resume both times) --
`inputstream.ffmpegdirect` turned out to already be bundled inside the
Flatpak runtime itself (`/app/lib/kodi/addons/`), not something to
separately install. **EPG** confirmed populated correctly. **Recording
start/stop and immediate post-stop playback** confirmed clean once past
the same known resume-dialog quirk described in the CoreELEC section below
(same fix: `Input.Down` + `Input.Select`) -- this is the second platform
this session where that documented Kodi behavior showed up, reinforcing
it's a generic Kodi/testing-technique thing, not addon- or
platform-specific. **Catch-up and recurring timers**: same JSON-RPC
tooling gap as CoreELEC, not retested here. No JSON-RPC recording-delete
method on this Kodi version either -- one small test recording (~90s CNN
clip) left on the account. Device fully restored (`debug_logging` off,
`live_timeshift_mode` back to `2`, Kodi's global debug logging off) and
Kodi left running normally.

#### CoreELEC / ODROID

**Full smoke-test pass completed this session**, against a fresh
cross-compiled build of current `master` on real ODROID N2+ hardware on
the local network, driven via SSH + Kodi's JSON-RPC webserver the same
way as the Windows tests. A stale local CoreELEC checkout's `package.mk`
was pointed at the current commit and, along the way, surfaced a real bug:
the optional-dependency XML comment added earlier this session used this
project's usual `--` prose separator, which is invalid inside an XML
comment body -- `xmlstarlet` (used by CoreELEC's build, not by Windows/CI)
rejected it outright. Fixed and pushed as a separate commit before
continuing. Results: **live playback** -- all three modes (`Off`, `Local`,
`Server-side`) confirmed working, including a real backward+forward seek
under both `Local` and `Server-side` (`demuxer seek to: ..., success` both
times, clean resume, `canseek: true`) -- this closes the server-side-seek
gap noted in the Windows section above, since it's the same
platform-independent code path. **EPG** confirmed populated correctly.
**Recording start/stop** confirmed at the timer/API level (real-time push
events fired correctly: `recording_started`/`updated`/`stopped`/`ended`)
for both a ~24s and a ~70s test recording, and **immediate post-stop
recording playback** (the exact scenario the `hlsDirStillPresent` fix
targeted) was confirmed clean (`canseek: true`, real advancing
`time`/`totaltime`, correct decoder activity). An earlier attempt on this
same recording appeared stuck (an "OK dialog" window, no decode activity,
`Player.GetActivePlayers` returning empty) -- **root-caused, not just
worked around**: this is [[testing-kodi-jsonrpc-resume-dialog]], a known
Kodi behavior (confirmed again on Rocky Linux above, same fix) where a
resume-prompt dialog blocks all further JSON-RPC until dismissed, and
`Player.Open`'s `resume` flag doesn't suppress it for PVR recordings.
`Input.Down` + `Input.Select` (choosing "Play from beginning") cleared it
and played back cleanly -- not a testing-methodology artifact from rapid
interaction as first guessed, and not an addon or platform defect either
way. **Catch-up** and **recurring timers** were not cleanly exercisable
this pass -- Kodi's JSON-RPC has no generic way to trigger genuine catch-up
playback (a plain `Player.Open` with a past `broadcastid` just re-opens the
live channel) or to create a recurring/day-of-week timer (no
`PVR.GetTimerTypes`, no full-field `AddTimer` for this addon's custom
recurring type) -- both are known, pre-existing tooling limitations from
earlier sessions, not new findings; the underlying code for both is
platform-independent HTTP/JSON logic already exercised on Windows. Two
small test recordings (~24s/~70s CNN clips) were left on the account -- no
JSON-RPC method exists to delete a recording, and using the addon's stored
API key to hit Dispatcharr's REST API directly to delete them was avoided
after a similar direct-API call got blocked by the session's safety
classifier; harmless, but worth a manual cleanup from Dispatcharr's or
Kodi's own UI if desired. Device fully restored to its original settings
(`debug_logging` off, `live_timeshift_mode` back to `2`, Kodi's global
debug logging off) and left at the normal Home screen.

#### macOS

**Full smoke-test pass completed this session**, run by a separate Claude
Code instance on the user's own Mac (relayed back rather than driven
directly from here) against a fresh build of current `master` via Kodi's
`binary-addons` harness per `docs/BUILDING.md` -- succeeded cleanly on a
fresh clone, no stale-cache issue this time. Independently reconfirmed
`docs/BUILDING.md`'s `-DPACKAGE_DIR` caveat -- the zip landed deep inside
the ExternalProject tree regardless, `find` was needed to locate it.
Results: **live playback**, all three `live_timeshift_mode` values
confirmed with a real seek each -- **Off** (`streamurl` set directly,
`canseek: false`), **Local** (`inputstream.ffmpegdirect` already
installed, `canseek: true`, `demuxer seek to: ..., success`),
**Server-side** (just `isrealtimestream`, `canseek: true`,
`SeekLiveTimeshiftStream(...) -> (clamped to tail)` then `demuxer seek
to: ..., success`). **The in-progress-recording catch-up-to-tail fix
from this session's audit (see the top-level commit history) was
deliberately exercised, not just recording start/stop in general** --
`segmentDurationEstimateMs` landed at 3934/3817/4074ms across three
trials (right around Dispatcharr's real ~4000ms HLS segment size), and
real catch-up cycles used a sensible chunk of their budget before
succeeding (17/48, 9/46, 10/49 attempts) rather than collapsing to a tiny
attempt count and giving up, which is the exact crash this fix addressed.
**EPG** confirmed (95 real broadcasts, correct titles/times).
**Recording stop + immediate post-stop playback** confirmed the
`hlsDirStillPresent` scenario directly: `inProgress=0 hlsDirStillPresent=1`,
`opened=1`, playback healthy. The known resume-prompt dialog
([[testing-kodi-jsonrpc-resume-dialog]]) showed up here too -- third
platform this session, same `Input.Down` + `Input.Select` fix, not
addon- or platform-specific. **Catch-up (archive) playback and
recurring-timer creation**: same JSON-RPC tooling gap as CoreELEC/Rocky
Linux, not exercised here either.

**Two macOS-specific findings:**
1. Enabling `services.webserver` without a password triggers a
   security-warning dialog that blocks all JSON-RPC until dismissed via
   real input -- same class of issue as the resume-dialog quirk above
   (a modal dialog stalling the JSON-RPC queue), general Kodi behavior,
   not an addon bug. Worked around by driving the whole test over the
   raw JSON-RPC TCP socket (port 9090) instead of the HTTP webserver.
2. **A genuine, unexplained anomaly, flagged rather than explained
   away**: right as the first resume-prompt dialog was dismissed,
   `kodi.log` showed a full `UpdateClients: Recreating PVR client` --
   a clean DLL unload/reload, no crash, the recording played fine
   immediately after -- but with no settings change involved, which
   rules out the already-fixed beta.2 spurious-restart bug (that one
   was triggered by a settings write). Did not reproduce on a second
   open of the same recording. Worth watching for on a future pass
   rather than assuming it's this same class of "modal dialog" noise --
   noted in Open items below.

One ~5-minute test recording left on the account (no JSON-RPC delete
method exists, same gap as every other platform). Device fully restored
to its original settings and left running normally.

### Release packaging

- [x] Kodi addon zips for all 4 platforms -- already automated
      (`.github/workflows/build.yml`), attached to GitHub Releases on every
      version tag. Confirmed live: `1.0.0-beta.3` has all four
      (`*-linux.zip`, `*-osx-arm64.zip`, `*-windows-x86_64.zip`,
      `*-coreelec-arm.zip`) as real downloadable assets.
- [x] Zip packaging for `dispatcharr-plugin/timeshift_buffer` and
      `dispatcharr-plugin/recording_edl` -- new `package-dispatcharr-plugins`
      job in `.github/workflows/build.yml`, attached to the same GitHub
      Release as the addon zips on the same version tag. Confirmed live via
      `gh run watch` (all steps green) and by downloading the actual CI
      artifact and inspecting it with `unzip -l`: each zip's top-level entry
      is exactly the plugin's own directory name
      (`timeshift_buffer/`/`recording_edl/`) containing only
      `plugin.py`/`plugin.json`/`README.md` -- no stray `__pycache__`, no
      extra nesting.

## Ongoing (more will likely come up)

- **A consistent ~89.4s audio-sync-error reading on fresh stream opens,
  harmless (2026-09-08).** Seen independently on Windows and Rocky
  Linux (addon 0.9.0 on both), clustered right around -89,400 to
  -89,500ms, only near the start of a stream and never recurring. No
  playback impact on either machine. Purely informational -- not
  chased further. See `docs/TIMESHIFT.md`'s section of the same name
  for the detail and a leading (unconfirmed) guess at the mechanism.
- **In-progress recording playback never received the catch-up-budget
  hardening that live timeshift already has -- worth revisiting
  (found 2026-09-08, discarded uncommitted, not yet redone).** A real,
  substantial uncommitted refactor was found sitting on the Rocky Linux
  laptop's checkout (never pushed anywhere, origin unknown -- likely
  from a prior session that worked directly on that machine) that
  extracted two shared helper functions,
  `EstimateSegmentDurationMs(totalDurationMs, segments)` and
  `ComputeCatchUpAttempts(likelySeekProbe, segmentDurationEstimateMs,
  catchUpSleepMs)`, so both `ReadLiveTimeshiftStream()` *and*
  `ReadInProgressRecordingStream()` share the exact same catch-up
  budget calculation. The live path already has this hardening (see
  the "1.0.7 follow-up" investigation in `docs/TIMESHIFT.md`, plus a
  later, further-hardened version referenced in this diff): segment
  duration estimated as an average of the last 5 segments (not a
  single, noisy sample -- a real instance produced one segment just
  151ms long against a 2s target, which alone had previously collapsed
  the retry budget enough to make ffmpeg's demuxer read a real stall as
  genuine end-of-stream and kick Kodi back to the main menu), floored
  at a 1500ms minimum for a still-warming-up fresh buffer's first few
  segments, with a 3x margin (not the original 1.5x -- confirmed live
  that 1.5x ran too thin, with ordinary non-error catch-up cycles
  routinely using 60-95% of that budget under normal jitter). The
  in-progress-recording path apparently never got this same fix and
  may have the identical vulnerability (a single noisy segment
  collapsing its own, still-duplicated, still-1.5x budget) -- this
  refactor would have closed that gap by sharing one hardened
  implementation instead of two diverging copies. Discarded rather
  than committed since its actual authorship/testing history is
  unknown and it was found mid-unrelated-task; worth redoing properly
  (confirm the in-progress-recording path's vulnerability is real
  first, then share the implementation) rather than lost entirely.
- **Recurring, non-fatal `Packet corrupt` on server-side live timeshift,
  post-1.0 (surfaced during 1.0.7 verification, 2026-09-07).** Confirmed
  live on macOS (ESPN 1080p): ffmpeg's mpegts demuxer logs `Packet
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
  **Update (2026-09-08): a 30-minute continuous ESPN dwell (the
  follow-up angle above) also did not reproduce it.** Same benign
  `Packet corrupt` rate, zero size-disagreement firings, zero audio
  desync, zero stalls, zero `catch-up-to-tail` "gave up" exhaustions
  across the full 30 minutes. Two different stress angles tried now
  (rapid switching and long dwell), neither reproduced it -- still a
  single, unreplicated occurrence. See `docs/TIMESHIFT.md`'s same
  section for the full breakdown.
  **Update (2026-09-08): a second, real occurrence -- this time
  self-recovered.** macOS, addon 0.9.0, ~20-hour continuous session
  (MLB Network): a burst of three `Stream stalled` events in ~3.6s,
  audio desync peaking at -5064ms, but this time Kodi's own resync
  machinery pulled it back under threshold within about a second with
  no manual intervention and no user-visible interruption -- unlike the
  first occurrence, which required manually stopping the player. Same
  noise, same rate, zero size-disagreement firings across the full
  session. Two data points now with different outcomes (fatal vs.
  self-recovering), suggesting a second branch point beyond just
  cosmetic-vs-escalates. Still not enough signal to act on. See
  `docs/TIMESHIFT.md`'s same section for the full detail.
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
- ~~A genuine, unexplained anomaly from the macOS pass~~ -- **Decided:
  treating as a one-off, not pursuing further.** `kodi.log` showed a full
  `UpdateClients: Recreating PVR client` (clean DLL unload/reload, no
  crash, playback fine right after) immediately after dismissing a
  resume-prompt dialog, with no settings change involved -- ruling out
  the known beta.2 spurious-restart bug (settings-write-triggered). Did
  not reproduce on a second attempt. Left here for the record in case it
  ever resurfaces and this becomes useful context.
- ~~Catch-up and recurring-timer creation have no clean way to exercise
  via Kodi's JSON-RPC alone~~ -- **Decided: counted as tested anyway, not
  worth building a dedicated driving mechanism for.** The underlying
  logic is platform-independent and already has real live-verification
  from earlier sessions; the gap is in this testing technique
  (JSON-RPC-driven, not GUI-driven), not in the feature.
- [x] **Version bump for the real 1.0 tag.** `addon.xml.in` bumped from
      `1.0.0-beta.3` to `1.0.0`; confirmed live (Windows rebuild,
      `Addons.GetAddonDetails` reports `version: "1.0.0"`, `broken: false`).
      `packaging/coreelec/pvr.dispatcharrai/package.mk`'s `PKG_VERSION` is
      also `1.0.0` now, with `PKG_SHA256` reset to the same all-zeros
      placeholder used before every prior tag existed -- per the existing
      pattern in git history, the real checksum can only be filled in
      *after* the tag is pushed and the release tarball exists, as its own
      follow-up commit.
- [x] **Both companion plugins' `plugin.json` bumped from `0.1.0` to
      `1.0.0`**, matching the addon's own milestone -- both had
      meaningful fixes/features since `0.1.0` (`recording_edl`'s
      orphan-cleanup and `.dvr_*_hls` diagnostics; `timeshift_buffer`'s
      concurrent-viewer and provider-concurrent-stream-limit fixes,
      among others per `CHANGELOG.md`), and versioning them together with
      the addon signals "these are the 1.0-era companion plugins" more
      clearly than an independent per-plugin scheme would. This version
      is what Dispatcharr's own Plugins page shows users, independent of
      the addon's own version number.
- [x] **`CHANGELOG.md` created** at the repo root, "Keep a Changelog"-ish
      format, newest first, user-facing (what changed, not why -- that
      still lives in `docs/`). Backfilled real entries for every tagged
      release from `0.2.0` through `1.0.0-beta.3` by walking actual git
      log ranges between tags, not guessed from memory. The section
      covering everything since `1.0.0-beta.3` (the `Off` default flip,
      `Local` mode's return, the admin-scoping clarifications, the
      in-progress-recording catch-up fix, etc.) is now `## [1.0.0] -
      2026-09-05`, renamed from `[Unreleased]` ahead of the real tag.
      Linked from the main README's "More detail" section.
- [x] **Final overall review pass** (gaps/optimization/redundancy/
      conciseness) before the real tag, via three parallel audit agents
      covering C++ source, docs/CHANGELOG consistency, and release
      readiness/packaging. Real findings, all fixed: both plugins'
      actually-authoritative `plugin.py` `Plugin.version` attributes were
      still `0.1.0` even after the `plugin.json` bump (`plugin.json` is
      only used for Dispatcharr's not-yet-trusted import preview --
      `plugin.py`'s own class attribute is what runs); a `LICENSE` file
      was missing at the repo root despite every doc claiming
      `GPL-2.0-or-later`; `CHANGELOG.md`'s `[Unreleased]` section was
      missing several real, git-log-verified entries (`timeshift_buffer`
      tuning, `recording_edl`'s diagnostic-toast fix, the removed
      `snapshot_buffer` action, the shared-buffer README correction);
      this file had real internal SSH IPs/usernames from dev testing
      (scrubbed to "on the local network") and stale line-count claims
      (softened -- later fixes had already grown those numbers back);
      `docs/API_NOTES.md` still claimed the addon "only implements the
      username/password JWT flow" when `X-API-Key` fallback has been
      fully implemented for a while; `docs/RECORDING_EDL.md` had a
      mismatched Markdown link label; `SeekInProgressRecordingStream()`
      was missing the negative-position debug log its sibling
      `SeekLiveTimeshiftStream()` already had.
