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
| Recordings list/create | GET/POST | `/api/channels/recordings/` -- **bare array** on GET, not `{results:[...]}`. `Recording` has only `id`, `start_time`, `end_time`, `task_id`, `custom_properties`, `channel` -- **no title/subtitle/description/duration/in-progress fields at all.** Those are derived: duration from `end_time - start_time`, in-progress from `start_time <= now < end_time`, and title/subtitle/description read (best-effort, unconfirmed key names) from `custom_properties` on the same `title`/`sub_title`/`description` convention `ProgramData` uses elsewhere in this API. |
| Recording delete | DELETE | `/api/channels/recordings/{id}/` |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` (anonymous, Range-seekable) |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` -- body `{tvg_id}`, both optional |
| Series rules list | GET | `/api/channels/series-rules/` -- returns **`{"rules": [...]}`**, not a bare array or `{results:[...]}`. Confirmed live (empty instance); exact per-item field names inside `rules` are still unconfirmed -- no populated rule was available to inspect (see permissions note below). |
| Series rule create | POST | `/api/channels/series-rules/` -- body is `{title, tvg_id?, channel_id?, mode?, title_mode?, description?, description_mode?, untagged_is_new?, epg_source_id?}`. **Not** `{channel, title_pattern}** -- confirmed against the live `SeriesRuleRequest` schema. `channel_id`/`tvg_id` are both optional (channel_id "defaults to lowest-numbered channel for the EPG" if omitted). |
| Series rule delete | DELETE | `/api/channels/series-rules/?title=...&tvg_id=...&epg_source_id=...` (query params) -- confirmed against the live schema. **There is no `/api/channels/series-rules/{id}/` route**; series rules have no path-addressable id at all. |
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
catch-up URL shape (one complete, Range-seekable file per programme).

Also tried and reverted alongside it: `PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE`
(a plain Kodi-core flag, unrelated to ffmpegdirect) to make the OSD feel
more like live TV. Per Kodi's own PVR client header
(`xbmc/pvr/addons/PVRClient.h`), setting it makes Kodi call back into
`GetChannelStreamProperties()` -- the *live-channel* path -- instead of
just using the catch-up URL returned from `GetEPGTagStreamProperties()`.
That's the live-channel code path, not built for a static archived file,
and made seeking worse, not better. Confirmed by removing it: seeking
returned to the original (flaky-but-present) behavior.

So the addon now plays the catch-up URL directly via
`PVR_STREAM_PROPERTY_STREAMURL` -- exactly the same three properties as
the original catch-up implementation, before either of these two attempts
at improving seek reliability. Seeking precision on raw MPEG-TS via Kodi's
built-in PCR/bitrate-based estimation remains inherently approximate; no
further improvement path has been identified without a backend change
(e.g. Dispatcharr transcoding catch-up to a seek-friendly container/format,
or exposing a proper index), which is out of scope for this addon.

### Live TV pause/rewind ("timeshift")

Requested as "timeshifting with the live TV buffer held on the Dispatcharr
side, similar to how tvheadend does timeshifting with Kodi." Confirmed
Dispatcharr has no equivalent to TVHeadend's server-side rolling live
buffer -- there's nothing in its API for a continuous, always-recording,
seekable live buffer per channel. That rules out a true server-side
implementation matching TVHeadend's model.

What's implemented instead, gated behind the `enable_live_timeshift`
setting (off by default): `GetChannelStreamProperties()` optionally routes
live channel playback through `inputstream.ffmpegdirect`'s `stream_mode:
timeshift`. Confirmed via ffmpegdirect's own README this is exactly what
that mode is for -- unlike the catch-up case above, this isn't a
mismatch: timeshift mode is explicitly designed to add pause/rewind to a
plain, continuously-arriving live stream with no backend cooperation
required at all, by recording it to a local on-disk buffer as it plays.
That's a materially different architecture than TVHeadend's -- the buffer
lives on the Kodi device's own storage (size/path/retention controlled by
ffmpegdirect's own addon settings), not on the Dispatcharr server, so it
doesn't persist across a Kodi restart and isn't shared between devices --
but it delivers the same pause/rewind/fast-forward gesture the user
actually interacts with.

Requires `inputstream.ffmpegdirect` to be installed; if it isn't and the
setting is on, live channel playback fails outright (not just timeshift),
which is why the setting defaults to off and the in-app help says so.

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

## Recordings/timers: confirmed end-to-end against real data

Once the account's permissions were raised (see the permissions note
below), a real recording and two real series rules were created, listed,
and deleted -- both directly over the API and through Kodi's own PVR
manager (`PVR.GetTimers`/`PVR.DeleteTimer` via JSON-RPC) -- confirming:

- A `Recording` created with **no** `custom_properties` gets auto-enriched
  by Dispatcharr itself from whatever EPG programme was actually airing,
  nested as `custom_properties.program.{title,sub_title,description}`
  (alongside `status`, `file_url`/`output_file_url` pointing at an
  in-progress HLS playlist, `file_name`/`file_path` for the eventual MKV,
  and `poster_logo_id`). Sending your own `custom_properties` on create
  **replaces** this entirely rather than merging with it -- confirmed by
  comparing a recording created with an explicit `custom_properties.title`
  (which got exactly that flat object back, nothing else) against one
  created with none (which got the full auto-populated object above).
  `CreateOneTimeRecording()` no longer sends its own `custom_properties`
  as a result, and `GetRecordings()` reads the nested `program.*` fields
  first, falling back to flat `custom_properties.title` etc. for anything
  that did set them directly.
- A series rule has **no numeric id field at all** -- a real one is just
  `{mode, title, tvg_id, channel_id, title_mode, description,
  description_mode}`. `GetTimers()` previously used `rule.id` (always 0)
  to build each series timer's Kodi `ClientIndex`, which would collide for
  any second series rule; now hashes `(title, tvgId)` instead -- confirmed
  with two simultaneous rules that they now show as distinct timers.
- `DeleteSeriesRule()`'s title+tvg_id query-param delete and
  `DeleteRecording()`'s path-id delete were both confirmed to actually
  remove the item server-side (checked directly against the API after
  deleting through Kodi), not just update Kodi's local view of it.
- **Pressing Kodi's "Record" button on an EPG guide entry never actually
  created a recording** -- reported as: a brief delay, then a repeating
  "Switch / Record / Cancel" dialog that just re-appears no matter which
  button is pressed. Root cause confirmed against Kodi's own source
  (`xbmc/pvr/timers/PVRTimerInfoTag.cpp`, `CreateFromEpg()`): that code
  path requires a timer type with neither `PVR_TIMER_TYPE_IS_MANUAL` nor
  `PVR_TIMER_TYPE_IS_REPEATING` set. This addon's two timer types had
  `IS_MANUAL` (the one-time type, needed for Kodi's separate "new manual
  timer, no EPG event" flow) and `IS_REPEATING` (the series type) --
  neither qualified, so Kodi could never build a timer object for a plain
  "record this guide entry" press, regardless of anything the addon's own
  `AddTimer()` does (it was never being reached at all). Fixed by adding a
  third type (`kTimerTypeOneTimeEpgBased`) with neither flag; confirmed by
  actually driving Kodi's guide UI (context menu -> Record) end-to-end and
  watching a real Dispatcharr recording appear and start writing an MKV.
  The dialog itself is likely `CPVRGUIActionsTimers::AnnounceReminder()` --
  wasn't reproduced directly, but its button set is the only exact match
  in Kodi's source for those three labels with an auto-close countdown,
  and Kodi's UI paths documented above only ever show a plain one-button
  "Timer creation failed" dialog for this specific failure, so there may
  be a Kodi-version-specific difference in exactly which dialog surfaces
  it -- the underlying missing-timer-type cause and its fix are confirmed
  either way.
- **Clicking an in-progress recording did nothing.** Two separate bugs
  stacked here:
  1. `GetRecordings()` was excluding any `isInProgress`/`isUpcoming`
     recording entirely, on the assumption that `GetTimers()` covered
     those. Wrong: Kodi's own `CPVRRecording::IsInProgress()`
     (`xbmc/pvr/recordings/PVRRecording.cpp`) works by cross-referencing
     `GetRecordings()`'s list against the active timer list by
     channel+time overlap -- it expects a still-recording item to be
     listed in **both** places at once. Omitting it from `GetRecordings()`
     meant it only ever existed as an uneditable timer row, nothing
     clickable. Fixed to only exclude genuinely upcoming (not yet started,
     nothing to play) recordings.
  2. Even once listed, `/api/channels/recordings/{id}/file/` (and its
     redirect target while still recording, `.../hls/index.m3u8`) both
     returned a flat **403 for an anonymous request** against a real
     instance -- confirmed for both an in-progress and a fully completed
     recording, despite this endpoint's schema listing anonymous access
     (`{}`) as one of its allowed security schemes. A request with either
     a Bearer token or an `X-API-Key` header succeeds. A JWT access token
     expires after 30 minutes (confirmed by decoding one -- `exp - iat`
     -- see the login note above), too short for most recordings, so this
     addon now generates a Dispatcharr API key on first use via `POST
     /api/accounts/api-keys/generate/` and persists it to its own
     `api_key` setting, appended to the recording stream URL as an
     `X-API-Key` header via Kodi's `|key=value` stream-URL syntax.
     Regenerating that endpoint replaces the account's previous key
     (confirmed: two calls returned two different keys), which is why
     this only ever generates one and caches it, rather than doing so on
     every addon start.
  Confirmed by actually creating a real in-progress recording and playing
  it through Kodi end-to-end (real bytes streamed, correct duration
  reported), not just inspecting the code.

## Still unconfirmed (verify before relying on in production)

- Whether a one-time recording that doesn't match any real EPG programme
  (a fully manual/custom time range with nothing airing to auto-enrich
  from) gets *any* usable title, or falls back to this addon's
  `"Recording <id>"` default -- only the common EPG-matched case (which
  does get a real title via the auto-enrichment above) was tested.
- The account used to verify this (`claude`) initially got "You do not
  have permission to perform this action" trying to create a recording or
  series rule -- same account that could log in, browse channels, and
  stream fine. Raising that account's role in Dispatcharr's admin UI
  resolved it. If you hit the same error, that's a Dispatcharr-side
  permissions setting, not an addon bug -- check the account's role before
  assuming otherwise.

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
