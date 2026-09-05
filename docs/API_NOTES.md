# Dispatcharr API notes

*This file and the ones it indexes below are engineering/development
notes -- root causes, real API behavior confirmed live, decisions tried
and reverted -- kept as project history, not user documentation. If
you're looking to install or configure the addon, see the main
[README.md](../README.md) instead.*

`src/DispatcharrClient.cpp` talks to Dispatcharr's own REST API. Dispatcharr
ships a live OpenAPI document at `http://<your-server>:9191/api/schema/`
(and a Swagger UI at `/swagger/`) -- **check that first** against anything
below before shipping a build, since Dispatcharr is a young, fast-moving
project and its schema has changed across recent releases (v0.23, v0.25 both
touched API schemas).

## Confirmed against a live instance

Verified 2026-08-30 against a real Dispatcharr instance's own
`/api/schema/` and actual API responses (not just docs/issues):

| Purpose | Method | Path |
|---|---|---|
| Login | POST | `/api/accounts/token/` (returns `{access, refresh}`) |
| Refresh | POST | `/api/accounts/token/refresh/` |
| API key auth | header | `X-API-Key: <key>` -- accepted as an alternative to the JWT bearer token on nearly every endpoint. Generate via `POST /api/accounts/api-keys/generate/`. **Accounts with restricted ("streamer") permissions may not be able to log in via `/api/accounts/token/` at all** (confirmed: a real streamer-role account got "No active account found" from the login endpoint) -- if login fails for a permissions reason rather than a wrong-password reason, an API key is the working alternative. This addon currently only implements the username/password JWT flow; switching to `X-API-Key` would need a settings.xml/CMakeLists-level change, not just a field-name fix. |
| List channels | GET | `/api/channels/channels/` |
| Channel groups | GET | `/api/channels/groups/` (**not** `/api/channels/channel-groups/`, which doesn't exist -- Dispatcharr's SPA serves its own `index.html` for unmatched routes, so that guess returned a misleading HTTP 200 of HTML, not JSON) |
| List streams | GET | `/api/channels/streams/` |
| Channel logo | GET | `/api/channels/logos/{id}/cache/` -- `{id}` is the channel's `logo_id`, **not the channel's own id** |
| XMLTV guide | GET | `/output/epg` -- **its `<channel id="...">` is the channel's `channel_number`, not `tvg_id`** (confirmed: USA Network, `channel_number` 2632 and `tvg_id` "USANetwork.us", appears in the XMLTV as `<channel id="2632">`; checked against two other channels too). This addon originally matched EPG programmes to channels by `tvg_id`, which never matched anything -- see `XmlTvParser.h`. |
| Live stream | GET | `/proxy/ts/stream/{channel_uuid}` -- confirmed via a real instance that a channel's `uuid` field is accepted here (a wrong identifier would 404 immediately; instead it attempted to reach the upstream source and only failed there) |
| Stream session management | GET/POST | `/proxy/ts/status`, `/proxy/ts/status/{channel_id}`, `POST /proxy/ts/stop/{channel_id}`, `POST /proxy/ts/change_stream/{channel_id}`, `POST /proxy/ts/next_stream/{channel_id}` -- exist and look relevant to channel-switching reliability, but this addon doesn't currently call any of them (see the "channel switching" note below) |
| Recordings list/create | GET/POST | `/api/channels/recordings/` -- **bare array** on GET, not `{results:[...]}`. `Recording` has only `id`, `start_time`, `end_time`, `task_id`, `custom_properties`, `channel` -- **no title/subtitle/description/duration/in-progress fields at all.** Those are derived: duration from `end_time - start_time`, in-progress from `start_time <= now < end_time` (overridden by `custom_properties.status == "recording"` when present -- see `docs/RECORDINGS.md`), and title/subtitle/description confirmed (against real recordings, not just guessed) as `custom_properties.program.{title,sub_title,description}` when Dispatcharr auto-enriches from the airing EPG programme, falling back to a flat `custom_properties.{title,sub_title,description}` for anything that set them directly -- see `docs/RECORDINGS.md`'s first entry for how that was confirmed and why sending your own `custom_properties` on create replaces rather than merges with the auto-enrichment. |
| Recording delete | DELETE | `/api/channels/recordings/{id}/` |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` for a completed recording -- **not anonymous** despite the schema listing anonymous access as an allowed security scheme; a Bearer token or `X-API-Key` header is actually required (confirmed: flat 403 without one), and this endpoint properly honors `Range`. An in-progress (still-recording) one instead redirects to `.../hls/index.m3u8` -- same auth requirement, but each individual `seg_NNNNN.ts` segment **ignores `Range` entirely** and always serves its full body with a 200 regardless of what was requested (confirmed live; see `docs/RECORDINGS.md`'s in-progress-recording section for the two bugs this caused and their fixes) -- use `HEAD`/`Content-Length` to size a segment, and cache/slice client-side rather than relying on a ranged GET against it for partial reads. |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` -- body `{tvg_id}`, both optional |
| Series rules list | GET | `/api/channels/series-rules/` -- returns **`{"rules": [...]}`**, not a bare array or `{results:[...]}`. Per-item fields confirmed against a real populated rule: `{mode, title, tvg_id, channel_id, title_mode, description, description_mode}` -- **no numeric id field at all** (see `docs/RECORDINGS.md` for why `GetTimers()` hashes `(title, tvgId)` instead of using one). |
| Series rule create | POST | `/api/channels/series-rules/` -- body is `{title, tvg_id?, channel_id?, mode?, title_mode?, description?, description_mode?, untagged_is_new?, epg_source_id?}`. **Not** `{channel, title_pattern}** -- confirmed against the live `SeriesRuleRequest` schema. `channel_id`/`tvg_id` are both optional (channel_id "defaults to lowest-numbered channel for the EPG" if omitted). |
| Series rule delete | DELETE | `/api/channels/series-rules/?title=...&tvg_id=...&epg_source_id=...` (query params) -- confirmed against the live schema. **There is no `/api/channels/series-rules/{id}/` route**; series rules have no path-addressable id at all. |
| Catch-up session | POST | `/api/catchup/sessions/` -- body `{channel_uuid, start (ISO-8601), duration (minutes, optional)}`; response's `playback_url` is a **relative path**, prepend `BaseUrl()`. Confirmed end-to-end against a real instance: creates a session-bound URL that streams real MPEG-TS data immediately with no further auth. Per Dispatcharr's own docs, the session stays valid via a 10-minute *sliding* idle window (refreshed by each range/seek request), so unlike embedding a JWT directly in the URL (`GET /proxy/catchup/{uuid}?start=...&token=...`, also confirmed working but not used here), it won't expire mid-playback of a long programme. |

## Feature notes

This file used to hold everything in one place; it grew past 1,600
lines across a long multi-session history and got split by topic so
a given task doesn't need to load all of it. Each file below carries
the same "confirmed live, not just assumed" standard as this one.

- [EPG.md](EPG.md) -- channel/EPG API field shapes, and mapping rich
  XMLTV data (posters, new/premiere/live badges, cast, genre) into
  Kodi's EPG
- [CATCHUP.md](CATCHUP.md) -- catch-up ("play from guide") playback
  and its seeking-reliability trade-offs
- [TIMESHIFT.md](TIMESHIFT.md) -- live TV pause/rewind, both local
  (inputstream.ffmpegdirect) and server-side (the companion
  Dispatcharr plugin), including the confirmed-broken seek
  investigation for the server-side snapshot path
- [RECORDINGS.md](RECORDINGS.md) -- recordings and timers, confirmed
  end-to-end against real data (the largest file here)
- [RECORDING_EDL.md](RECORDING_EDL.md) -- commercial-break markers
  (comskip EDL) on the recording seekbar, and the companion plugin that
  makes them reachable at all
- [RECURRING_RULES.md](RECURRING_RULES.md) -- recurring (day-of-week)
  timer rules, backed by Dispatcharr's own RecurringRecordingRule model,
  and the timezone-offset setting bridging Kodi's UTC timers against
  Dispatcharr's own configured system timezone
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) -- the channel-switching
  IPv6 root cause, known Kodi-core quirks that aren't this addon's
  bug, multi-client limitations, and what's still unconfirmed

## How to verify quickly

From a machine that can reach your Dispatcharr instance:

```bash
# Get a token (only works for accounts with full login permissions)
curl -X POST http://<host>:9191/api/accounts/token/ \
  -H 'Content-Type: application/json' \
  -d '{"username":"<user>","password":"<pass>"}'

# Or use an API key instead (works for restricted/streamer-role accounts too)
curl http://<host>:9191/api/channels/channels/ \
  -H "X-API-Key: <your-api-key>" | jq '.[0] // .results[0]'

# Inspect the recordings schema via the live OpenAPI doc
curl http://<host>:9191/api/schema/ | grep -A30 '/api/channels/recordings/:'
```

Once you've confirmed a field/path, update the corresponding line in
`DispatcharrClient.cpp` (they're grouped near the top and clearly commented)
and update TROUBLESHOOTING.md's "Still unconfirmed" list.
