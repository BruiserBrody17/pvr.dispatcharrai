# Dispatcharr API notes

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
| XMLTV guide | GET | `/output/epg` |
| Live stream | GET | `/proxy/ts/stream/{channel_uuid}` -- confirmed via a real instance that a channel's `uuid` field is accepted here (a wrong identifier would 404 immediately; instead it attempted to reach the upstream source and only failed there) |
| Stream session management | GET/POST | `/proxy/ts/status`, `/proxy/ts/status/{channel_id}`, `POST /proxy/ts/stop/{channel_id}`, `POST /proxy/ts/change_stream/{channel_id}`, `POST /proxy/ts/next_stream/{channel_id}` -- exist and look relevant to channel-switching reliability, but this addon doesn't currently call any of them (see the "channel switching" note below) |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` (anonymous, Range-seekable) |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` |
| Series rules exist | -- | `/api/channels/series-rules/` (CRUD base confirmed to exist) |

### Confirmed channel JSON fields (`GET /api/channels/channels/`)

The response is a **bare JSON array**, not paginated/wrapped in
`{results: [...]}` (at least on the instance this was checked against) --
`DispatcharrClient::GetChannels()`'s `results`-or-bare-array handling covers
this correctly either way, so no change was needed there.

| Field | Notes |
|---|---|
| `id` | integer, channel's own id |
| `uuid` | string, used in the live-stream URL (confirmed, see above) |
| `name` | string |
| `channel_number` | number (observed as a float, e.g. `10687.0`) |
| `channel_group_id` | **bare integer**, not a nested `channel_group` object |
| `tvg_id` | bare string field directly on the channel, not nested under `epg_data` |
| `logo_id` | integer, FK to a separate Logo object -- **there is no `logo_url` field on Channel** (that field exists on the Stream model instead, which is a different object) |

`DispatcharrClient::GetChannels()` already handles the nested-object variants
defensively as a fallback, but the flat/bare forms above are what a real
instance actually returns.

## Streamer-role accounts and upstream provider limits

The M3U/Xtream account backing your channels (visible at
`GET /api/m3u/accounts/`) carries its own `max_streams` concurrent-connection
cap, enforced by the *upstream* IPTV provider, not just Dispatcharr. Rapid,
automated-looking channel switching (tested: ~18 distinct channel requests
within about a minute) triggered widespread timeouts/503s from the upstream
provider even though Dispatcharr's own `/proxy/ts/status` reported zero
active channels throughout -- i.e. the failures were the *provider*
throttling/flagging the account, not Dispatcharr holding a stale connection
open. If you see one channel play fine and the next one fail, try reproducing
it at a normal (non-rapid) channel-change pace before assuming it's an addon
or Dispatcharr bug.

**Update, confirmed against a real user's `kodi.log`:** a normal (non-rapid,
one-time) channel switch *did* fail -- Kodi's own `CCurlFile` timed out after
30s trying to open the new channel's `/proxy/ts/stream/{uuid}` URL. Critically,
that exact URL was tested independently moments later and streamed real,
sustained data immediately (27MB in 15s) -- so the channel itself wasn't dead;
the failure was specific to the moment of switching away from the previous
channel. This is consistent with Dispatcharr's proxy (or the upstream
provider) needing a brief window to release the old connection before the new
one succeeds.

As a first, low-risk mitigation (before the bigger architecture change below),
`GetChannelStreamProperties()` now waits `channel_switch_delay_seconds`
(default 2, a settings.xml `livetv` category option, 0 disables it) before
handing Kodi the new URL, giving that release window a chance to pass without
the addon needing to know anything about Dispatcharr's session state. If this
turns out not to be enough, or the delay needs to be very long to help, that's
itself evidence pointing at the heavier fix below rather than just a bigger
number.

If it turns out Dispatcharr itself *is* holding a channel's upstream slot
open after Kodi stops watching it (e.g. because Kodi's own disconnect isn't
detected promptly), the `/proxy/ts/stop/{channel_id}` endpoint above is the
one to call proactively -- but that requires the addon to switch from the
current stream-URL-passthrough model (`GetChannelStreamProperties()` only)
to also implementing `OpenLiveStream()`/`CloseLiveStream()` with
`PVRCapabilities::SetHandlesInputStream(true)`, which hands stream I/O
lifecycle to the addon instead of Kodi's own player. That's a real
architecture change, not a one-line fix -- try the delay above first.

## Still unconfirmed (verify before relying on in production)

- `POST /api/channels/recordings/` -- one-time recording creation. The
  request body in `DispatcharrClient::CreateOneTimeRecording()`
  (`channel`, `start_time`, `end_time`, `name`) is a best guess; check the
  live schema for the recordings endpoint and fix the field names in that
  one function if they differ.
- `POST /api/channels/series-rules/` -- series rule creation. Body fields
  (`channel`, `tvg_id`, `title_pattern`) are likewise a best guess.
- `Recording.startTime` is currently left at `0` -- the response almost
  certainly includes an ISO-8601 timestamp field (name unconfirmed); add a
  small date parser and wire it up in `DispatcharrClient::GetRecordings()`.

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
and update this file's "Still unconfirmed" list.
