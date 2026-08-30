# Dispatcharr API notes

`src/DispatcharrClient.cpp` talks to Dispatcharr's own REST API. Dispatcharr
ships a live Swagger/OpenAPI document at `http://<your-server>:9191/swagger/`
-- **check that first** against anything below before shipping a build,
since Dispatcharr is a young, fast-moving project and its schema has changed
across recent releases (v0.23, v0.25 both touched API schemas).

## Confirmed

These were verified against Dispatcharr's public GitHub issues, release
notes, and docs at the time this was written:

| Purpose | Method | Path |
|---|---|---|
| Login | POST | `/api/accounts/token/` (returns `{access, refresh}`) |
| Refresh | POST | `/api/accounts/token/refresh/` |
| List channels | GET | `/api/channels/channels/` |
| List streams | GET | `/api/channels/streams/` |
| Channel logo | GET | `/api/channels/logos/{id}/cache/` |
| XMLTV guide | GET | `/output/epg` |
| Live stream | GET | `/proxy/ts/stream/{channel_uuid}` |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` (anonymous, Range-seekable) |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` |
| Series rules exist | -- | `/api/channels/series-rules/` (CRUD base confirmed to exist) |

## Assumed (verify before relying on in production)

These follow standard Django REST Framework `ModelViewSet` conventions,
matching the confirmed routes above, but the **exact path segment and JSON
field names were not independently confirmed**:

- `GET /api/channels/channel-groups/` -- channel group list. Could instead be
  `/api/channels/groups/`.
- `POST /api/channels/recordings/` -- one-time recording creation. The
  request body in `DispatcharrClient::CreateOneTimeRecording()`
  (`channel`, `start_time`, `end_time`, `name`) is a best guess; check the
  live Swagger's schema for the recordings endpoint and fix the field names
  in that one function if they differ.
- `POST /api/channels/series-rules/` -- series rule creation. Body fields
  (`channel`, `tvg_id`, `title_pattern`) are likewise a best guess.
- Channel JSON fields: `uuid` (used for the live-stream URL), `epg_data.tvg_id`
  / `tvg_id` (used to match XMLTV `<channel id="...">`), `channel_group`
  (nested object or bare id) -- `DispatcharrClient::GetChannels()` tries a
  couple of reasonable variants defensively, but confirm against a real
  response from your instance.
- `Recording.startTime` is currently left at `0` -- the response almost
  certainly includes an ISO-8601 timestamp field (name unconfirmed); add a
  small date parser and wire it up in `DispatcharrClient::GetRecordings()`.

## How to verify quickly

From a machine that can reach your Dispatcharr instance:

```bash
# Get a token
curl -X POST http://<host>:9191/api/accounts/token/ \
  -H 'Content-Type: application/json' \
  -d '{"username":"<user>","password":"<pass>"}'

# Inspect a channel's real field names
curl http://<host>:9191/api/channels/channels/ \
  -H "Authorization: Bearer <access-token>" | jq '.results[0] // .[0]'

# Inspect the recordings schema via Swagger's raw JSON
curl http://<host>:9191/api/schema/ | jq '.paths."/api/channels/recordings/"'
```

Once you've confirmed a field/path, update the corresponding line in
`DispatcharrClient.cpp` (they're grouped near the top and clearly commented)
and delete the matching entry from this file's "Assumed" list.
