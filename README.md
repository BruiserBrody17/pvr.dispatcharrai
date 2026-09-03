# pvr.dispatcharrai

A Kodi PVR (Live TV / DVR) addon for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
built against Dispatcharr's **native REST API** (not the Xtream Codes
compatibility layer), so that channel management, EPG, and DVR recording
in Kodi map directly onto Dispatcharr's own backend and settings --
similar in spirit to how the TVHeadend PVR addon (`pvr.hts`) works against
a TVHeadend server.

Targets: Windows and macOS, both confirmed working through real use.
CoreELEC (aarch64, an ODROID N2+ is the reference device) is also a target
but not yet confirmed against a real device -- see `docs/BUILDING.md` for
platform-specific build steps, including why CoreELEC in particular needs
more than a plain cross-compile and exactly which parts of that build path
are still unverified rather than glossed over. CI also builds a generic
Linux (x86_64) zip on every push; see `.github/workflows/build.yml`.

## Status

This is a first working scaffold, not a finished addon. Implemented:

- JWT login (with reactive refresh-on-401) against Dispatcharr's own
  `/api/accounts/token/`
- Channel + channel-group listing
- EPG via Dispatcharr's XMLTV output (`/output/epg`), including per-programme
  posters, "New"/"Premiere"/"Live" badges, genre (with content-type colour
  coding, not just a flat label), cast, and episode names when the
  underlying EPG source (e.g. Schedules Direct) provides them -- confirmed
  live to match what TVHeadend's `pvr.hts` shows for the same kind of
  source; see `docs/API_NOTES.md` for exactly which XMLTV elements map to
  what, and what's deliberately not mapped
- Live channel playback (see the live TV pause/rewind bullet below for the
  current mechanism)
- Recording listing, playback, and deletion
- Commercial-break markers on a recording's seekbar, for a recording
  Dispatcharr's own comskip integration has processed in "mark" mode --
  requires the companion `dispatcharr-plugin/recording_edl/` plugin
  (installed separately, same as the live-timeshift one) since there's no
  other HTTP-reachable way to fetch a comskip `.edl` file's content; see
  `docs/RECORDING_EDL.md`. No setting to enable -- unconditional, and a
  recording with no markers (comskip never ran, or ran in the default
  "cut" mode that removes commercials directly instead of just marking
  them) is unaffected either way
- One-time and series timers, backed by Dispatcharr's recording and
  series-rule endpoints
- Catch-up/archive playback ("play from guide") for channels whose upstream
  provider supports it, via Dispatcharr's catch-up session API -- see
  `docs/API_NOTES.md` for exactly what this does and doesn't cover (it's
  per-programme catch-up, not a continuous live-timeshift buffer). Seeking
  within a catch-up programme uses Kodi's own built-in (imprecise)
  byte-estimation seeking by default; an optional setting
  (`enable_catchup_ffmpegdirect_seek`, off by default) routes it through
  `inputstream.ffmpegdirect` instead for noticeably more precise seeking,
  at the cost of occasionally-slow (usually ~10-20s, rarely longer) seeks
  -- requires `inputstream.ffmpegdirect` to be installed
- Live TV pause/rewind ("timeshift") via a server-side rolling buffer held
  by Dispatcharr itself, via a companion Dispatcharr plugin shipped
  alongside this addon (`dispatcharr-plugin/timeshift_buffer/` in this
  repo, installed separately on your Dispatcharr instance, not through
  Kodi; requires the Dispatcharr account configured above to be an admin
  account). Real pause/rewind/fast-forward from a plain Play, no extra
  step -- exposes the Dispatcharr-held buffer through this addon's own
  `OpenLiveStream`/`ReadLiveStream`/`SeekLiveStream` implementation so
  Kodi's native demuxer handles seeking directly, rather than routing
  through the separate `inputstream.ffmpegdirect` addon (an earlier
  approach that did that, and was confirmed live to have unfixable
  seeking -- see `docs/TIMESHIFT.md` for that investigation and the
  architecture that replaced it). Confirmed live end-to-end, including a
  95-second rewind spanning several buffer refreshes. This is the
  default (`live_timeshift_mode`, default `2`/server-side) -- an `Off`
  setting (`0`) is available for anyone who doesn't want (or can't get)
  an admin-level Dispatcharr account: a plain live stream straight from
  Dispatcharr's own proxy, no companion plugin, no elevated account, no
  pause/rewind. (This addon briefly made server-side unconditional with
  no setting at all; that turned out to be a real problem for a
  non-admin account, since it meant live playback failed outright, not
  just pause/rewind -- Off was reintroduced as a result. A third,
  local-device mode this addon also used to offer, via
  `inputstream.ffmpegdirect`'s own on-device buffer, stays retired.) See
  `docs/TIMESHIFT.md` and that plugin's own `README.md`
- In-progress recording playback -- lets you start watching a recording
  before Dispatcharr finishes writing it, from the true beginning, with
  real pause/rewind/fast-forward *and* live-follow together in the same
  session, no mutually-exclusive trade-off, and no setting required (this
  addon used to gate it behind an opt-in `enable_inprogress_playback`
  setting; removed once the feature proved stable, so it now behaves the
  same as playing a completed recording -- just press Play). Uses the
  same growing-buffer, native-demuxer architecture as server-side live
  timeshift above -- Dispatcharr's in-progress recording HLS output is
  exposed through this addon's own
  `OpenRecordedStream`/`ReadRecordedStream`/`SeekRecordedStream`
  implementation so Kodi's native demuxer handles seeking directly, rather
  than routing through `inputstream.ffmpegdirect` (an earlier approach
  that did that, and was confirmed live to have seek and live-follow
  permanently, architecturally mutually exclusive within one session --
  see `docs/RECORDINGS.md` for that investigation and the architecture
  that replaced it). Confirmed live end-to-end: real seeks landing near
  the requested position, pause/resume, and the recording's reported
  duration growing live while playback continues uninterrupted. A
  completed recording is unaffected and plays exactly as before.
- Optional real-time recording/timer updates (`enable_realtime_updates`
  setting, off by default, experimental) -- connects directly to
  Dispatcharr's own WebSocket push feed (the same one its web UI uses, no
  plugin or server-side change needed) so a change shows up in Kodi within
  about a second instead of waiting out the polling interval below; see
  `docs/API_NOTES.md`
- Periodic recordings/timers refresh (`recording_refresh_minutes` setting,
  default 5) so a change made outside this specific Kodi's own actions --
  another Kodi install sharing the account, a change made directly against
  Dispatcharr, a recording finishing on its own -- still surfaces without a
  restart, even without the real-time feature above enabled
- Background channel/EPG loading -- a dedicated background thread keeps
  the channel list and full EPG guide warm on its own schedule, so Kodi's
  own calls are normally served from cache instead of blocking on a fetch;
  confirmed live against a real ~9,000-channel instance (a genuinely slow
  fetch+parse otherwise) -- see `docs/EPG.md`
- Recurring (day-of-week) timer rules, backed by Dispatcharr's own
  `RecurringRecordingRule` model/scheduler -- shown in Kodi as a normal
  repeating timer (parent rule + individual materialized occurrences as
  child timers). One setting to be aware of:
  `recurring_rule_utc_offset_minutes` (default 0) -- Dispatcharr
  interprets a rule's time-of-day using its own configured system
  timezone, not UTC, with no server-side conversion available, so this
  bridges the gap; leave at 0 if Dispatcharr's system timezone is UTC,
  otherwise see `docs/RECURRING_RULES.md` for exactly what to set (and
  why it needs manual adjustment across DST if your zone observes it)

Not yet implemented / worth hardening next:

- The remaining genuinely-open items are narrow and already tracked
  in-place rather than listed here. `docs/CATCHUP.md` flags a provider's
  real catch-up archive depth, which has no API to query directly --
  inherent, not something further testing resolves. `docs/RECORDINGS.md`
  flags a one-off macOS WiFi timer-latency spike (8.4s, 1 of 3 runs):
  since investigated properly on a real Mac (7 idle-gap trials, zero
  reproduction) -- not a clean confirm or refute, since that machine's
  own continuous background network traffic turned out to confound the
  test, but real evidence surfaced a more plausible unrelated alternate
  cause than the original theory, and the speculative keep-alive-traffic
  "fix" is explicitly not recommended as a result. The three items
  previously tracked here -- a no-EPG-match recording's title, a theory
  about Dispatcharr setting a placeholder `end_time`, and this WiFi
  theory -- have all now been run down with real evidence one way or
  another; see `docs/RECORDINGS.md`.

## Configuration

Set these in Kodi's addon settings once installed: Dispatcharr host, port,
HTTPS toggle, username, and password. The account needs permission to read
channels/EPG and manage recordings on your Dispatcharr instance. With the
default `live_timeshift_mode` (server-side timeshift), the account must
also be a Dispatcharr **admin** account, with the companion
`dispatcharr-plugin/timeshift_buffer/` plugin (see above) installed and
enabled on your Dispatcharr instance -- set `live_timeshift_mode` to
`Off` instead if you'd rather not grant an admin account or install that
plugin; live channels still play, just without pause/rewind.

## Building

See `docs/BUILDING.md`. Short version: this can't be compiled in isolation
-- it plugs into Kodi's own binary-addon build harness, which needs a
matching Kodi source checkout. `.github/workflows/build.yml` automates
Windows/macOS/Linux builds on push.

## License

GPL-2.0-or-later, matching Kodi's own binary addon convention.
