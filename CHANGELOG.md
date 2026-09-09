# Changelog

User-facing summary of what changed release to release. For the *why*
behind any of these -- root causes, investigations, things tried and
reverted -- see the `docs/` directory (start at
[docs/API_NOTES.md](docs/API_NOTES.md)); this file only covers what's
different, not the story behind it.

Versions before `0.2.0` aren't itemized here -- that was this project's
initial scaffold and buildout, before it had any tagged releases to
compare against.

## [0.9.2] - 2026-09-09

Addon only -- neither companion plugin changed for this pass.

### Added

- Recordings can now be renamed from Kodi's own recordings list.
- Recordings now show their real file size.
- An in-progress recording can now be extended (add more time to a
  still-recording timer) directly from Kodi's Timers screen.
- The PVR backend version shown to Kodi is now Dispatcharr's real,
  live version instead of a placeholder string.
- Broader timezone coverage for the recurring-timer timezone setting
  (about twice as many zones now auto-detect their UTC offset,
  including daylight saving).

## [0.9.1] - 2026-09-08

Addon only -- neither companion plugin changed for this pass.

### Fixed

- The on-screen seek bar for server-side live TV pause/rewind didn't
  move when you seeked -- it kept climbing with real time regardless of
  where you'd rewound or fast-forwarded to, even though the actual
  playback position was correct. Now reflects the real position.
- The same seek bar bug, for playback of a recording that's still in
  progress.
- A rare but severe playback corruption (garbled video, audio badly out
  of sync, sometimes needing a restart) that some channels could hit
  after fast-forwarding all the way to the live edge. Fast-forwarding to
  live now leaves a little more buffer margin, which resolved it in
  testing.

## [0.9.0] - 2026-09-07

Versioning scheme change only -- no code changed in this entry. All three
pieces stepped back from `1.0.x` to `0.x`: addon `1.0.8` -> `0.9.0`,
`timeshift_buffer` `1.0.6` -> `0.6.0`, `recording_edl` `1.0.2` -> `0.2.0`.
This project is still single-user and still turning up real bugs in
testing, and depends on Dispatcharr, which is itself still pre-1.0
(`0.30.0`) -- `1.0.x` signaled more stability than actually existed. Per
SemVer, `0.x` means "still changing," which is the honest state of things
right now. Future `1.0.0` releases (for any of the three pieces) will
happen once that's genuinely true, not on a fixed schedule.

## [1.0.8] - 2026-09-07

Addon only -- neither companion plugin changed for this pass.

### Fixed

Three findings from a project-wide code review of the addon's full C++
source (following the same review already done for both companion
plugins), none reproduced live -- all defensive gaps caught by
deliberately re-checking the whole codebase for failure shapes already
found once elsewhere this cycle:

- The real-time-updates WebSocket client (`WebSocketClient::SendAll()`)
  had no overall deadline on a stalled/zombie connection, unlike its
  sibling read-side function -- could have silently hung the background
  update thread indefinitely. See `docs/API_NOTES.md`'s
  "`WebSocketClient::SendAll()` could hang this thread indefinitely"
  section.
- EPG broadcast IDs were computed from a channel id and only the low 16
  bits of each entry's start time, risking collisions between different
  programmes on the same channel roughly every 18.2 hours apart -- a
  real, non-negligible risk given how many entries a multi-day guide
  holds per channel. See `docs/EPG.md`'s "Broadcast IDs could collide
  across a channel's own EPG entries" section.
- An in-progress recording's HLS playlist duration parsing could hit
  undefined behavior (not a clean, catchable error) on a malformed
  `inf`/`nan` value. See `docs/RECORDINGS.md`'s entry on the same.

## `timeshift_buffer` [1.0.6] - 2026-09-07

Plugin only -- the addon didn't change for this fix, per the decoupled
versioning policy.

### Fixed

- A malformed `#EXTINF:` duration value (`inf`, not expected from
  ffmpeg in practice) in a channel's live playlist could fail the
  entire `get_live_manifest` fetch instead of just defaulting that one
  segment's duration to zero. Found via a follow-up audit prompted by
  an analogous fix in the companion `recording_edl` plugin (see below)
  -- every numeric conversion in this plugin was re-checked against the
  same failure shape. See `docs/TIMESHIFT.md`'s "A malformed `#EXTINF:`
  duration could fail the whole manifest fetch" section.

## `recording_edl` [1.0.2] - 2026-09-07

Plugin only -- the addon didn't change for this fix, per the decoupled
versioning policy.

### Fixed

- A single malformed line in a recording's `.edl` file (a `nan` or
  `inf` timestamp -- not expected from comskip in practice, but not
  something a parser reading a file should assume) could take down the
  entire `get_edl` result for that recording instead of just being
  skipped, showing zero commercial markers rather than whatever other
  entries were valid. Found via a comparative architecture review of
  the plugin (the same pass that reviewed `timeshift_buffer` earlier).
  See `docs/RECORDING_EDL.md`'s "A malformed `.edl` line could take
  down the whole result, not just itself" section.

## [1.0.7] - 2026-09-07

`timeshift_buffer` also bumped, to its own independent `1.0.5` -- this
release **requires redeploying the updated plugin to Dispatcharr** for
the plugin-side half to take effect.

### Fixed

- Live TV server-side timeshift: a second, distinct freeze from the
  1.0.6 fix above, reported immediately after upgrading -- playback
  could still stall permanently, this time within seconds of opening,
  with `ffmpeg`'s own demuxer logging `Packet corrupt`. Root cause: if
  the plugin ever reports a segment's byte size before a write has
  fully settled, this addon locks that size in permanently and every
  *later* segment's computed position silently drifts out of alignment
  with its real file for the rest of the session -- producing
  corrupted-looking playback with no error anywhere, since each
  individual read still "succeeds." Fixed at both ends: the plugin no
  longer trusts a cached size for the newest segment on any given call,
  and the addon now cross-checks the real size the file server reports
  on every read against what it has cached, treating any disagreement
  as an immediate, clearly-logged failure instead of silent corruption.
  See `docs/TIMESHIFT.md`'s "1.0.6 follow-up: a second, distinct freeze"
  section for the full investigation, including what was ruled out.
- `timeshift_buffer`: found via the diagnostic above actually catching a
  real, live disagreement -- switching away from a channel and back
  could still hit the same "wrong cached segment size" failure, this
  time because the plugin's own manifest cache wasn't invalidated
  reliably across a buffer restart in a multi-worker deployment (a
  channel's new ffmpeg process reuses the same segment filenames and
  sequence numbers as its previous instance, and a worker holding a
  stale cache entry from the old instance had no way to know it was
  gone). An initial fix tying cache entries to the buffer's process ID
  was itself confirmed live to have a gap under heavy testing churn --
  OS pids get recycled, and a stale entry tagged with a since-reassigned
  pid passed the check it should have failed. Fixed properly by keying
  on the buffer's own access token instead (already a fresh, random
  value per genuine instance, with no reuse risk regardless of churn).
  See `docs/TIMESHIFT.md`'s "1.0.7 follow-up #2" section, including the
  "pid-based version wasn't good enough" update.

## [1.0.6] - 2026-09-07

Addon only -- `timeshift_buffer` didn't change for this fix.

### Fixed

- **1.0.5 regression:** Live TV server-side timeshift could get
  permanently stuck in "buffering" and never recover, reproduced on the
  very first playback attempt after upgrading to 1.0.5. Caused by the new
  per-viewer heartbeat (added in 1.0.5) riding along on the same thread
  Kodi's own demuxer depends on for continuous reads, with no bound on
  how long that call could take -- an ordinary transient network hiccup
  at the wrong moment (roughly every 4 minutes, whenever the access token
  needed a routine refresh) could stack into minutes of blocking on a
  single read call, long enough to permanently stall playback even after
  the slow call eventually completed. Fixed by bounding the heartbeat to
  a short, fixed 2-second timeout and never letting it trigger a token
  refresh or re-login itself. See `docs/TIMESHIFT.md`'s "1.0.5
  regression: the new per-viewer heartbeat could stall playback
  permanently" section for the full root cause.

## [1.0.5] - 2026-09-07

Addon only. `timeshift_buffer` also bumped, to its own independent
`1.0.2` (per the decoupled versioning policy) -- the addon-side and
plugin-side halves below are paired, so this release **requires
redeploying the updated `timeshift_buffer` plugin to Dispatcharr** for
any of it to take effect server-side. All three items found via a
comparative architecture review of the plugin's own implementation, not
user reports or live incidents.

### Fixed

- Live TV server-side timeshift: a viewer that crashed (force-quit,
  network drop) without cleanly stopping could leave its own reference-
  count entry stuck on the shared buffer forever, silently preventing the
  *next* viewer's clean stop from actually tearing the buffer down --
  defeating the fast-teardown guarantee the whole viewer reference-
  counting design depends on. Fixed by tracking each viewer's own
  last-seen time separately instead of one buffer-wide heartbeat; the
  addon now pings the plugin with a per-viewer heartbeat every 10s while
  a stream is open. See `docs/TIMESHIFT.md`'s "A crashed viewer's own
  reference-count entry never got cleaned up" section.
- Live TV server-side timeshift: the plugin's own segment/playlist file
  server (a separate port from Dispatcharr's own, exposed the same way
  per the plugin's own setup instructions) had no access control at all
  -- anyone who could reach that port could read any channel's currently-
  buffered live segments with zero Dispatcharr credentials. Fixed by
  requiring a per-buffer access token, issued only via the already
  admin-gated `start_buffer` action. See `docs/TIMESHIFT.md`'s "The
  plugin's own file server had no access control at all" section.

### Changed

- `timeshift_buffer`'s `get_live_manifest` action no longer rebuilds its
  entire response from scratch on every call -- at this plugin's own
  defaults that was up to 1,800 `stat()` syscalls and a full playlist
  re-parse for a call that found nothing new, and it's called far more
  often than the buffer could possibly have grown. Now caches per-worker-
  process and only re-stats genuinely new segments. See
  `docs/TIMESHIFT.md`'s "`get_live_manifest` rebuilt its whole response
  from scratch on every call" section.

## [1.0.4] - 2026-09-06

Addon bumped to 1.0.4. Both companion plugins also bumped, to their own
independent `1.0.1` (per the decoupled versioning policy) -- not tied to
this addon release number.

### Changed

- Trimmed the longest addon-settings help texts (`live_timeshift_mode`,
  the recording-padding/recurring-timezone settings, sports padding,
  catch-up seek, real-time updates -- several were 800-1,260 characters,
  slow-scrolling walls of text in Kodi's help popup). Cut down to the
  essential decision-relevant info, with a pointer to the relevant
  `docs/*.md` file for anyone who wants the full explanation. Also fixed
  a rendering artifact (extra-looking spacing) in `live_timeshift_mode`'s
  help text caused by a repeated `" -- "` separator.
- `Recurring timer: manual timezone offset from UTC` is now greyed out
  and uneditable whenever `Recurring timer: timezone` is set to anything
  other than "Manual", since it has no effect in that case.
- Trimmed the longest help text in both companion plugins' own settings
  (`timeshift_buffer`'s idle-timeout and test-channel-UUID fields,
  `recording_edl`'s test-recording-ID field) the same way, for the same
  reason -- same content, tighter wording. Both plugins bumped to
  `1.0.1` for this (their own independent version, per the decoupled
  versioning policy -- the addon didn't change).

## [1.0.3] - 2026-09-06

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
  guide data tags as sports can now get extra end-of-recording padding on
  top of the normal recording padding, since sports broadcasts commonly
  run long in a way scripted programming doesn't (new
  `sports_extra_padding_minutes` setting -- off by default, opt-in).
  Doesn't apply if the timer's end time has already been manually
  adjusted, or to recurring/series rules. See `docs/EPG.md`'s "Sports
  events get extra recording padding automatically" section.

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
