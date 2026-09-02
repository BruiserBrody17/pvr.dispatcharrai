# pvr.dispatcharrai

A Kodi PVR (Live TV / DVR) addon for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
built against Dispatcharr's **native REST API** (not the Xtream Codes
compatibility layer), so that channel management, EPG, and DVR recording
in Kodi map directly onto Dispatcharr's own backend and settings --
similar in spirit to how the TVHeadend PVR addon (`pvr.hts`) works against
a TVHeadend server.

Targets: Windows, macOS, and CoreELEC (aarch64, tested against an ODROID
N2+). See `docs/BUILDING.md` for platform-specific build steps -- CoreELEC
in particular needs a bit more than a plain cross-compile; that's explained
there rather than glossed over.

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
- Live channel playback via Dispatcharr's stream proxy
  (`/proxy/ts/stream/{uuid}`)
- Recording listing, playback, and deletion
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
- Optional live TV pause/rewind ("timeshift"), via the `live_timeshift_mode`
  setting (off by default): either buffered locally to Kodi-device disk via
  the separate `inputstream.ffmpegdirect` addon, or a server-side rolling
  buffer held by Dispatcharr itself, via a companion Dispatcharr plugin
  shipped alongside this addon (`dispatcharr-plugin/timeshift_buffer/` in
  this repo, installed separately on your Dispatcharr instance, not through
  Kodi; requires the Dispatcharr account configured above to be an admin
  account). Both modes give real pause/rewind/fast-forward from a plain
  Play, no extra step -- server-side mode does this by exposing the
  Dispatcharr-held buffer through this addon's own
  `OpenLiveStream`/`ReadLiveStream`/`SeekLiveStream` implementation so
  Kodi's native demuxer handles seeking directly, rather than routing
  through `inputstream.ffmpegdirect` (an earlier approach that did that,
  and was confirmed live to have unfixable seeking -- see
  `docs/TIMESHIFT.md` for that investigation and the architecture that
  replaced it). Confirmed live end-to-end, including a 95-second rewind
  spanning several buffer refreshes. See `docs/TIMESHIFT.md` and that
  plugin's own `README.md`
- Optional in-progress recording playback (`enable_inprogress_playback`
  setting, off by default, experimental), also via
  `inputstream.ffmpegdirect` -- lets you start watching a recording before
  Dispatcharr finishes writing it, from the true beginning. Runs a tiny
  loopback-only local HTTP server inside the addon to make this work (see
  `docs/API_NOTES.md`). Seek/FF/RW and continuing to follow the recording
  live are mutually exclusive within one playback session (Kodi decides
  seekability once, at open, based on a duration that can only be known
  once the recording is treated as complete) -- so plain Play on an
  in-progress recording defaults to "Play from start" (full seek/rewind
  over what's been recorded so far, won't pick up anything recorded after
  you pressed Play); a "Play live" entry in the recording's context menu
  (follows new content as it's recorded, no seek) arms that mode for the
  next time you press Play on it instead.
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

Not yet implemented / worth hardening next:

- Recurring (day-of-week) timer rules beyond simple series rules
- Proper ISO-8601 date parsing for recording start times (currently a
  known gap -- see `docs/API_NOTES.md`)
- Background/async data loading (current version loads channels/EPG
  synchronously on first access, which is fine for a home server but
  could be made non-blocking)
- A handful of Dispatcharr API paths and field names are best-effort
  guesses pending verification against your instance's live Swagger --
  all flagged in `docs/API_NOTES.md` and with inline `TODO(verify)`
  comments in the source.

## Configuration

Set these in Kodi's addon settings once installed: Dispatcharr host, port,
HTTPS toggle, username, and password. The account needs permission to read
channels/EPG and manage recordings on your Dispatcharr instance.

## Building

See `docs/BUILDING.md`. Short version: this can't be compiled in isolation
-- it plugs into Kodi's own binary-addon build harness, which needs a
matching Kodi source checkout. `.github/workflows/build.yml` automates
Windows/macOS/Linux builds on push.

## License

GPL-2.0-or-later, matching Kodi's own binary addon convention.
