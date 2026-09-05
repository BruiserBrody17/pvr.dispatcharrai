# pvr.dispatcharrai

A Kodi PVR (Live TV / DVR) addon for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
built against Dispatcharr's native REST API rather than its Xtream Codes
compatibility layer, so channels, EPG, live TV, and recordings in Kodi map
directly onto Dispatcharr's own backend and settings.

**Platforms**: Windows, macOS, Linux, and CoreELEC (tested on an ODROID N2+).

## Features

- Channel and channel-group listing, with EPG (posters, New/Premiere/Live
  badges, genre, cast, episode names where the EPG source provides them)
- Live TV playback, with optional server-side pause/rewind/seek via a
  companion Dispatcharr plugin (see below)
- Recording playback, including watching a recording while it's still
  being written
- Recording pre/post padding, synced both ways with Dispatcharr's own
  global setting
- Commercial-break markers on a recording's seekbar, for recordings
  Dispatcharr's comskip integration has marked (needs the
  `recording_edl` companion plugin)
- One-time, series, and recurring (day-of-week) timers, all editable in
  place from Kodi's own timer list
- Catch-up/archive playback ("play from guide") for channels whose
  provider supports it
- Optional real-time push updates for recordings/timers, so a change made
  elsewhere (another Kodi install, Dispatcharr's own web UI) shows up
  immediately instead of waiting for the next periodic refresh

## Companion Dispatcharr plugins

Two optional server-side plugins live in `dispatcharr-plugin/` in this
repo and install separately on your Dispatcharr instance (not through
Kodi):

- **`timeshift_buffer`** -- enables live TV pause/rewind/seek. Requires an
  admin-level Dispatcharr account. Without it, live channels still play
  fine (the default), just without pause/rewind -- set `live_timeshift_mode`
  to `Server-side` once you've installed it.
- **`recording_edl`** -- exposes comskip commercial-break markers to Kodi.
  Also requires an admin-level Dispatcharr account. Optional; without it
  (or with a non-admin account), recordings just show no markers --
  playback itself is unaffected.

Each has its own README with install steps.

## Installing

1. Download the zip for your platform from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharrai/releases).
2. In Kodi: **Add-ons -> Install from zip file**, and select the
   downloaded zip.
3. Enable the addon under **Settings -> Player -> Live TV** (or
   **PVR & Live TV -> Client specific settings** once enabled) and
   configure it (see below).

## Configuration

Set in Kodi's addon settings: Dispatcharr host, port, HTTPS toggle,
username, and password. A standard (non-admin) account works for
everything native to Dispatcharr itself: channel/EPG browsing and
recording playback need no special permission, and recording management
(adding/editing/deleting timers) just needs that account's `dvr_access`
set to `manage` -- still short of full admin.

Both companion plugins are the exception: Dispatcharr's plugin API is
admin-only regardless of what a plugin actually does, so **either one
requires a real Dispatcharr admin account** for the configured account to
use it at all -- live-TV pause/rewind (`timeshift_buffer`) and comskip
markers (`recording_edl`) alike. By default (`live_timeshift_mode` set to
`Off`), live channels play with no extra setup; set it to `Server-side`
once you've installed `timeshift_buffer` and have an admin account,
otherwise every live channel will fail to play in that mode.
`recording_edl` fails softer -- without admin, recordings just show no
markers rather than a playback failure.

Most settings take effect immediately after saving. Connection settings
(host/port/HTTPS/username/password) need a Kodi restart -- Kodi will tell
you when one does.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). This addon builds through
Kodi's own binary-addon build harness, which needs a matching Kodi source
checkout -- it can't be compiled standalone.
`.github/workflows/build.yml` builds Windows/macOS/Linux automatically on
every push and attaches release zips on a version tag.

## More detail

The `docs/` directory holds engineering notes -- how each feature was
built, root causes for bugs that were found and fixed, and decisions that
were tried and reverted -- kept as project history rather than user
documentation. Start at [docs/API_NOTES.md](docs/API_NOTES.md) if you
want it.

## License

GPL-2.0-or-later, matching Kodi's own binary addon convention.
