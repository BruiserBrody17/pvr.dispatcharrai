# pvr.dispatcharr

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
- EPG via Dispatcharr's XMLTV output (`/output/epg`)
- Live channel playback via Dispatcharr's stream proxy
  (`/proxy/ts/stream/{uuid}`)
- Recording listing, playback, and deletion
- One-time and series timers, backed by Dispatcharr's recording and
  series-rule endpoints

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
