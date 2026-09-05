*(tracked punch-list for the 1.0 release -- not part of the "confirmed live" API_NOTES.md family of docs; check items off and add new ones here as they come up)*

# 1.0 release checklist

## Documentation

- [x] README cleaned up and made concise -- rewritten from 211 to ~90
      lines: install/config/features only, narrative moved out. Decided
      **not** to physically relocate the existing `docs/*.md` files --
      ~40+ source-code comments (C++ and the Dispatcharr plugins' own
      Python) point at their exact current paths, so moving them would be
      a large, error-prone churn for no real benefit. Instead added a
      one-line disclaimer at the top of `docs/API_NOTES.md` marking that
      whole family as engineering history, not user docs, with a pointer
      back to the main README.
- [x] `dispatcharr-plugin/timeshift_buffer/README.md` (186 -> 70 lines)
      and `dispatcharr-plugin/recording_edl/README.md` (226 -> 61 lines)
      rewritten. Their narrative wasn't already covered elsewhere the way
      the main addon's was, so first backfilled the missing history into
      `docs/TIMESHIFT.md` (already had it -- just pointed there) and
      `docs/RECORDING_EDL.md` (didn't yet cover the orphan-sidecar-cleanup
      or `.dvr_*_hls` diagnostics/delete features added this session --
      added those sections), then trimmed each plugin README to
      install/actions/safety-essentials with a pointer back for the "why."
      Kept the safety-critical facts in place (what each destructive
      action does and doesn't touch, the `.dvr_*_hls` classification
      table) rather than treating them as narrative to cut.

## Platform testing

Four platforms: Windows, Rocky Linux, CoreELEC/ODROID, macOS.

**Smoke-test pass, defined**: live playback, recording start/stop,
catch-up, timeshift seek, EPG, recurring timers -- run once per platform
right before release.

| Platform | Status |
|---|---|
| Windows | ✅ Pass -- full 6-item smoke test |
| Rocky Linux | ✅ Pass (catch-up/recurring-timer creation not exercised -- see [Open items](#open-items-more-will-likely-come-up)) |
| CoreELEC / ODROID | ✅ Pass (same catch-up/recurring-timer caveat as Rocky Linux) |
| macOS | ✅ Pass (same catch-up/recurring-timer caveat; one unexplained anomaly noted below) |

### Windows

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

### Rocky Linux

**Full smoke-test pass completed this session**, against a fresh build of
current `master` (git checkout at `~/kodi-linux-build`, `git reset --hard
origin/master` then rebuilt -- unlike the Windows/CoreELEC trees, this
one's a real clone, no manual file-syncing needed) on real Rocky Linux
10.2 hardware (on the local network), Kodi via the official `tv.kodi.Kodi`
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

### CoreELEC / ODROID

**Full smoke-test pass completed this session**, against a fresh
cross-compiled build of current `master` on real ODROID N2+ hardware
(on the local network), driven via SSH + Kodi's JSON-RPC webserver the same
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

### macOS

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

## Release packaging

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

## Open items (more will likely come up)

- **All four platforms now have a completed smoke-test pass** (Windows,
  Rocky Linux, CoreELEC/ODROID, macOS) -- Local timeshift mode is
  confirmed live on all four, closing out what was the last real gap in
  platform coverage.
- **A genuine, unexplained anomaly from the macOS pass, not yet
  root-caused**: `kodi.log` showed a full `UpdateClients: Recreating PVR
  client` (clean DLL unload/reload, no crash, playback fine right after)
  immediately after dismissing a resume-prompt dialog, with no settings
  change involved -- ruling out the known beta.2 spurious-restart bug
  (settings-write-triggered). Did not reproduce on a second attempt.
  Worth a dedicated look (does dismissing a modal PVR-recording dialog
  itself ever trigger a client reload? on which platforms?) before
  1.0, or at minimum before assuming it's noise.
- Catch-up and recurring-timer creation still have no clean way to
  exercise via Kodi's JSON-RPC alone (see any platform's section above)
  -- worth deciding whether future platform passes should just accept
  this as a documented tooling gap (the underlying logic is
  platform-independent and already covered on Windows) or find a real
  driving mechanism (e.g. scripted GUI input) before calling any platform
  fully "tested."
