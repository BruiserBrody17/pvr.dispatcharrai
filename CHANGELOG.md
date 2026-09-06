# Changelog

User-facing summary of what changed release to release. For the *why*
behind any of these -- root causes, investigations, things tried and
reverted -- see the `docs/` directory (start at
[docs/API_NOTES.md](docs/API_NOTES.md)); this file only covers what's
different, not the story behind it.

Versions before `0.2.0` aren't itemized here -- that was this project's
initial scaffold and buildout, before it had any tagged releases to
compare against.

## [Unreleased]

Addon only.

### Fixed

- Live TV server-side timeshift: if the buffer died mid-playback (not
  just failing to start), playback froze indefinitely with no error --
  the addon kept silently retrying forever instead of recognizing the
  buffer was gone. Found via a comparative-architecture review against
  `pvr.hts`/Tvheadend. See `docs/TIMESHIFT.md` for the full root cause.
- If Kodi ever requested a second concurrent PVR instance, creating it
  could silently break settings-apply-live for the first one. Not known
  to have happened in practice -- hardened defensively. See
  `docs/API_NOTES.md`'s "Single-instance assumption" section.

### Added

- The optional real-time-updates WebSocket now reconnects immediately on
  an OS/device wake from sleep, instead of waiting out however much of
  its current (up to 60s) reconnect backoff was still left. See
  `docs/API_NOTES.md`'s "OS sleep/wake" section.
- A one-time recording created directly from an EPG entry Dispatcharr's
  guide data tags as sports now gets extra end-of-recording padding
  automatically (new `sports_extra_padding_minutes` setting, default 30)
  on top of the normal recording padding, since sports broadcasts commonly
  run long in a way scripted programming doesn't. Doesn't apply if the
  timer's end time has already been manually adjusted, or to recurring/
  series rules. See `docs/EPG.md`'s "Sports events get extra recording
  padding automatically" section.

## [1.0.2] - 2026-09-06

Addon only, same as 1.0.1.

### Fixed

- **1.0.1 regression, macOS only:** opening an in-progress recording could
  crash Kodi outright (a real concurrency bug in macOS's own system
  libcurl, triggered by 1.0.1's concurrent segment-probing). Fixed by no
  longer sharing the connection cache specifically for that concurrent
  probe burst -- see `docs/RECORDINGS.md` for the full root cause. The
  speed fix from 1.0.1 is unaffected: still ~5-6s to open a multi-hour
  in-progress recording cold, near-instant on a reopen.
- A self-heal API-key regeneration during an in-progress recording's open
  or a completed recording's read could immediately kill the playback
  that had just started, prompting a spurious "needs to restart" dialog.
  Not a regression from either fix above -- an older, separate bug this
  session's testing happened to surface. See `docs/RECORDINGS.md` for the
  full root cause.

## [1.0.1] - 2026-09-06

Addon only -- neither companion plugin changed, so neither's own version
moved (see `CLAUDE.md`'s versioning note: the addon and each plugin version
independently as of this release).

### Fixed

- Opening an in-progress recording got slower the longer it had already
  been recording, and re-paid that full cost on every open, not just the
  first -- a ~2h-in recording took 29.4s to open. Fixed by probing new
  segments' byte sizes concurrently instead of one at a time, and caching
  already-probed segments across opens for the same recording (see
  `docs/RECORDINGS.md` for the full root cause). Confirmed live: the same
  kind of open now takes ~4.7s cold, and well under a tenth of a second on
  a reopen.

## [1.0.0] - 2026-09-05

### Changed

- `live_timeshift_mode` now defaults to `Off` instead of `Server-side`.
  A fresh install with no admin account or `timeshift_buffer` plugin set
  up was hard-failing every live channel; `Off` plays live TV immediately
  with zero extra setup. Existing installs are unaffected -- this only
  changes what a brand-new profile starts with.
- Clarified in the docs (no behavior change): both companion plugins
  (`timeshift_buffer` and `recording_edl`) require a genuine Dispatcharr
  **admin** account -- a blanket restriction in Dispatcharr's own plugin
  API, not something specific to either plugin. Native channel/EPG/
  recording access does not need admin.
- Clarified in the docs (no behavior change): pushing a recording-padding
  change back to Dispatcharr also needs an admin account (reading the
  current value doesn't). A non-admin push currently fails silently --
  not yet fixed, just now documented.
- All three READMEs rewritten to be concise; engineering narrative moved
  into `docs/`.
- `timeshift_buffer` plugin tuning, found via real hardware/load testing:
  `segment_seconds` default lowered 6s -> 2s and `idle_timeout_seconds`
  default lowered 120s -> 30s (both for snappier catch-up and faster
  cleanup of abandoned buffers), and ffmpeg's SIGTERM-to-SIGKILL grace
  period on stop shortened 5s -> 2s. A buffer that can't start because a
  provider's own concurrent-stream limit is already exhausted now fails
  fast instead of hanging for a slow timeout.
- `recording_edl` plugin: diagnostic action results (e.g. the `.dvr_*_hls`
  cleanup actions) now surface through Dispatcharr's own result toast
  instead of a field nothing displayed; added a `test_recording_id`
  setting for easier manual testing from the Plugins page.

### Added

- **`Local` live-TV pause/rewind reintroduced** (`live_timeshift_mode`
  value `1`): real pause/rewind buffered entirely on the Kodi device via
  `inputstream.ffmpegdirect`, needing no Dispatcharr admin account and no
  server-side plugin. Fills the gap `Off`'s new default leaves for anyone
  who wants live pause/rewind without granting admin access.
- `recording_edl` plugin: orphaned `.edl`/`.logo.txt` sidecar file cleanup,
  and `.dvr_*_hls` staging-directory diagnostics plus cleanup of
  confirmed-orphaned ones.
- Recurring (day-of-week) timers now auto-compute their UTC offset for
  ~25 common timezones instead of requiring manual entry, staying correct
  across DST transitions.
- Both companion plugins (`timeshift_buffer`, `recording_edl`) are now
  packaged as downloadable zip assets on GitHub Releases, alongside the
  addon itself -- previously only the addon had a release zip.
- `inputstream.ffmpegdirect` declared as an optional addon dependency, so
  Kodi's own addon info reflects the relationship.

### Removed

- `timeshift_buffer`'s `snapshot_buffer` plugin action -- superseded by
  later fixes to seeking directly against the live buffer, so the
  workaround it existed for is no longer needed.

### Fixed

- Recording playback failing immediately after stopping a recording,
  while Dispatcharr was still finalizing the HLS-to-MKV concat in the
  background.
- Live-edge seek stall on in-progress (still-recording) playback.
- A crash-prone catch-up-retry calculation (could collapse to almost no
  retry budget on an unlucky short segment, ending playback outright) --
  previously fixed only for live-TV timeshift, now also applied to
  in-progress-recording playback, which had the same bug.
- An invalid XML comment that broke CoreELEC builds specifically (not
  caught by Windows/macOS/Linux builds, which don't validate addon.xml as
  strictly).
- README incorrectly claimed a second device joining an already-running
  server-side timeshift buffer could rewind into another device's earlier
  viewing history. Live-tested and found false: the underlying buffer
  *process* is genuinely shared per-channel, but each device's own
  rewind window is still capped to its own viewing session either way --
  corrected, with the accurate explanation moved into
  `timeshift_buffer`'s own README.

## [1.0.0-beta.3] - 2026-09-04

### Fixed

- Provider concurrent-stream-limit failures when switching or stopping a
  live channel.

## [1.0.0-beta.2] - 2026-09-04

### Fixed

- Concurrent live-timeshift viewers killing each other's buffers -- a
  second device opening the same channel could kill the first device's
  still-playing buffer outright.
- CI packaging so a tagged release actually gets its build zips attached
  (was silently broken).

## [1.0.0-beta.1] - 2026-09-04

Re-verified `0.4.0`'s fixes on real Linux (Rocky Linux 10) hardware; no
functional changes of its own.

## [0.4.0] - 2026-09-04

### Fixed

- Recurring-rule flooding, a settings-restart quirk, and live-timeshift
  stability issues -- all found via real CoreELEC/ODROID N2+ hardware
  testing.

## [0.3.0] - 2026-09-03

### Added

- Timer editing in place (`UpdateTimer()`) for all timer types, instead
  of delete-and-recreate.
- Recording folder organization and global recording padding as an addon
  setting.
- Recurring (day-of-week) timer rules.
- Commercial-break markers (comskip EDL) on the recording seekbar.

### Changed

- Settings apply live instead of requiring a Kodi restart.
- Channel/EPG loading moved to a background thread instead of blocking
  Kodi's calling thread.
- Reintroduced `Off` as a `live_timeshift_mode` option.

### Fixed

- Data races on the JWT token pair and API key across concurrent threads.

## [0.2.0] and earlier

Initial development and scaffold -- this project's first tagged release.
