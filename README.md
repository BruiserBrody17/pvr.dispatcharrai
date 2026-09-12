# pvr.dispatcharr-unofficial

**This is an unofficial, community-maintained addon.** It is not
affiliated with, endorsed by, or supported by the
[Dispatcharr](https://github.com/Dispatcharr/Dispatcharr) project --
it's a third-party client built against Dispatcharr's public API. Please
don't file addon bugs on Dispatcharr's own issue tracker, and don't
expect Dispatcharr's maintainers to support this addon.

A Kodi PVR (Live TV / DVR) addon for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
built against Dispatcharr's native REST API rather than its Xtream Codes
compatibility layer, so channels, EPG, live TV, and recordings in Kodi map
directly onto Dispatcharr's own backend and settings.

**Platforms**: Windows, macOS, Linux, and CoreELEC (tested on an ODROID N2+).

**Status**: pre-1.0 and actively developed by a single maintainer --
expect occasional rough edges. Verified against Dispatcharr `0.30.0`;
Dispatcharr's own REST API has changed shape across releases before,
so a real incompatibility against a different version is possible and
not guaranteed to be caught yet. Bug reports (with the detail requested
in the issue template) are genuinely useful for exactly this reason.

## Features

- Channel and channel-group listing, with EPG (posters, New/Premiere/Live
  badges, genre, cast, episode names where the EPG source provides them)
- Live TV playback, with optional pause/rewind/seek -- either server-side
  via a companion Dispatcharr plugin, or entirely on-device with no
  Dispatcharr admin account needed (see below)
- Recording playback, including watching a recording while it's still
  being written
- Recording pre/post padding, synced with Dispatcharr's own global
  setting (needs an admin account to change -- see below)
- Commercial-break markers on a recording's seekbar, for recordings
  Dispatcharr's comskip integration has marked (needs the
  `recording_edl` companion plugin)
- One-time, series, and recurring (day-of-week) timers, all editable in
  place from Kodi's own timer list
- Catch-up/archive playback ("play from guide") for channels whose
  provider supports it
- Optional real-time push updates for recordings/timers, so changes made
  elsewhere (another Kodi install, Dispatcharr's web UI) show up
  immediately, not on the next periodic refresh

## Companion Dispatcharr plugins

Two optional server-side plugins live in `dispatcharr-plugin/` in this
repo and install separately on your Dispatcharr instance (not through
Kodi):

- **`timeshift_buffer`** -- enables server-side live TV pause/rewind/seek
  (`live_timeshift_mode` set to `Server-side`): one buffer per channel,
  shared across every device currently watching, torn down as soon as the
  last viewer stops. Requires an admin-level Dispatcharr account (see the
  plugin's own README for the full picture). Without one, use `Local`
  instead: pause/rewind buffered on the Kodi device itself via
  `inputstream.ffmpegdirect`, no admin account or server-side plugin
  needed, but local-only and gone on restart. `Off` (the default) is a
  plain live stream, no pause/rewind, no extra dependency.
- **`recording_edl`** -- exposes comskip commercial-break markers to Kodi.
  Also requires an admin-level Dispatcharr account. Optional; without it
  (or with a non-admin account), recordings just show no markers --
  playback itself is unaffected.

Each has its own README with install steps.

## Installing

1. Download the zip for your platform from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/releases).
2. In Kodi: **Add-ons -> Install from zip file**, and select the
   downloaded zip.
3. Enable and configure the addon under **Settings -> PVR & Live TV ->
   General -> PVR client add-ons** (or **Add-ons -> My add-ons -> PVR
   clients**, the same list either way): select **Dispatcharr PVR
   Client**, enable it, then **Configure** (see below).

## Configuration

Set in Kodi's addon settings: Dispatcharr host, port, HTTPS toggle,
username, and password. A standard account with `dvr_access` set to
`manage` covers everything native to Dispatcharr -- browsing, playback,
and timer management. An admin account is needed for both companion
plugins and for pushing a recording-padding change back to Dispatcharr
(reading the current value doesn't need one; the write just fails
silently otherwise -- see [docs/RECORDINGS.md](docs/RECORDINGS.md)).

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

See [CHANGELOG.md](CHANGELOG.md) for what changed release to release.
The `docs/` directory holds engineering notes -- how each feature was
built, root causes for bugs that were found and fixed, and decisions that
were tried and reverted -- kept as project history rather than user
documentation. Start at [docs/API_NOTES.md](docs/API_NOTES.md) if you
want it.

## License

GPL-2.0-or-later, matching Kodi's own binary addon convention.
