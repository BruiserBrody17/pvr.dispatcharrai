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
| XMLTV guide | GET | `/output/epg` -- **its `<channel id="...">` is the channel's `channel_number`, not `tvg_id`** (confirmed: USA Network, `channel_number` 2632 and `tvg_id` "USANetwork.us", appears in the XMLTV as `<channel id="2632">`; checked against two other channels too). This addon originally matched EPG programmes to channels by `tvg_id`, which never matched anything -- see `XmlTvParser.h`. |
| Live stream | GET | `/proxy/ts/stream/{channel_uuid}` -- confirmed via a real instance that a channel's `uuid` field is accepted here (a wrong identifier would 404 immediately; instead it attempted to reach the upstream source and only failed there) |
| Stream session management | GET/POST | `/proxy/ts/status`, `/proxy/ts/status/{channel_id}`, `POST /proxy/ts/stop/{channel_id}`, `POST /proxy/ts/change_stream/{channel_id}`, `POST /proxy/ts/next_stream/{channel_id}` -- exist and look relevant to channel-switching reliability, but this addon doesn't currently call any of them (see the "channel switching" note below) |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` (anonymous, Range-seekable) |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` |
| Series rules exist | -- | `/api/channels/series-rules/` (CRUD base confirmed to exist) |
| Catch-up session | POST | `/api/catchup/sessions/` -- body `{channel_uuid, start (ISO-8601), duration (minutes, optional)}`; response's `playback_url` is a **relative path**, prepend `BaseUrl()`. Confirmed end-to-end against a real instance: creates a session-bound URL that streams real MPEG-TS data immediately with no further auth. Per Dispatcharr's own docs, the session stays valid via a 10-minute *sliding* idle window (refreshed by each range/seek request), so unlike embedding a JWT directly in the URL (`GET /proxy/catchup/{uuid}?start=...&token=...`, also confirmed working but not used here), it won't expire mid-playback of a long programme. |

## Catch-up ("timeshift") playback

Dispatcharr has a real catch-up/archive feature, exposed via
`GetEPGTagStreamProperties()`/`IsEPGTagPlayable()` on the guide's past
programmes (Kodi's normal "play from EPG" mechanism, same category as
recordings -- **not** `OpenLiveStream()`/`CloseLiveStream()`, no
`SetHandlesInputStream` capability needed). Important to understand what this
actually is, since it's easy to conflate with TVHeadend-style timeshifting:

- **It's per-channel and per-programme, not a continuous rolling buffer.**
  A channel supports it only if `Channel::catchupEnabled` (`is_catchup` in
  the API) is true and `catchupDays` (`catchup_days`) is nonzero -- both
  driven by whether the *upstream IPTV provider* offers catch-up/archive for
  that specific channel (the Xtream Codes `tv_archive` flag), not something
  Dispatcharr generates itself for every channel.
- **You pick a specific past (or currently-airing) EPG entry from the guide
  and it plays from that programme's start**, with normal seek/rewind
  *within that one programme* (the catch-up endpoint supports HTTP Range,
  confirmed with a real `206` on a ranged request). It is not "press
  rewind while watching live and seamlessly scroll back" the way TVHeadend's
  HTSP-based timeshift buffer works -- Dispatcharr has no equivalent concept
  of a generic rolling per-channel live buffer, and this addon's live
  playback (`GetChannelStreamProperties()`) is still plain URL passthrough
  with no addon-managed stream lifecycle.
- `IsEPGTagPlayable()` only reports true once the programme has actually
  started (`GetStartTime() <= now`) and is still within the channel's
  `catchupDays` retention window -- there's no way to query the provider's
  *actual* current archive depth per programme, so this is a best-effort
  window check, not a guarantee the archive still has that exact programme.

### Seeking reliability during catch-up playback

Reported as unreliable in practice. Isolated the cause by testing the raw
HTTP mechanics directly against a live instance: the catch-up endpoint's
`Accept-Ranges`/`Content-Range` handling is correct and fast at every offset
tried (six different large offsets across a 2.5GB archive file, all `206`,
all under 2s, verified the returned bytes actually differ between offsets).
So the unreliability isn't Dispatcharr's HTTP layer -- it's Kodi's own
generic MPEG-TS-over-HTTP seeking (plain `CCurlFile` + FFmpeg demuxer),
which estimates byte-position-from-time via internal PCR/bitrate sampling, a
well-known source of imprecise/flaky seeking for this content type in
general, independent of the server.

**Tried and reverted:** routing catch-up playback through the separate
`inputstream.ffmpegdirect` addon's `stream_mode: timeshift`, on the theory
that its seek handling would be more robust than Kodi's generic MPEG-TS
seeking. Confirmed live against a real install that this doesn't just fail
to help -- it breaks seeking entirely (no seeking at all, worse than the
original flakiness). Root-caused by reading ffmpegdirect's actual source
(`github.com/xbmc/inputstream.ffmpegdirect`, branch `Piers`):

- `stream_mode: timeshift` (`src/stream/TimeshiftBuffer.cpp`) seeks only
  within a local, segmented recording that ffmpegdirect itself
  progressively downloads from what it assumes is a *live*, continuously
  arriving source. Our catch-up URL is a single, already-complete archived
  file, not a live source -- there is nothing for it to progressively
  record, so its seek model doesn't apply.
- `stream_mode: catchup` (`src/stream/FFmpegCatchupStream.cpp`,
  `SeekCatchupStream()`) seeks by reconstructing a *new* URL for the exact
  wall-clock time being sought to, via a `catchup_url_format_string`. This
  requires the backend to support starting playback at an arbitrary
  in-programme timestamp. Dispatcharr's catch-up API does not support
  this: its `start` parameter only selects *which* archived programme to
  fetch, never a time offset within it -- in-programme position is meant
  to be handled entirely via HTTP Range on the byte stream.

Neither of ffmpegdirect's specialized modes matches Dispatcharr's actual
catch-up URL shape (one complete, Range-seekable file per programme), so
the addon now plays the catch-up URL directly via
`PVR_STREAM_PROPERTY_STREAMURL` -- the same mechanism as live channels --
plus `PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE` (a plain Kodi-core flag,
unrelated to ffmpegdirect, that just nudges Kodi's own UI to treat the
session more like live TV). Seeking precision on raw MPEG-TS via Kodi's
built-in PCR/bitrate-based estimation remains inherently approximate; no
further improvement path has been identified without a backend change
(e.g. Dispatcharr transcoding catch-up to a seek-friendly container/format,
or exposing a proper index), which is out of scope for this addon.

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

## Channel switching fails after the first channel (root cause: IPv6)

**If channel N+1 never plays after channel N worked, and this repeats for
every subsequent switch (not just once), check whether your Dispatcharr
host resolves to both an IPv4 and an IPv6 address, and whether the IPv6
route is actually reachable.** This was root-caused (not guessed) against a
real deployment and is almost certainly the first thing to check before
suspecting the addon, Dispatcharr, or the upstream IPTV provider.

What was actually observed, via Kodi's own `debug.setextraloglevel=64`
(`LOGCURL`) trace: opening the *first* channel, curl tried the host's IPv6
address, that failed/timed out, and after several retries across ~15s it
fell back to IPv4 and succeeded. Opening the *second* channel to the same
host, curl tried IPv6 **only** -- it never fell back to IPv4 at all, and sat
timed out for the full 30s Kodi gives it. This repeated identically for every
subsequent switch, including switching back to a channel that had played
fine moments earlier -- i.e. this has nothing to do with which channel, or
even that a switch is happening at all; it's specifically about Kodi's own
HTTP connection handling to that *host* misbehaving after the first request.

In the deployment this was diagnosed against, the Dispatcharr host's IP was
a Gluetun (VPN client) container's LAN IP -- Gluetun commonly blocks IPv6
outright as a leak-prevention measure, so the AAAA record pointed at an
address nothing could ever actually reach, even though DNS kept advertising
it. **The fix was entirely DNS-side: remove/disable the AAAA record for the
Dispatcharr hostname (or otherwise ensure it only resolves to an address
that's genuinely reachable over IPv6, or doesn't advertise IPv6 at all).**
Confirmed: switching straight to the plain IPv4 address in the addon's Host
setting fixed it immediately, before the DNS change was made.

This is *not* something this addon's code can reliably route around --
Kodi's `CCurlFile`/libcurl URL options (see `xbmc/filesystem/CurlFile.cpp`)
don't expose a way to force IPv4-only resolution, and having the addon
resolve the hostname to a hardcoded IPv4 address itself for the stream URL
would break certificate validation for anyone using HTTPS. If you hit this
and can't fix the DNS/network layer, using the bare IPv4 address in the
addon's Host setting instead of a hostname is the reliable workaround.

Two mitigations were tried and ruled out before finding this, kept here so
they aren't re-attempted:
- **A fixed delay before switching** (a `channel_switch_delay_seconds`
  setting was added, default 2s) -- didn't help even at 10+ seconds, which
  is what proved this wasn't a timing race with Dispatcharr's proxy
  releasing the previous connection.
- **`|Connection=close` on the stream URL** (a real Kodi/`CurlFile` URL
  option, confirmed applied in the log) -- didn't help either, which is what
  proved this wasn't ordinary HTTP keep-alive/connection-pool reuse.

Also ruled out: Dispatcharr/Unraid worker capacity (rapid channel zapping
through Dispatcharr's own web UI against the same instance worked
perfectly), and a dead/rate-limited channel (the exact failing URL streamed
real data fine when requested independently, and switching *back* to the
first, previously-working channel failed identically).

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
