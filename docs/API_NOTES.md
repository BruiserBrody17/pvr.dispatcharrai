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
- Dispatcharr's `SeriesRuleRequest.mode` (`"all"` vs `"new"`, i.e. record
  every episode including reruns vs first-run only) wasn't exposed at all
  when creating a series rule -- it always used the server's `"all"`
  default. Kodi's PVR API has a purpose-built control for exactly this,
  `PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES` (paired with
  `PVRTimer::SetPreventDuplicateEpisodes()`), which surfaces as a normal
  "Prevent duplicate episodes: Record all episodes / Record only new
  episodes" field in Kodi's own Timer Settings dialog when creating an
  "Add timer" (series) rule from the guide. Wired up and confirmed
  end-to-end through that real dialog: selecting "Record only new
  episodes" and saving produced a rule with `"mode":"new"` on the server.
  `GetTimerRules()` also reads the field back for existing rules, so an
  already-created rule shows the right selection if inspected again (not
  confirmed whether *editing* an existing rule's setting actually takes
  effect server-side -- this addon doesn't implement `UpdateTimer()` at
  all, a pre-existing gap unrelated to this specific field).
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
- **A new recording never showed up under Recordings until Kodi was
  restarted.** `AddTimer()` called `TriggerTimerUpdate()` but never
  `TriggerRecordingUpdate()` -- Kodi has no reason to re-poll
  `GetRecordings()` on its own just because a timer was added, and a
  recording that starts at or near "now" (including Kodi's "Record"
  button on a live guide entry, see above) may already be actively
  recording by the time `AddTimer()` returns. Confirmed by waiting 90+
  seconds after creating a recording with the old code -- it never
  appeared until a full restart. Now calls both triggers.
- **A completed recording still showed as "Recording &lt;id&gt;" instead
  of its real title, indefinitely -- even after the recording finished.**
  Confirmed directly against the server for a real, fully-completed
  recording: `custom_properties.program.title` was correct there the
  whole time. The problem is on Kodi's side: Dispatcharr only populates
  `custom_properties` a moment *after* a recording actually starts (right
  at creation it's `{}`), but nothing prompts Kodi to look at a *already
  known* recording's metadata again once it's cached it -- confirmed
  nothing else about the recording being displayed differently at any
  point in its life re-triggers a refetch, so whatever `GetRecordings()`
  returned on Kodi's very first look (our `"Recording <id>"` fallback,
  since enrichment hadn't happened yet) stuck around forever, completed
  recording or not. The `TriggerRecordingUpdate()` call in `AddTimer()`
  fired immediately, before that enrichment window, which is exactly
  what caused Kodi's first look to be too early. Fixed with a second,
  delayed (5s) `TriggerRecordingUpdate()` call for one-time recordings,
  giving Dispatcharr time to populate the title before Kodi's next
  fetch. If you already have a recording stuck showing "Recording
  &lt;id&gt;" from before this fix, its title is genuinely correct
  server-side already -- a plain Kodi restart will pick it up, no need
  to touch anything on Dispatcharr's side.
- **Stopping an in-progress recording from Kodi deleted it entirely
  instead of just stopping it -- confirmed to have actually destroyed a
  real recording, not just a theoretical risk.** Root cause, confirmed
  against Kodi's own source (`xbmc/pvr/timers/PVRTimers.cpp`): Kodi's
  `DeleteTimer()` addon call takes a `forceDelete` flag that specifically
  means "this timer is still actively recording" -- both the dedicated
  "Stop Recording" action and choosing "Delete" on a timer Kodi already
  knows is recording pass `forceDelete=true`; a timer that's merely
  scheduled (not yet started) passes `false`. This addon's `DeleteTimer()`
  ignored that flag entirely and always called `DeleteRecording()` --
  `DELETE /api/channels/recordings/{id}/`, which "removes the associated
  file(s) from disk" per its own description -- regardless of whether the
  recording was still being written. Confirmed against the live schema
  that Dispatcharr has a separate, purpose-built endpoint for exactly
  this: `POST /api/channels/recordings/{id}/stop/`, documented as "Stop a
  recording early while retaining the partial content for playback."
  Fixed: `forceDelete=true` now calls `StopRecording()` (the `/stop/`
  endpoint) instead. Verified end-to-end through Kodi's real "Stop
  recording" UI action (not just a direct API call): the recording
  remained listed afterward with real bytes written, `remux_success:
  true`, and a normal (non-HLS) `/file/` URL -- fully playable, exactly
  as documented -- and that a genuinely non-recording timer's delete
  (`forceDelete=false`) still correctly removes it entirely via
  `DeleteRecording()`.
- Stopping a recording early leaves its `end_time` at the originally
  *scheduled* value -- Dispatcharr doesn't rewrite it to the actual stop
  time, only `custom_properties.stopped_at` reflects that. `isInProgress`
  was computed purely from `start_time <= now < end_time`, so a
  recording stopped well before its scheduled end kept showing as
  actively recording in Kodi's timer list for the entire remainder of
  that original window, even though `custom_properties.status` was
  already `"stopped"` and the file was already complete and playable.
  Confirmed end-to-end (stopped a real in-progress recording via Kodi,
  it kept showing as a timer). Fixed by trusting `custom_properties.status`
  over the time window when present: `"recording"` means in-progress,
  any other non-empty value means finished, matching the confirmed
  values (`"recording"`/`"completed"`/`"stopped"`/`"interrupted"`)
  without assuming that's a closed set. Also added a
  `TriggerRecordingUpdate()` after a successful stop/delete (previously
  only `TriggerTimerUpdate()`), for the same reason `AddTimer()` needed
  one: the change affects the Recordings view too, not just Timers.
- **A recording that *did* show up under Recordings still failed to
  play, silently ("Error creating demuxer" in the log, no player ever
  started).** This took real digging, and an earlier note in this file
  claiming in-progress recording playback worked end-to-end was wrong --
  it did once, but wasn't actually reproducible, and the real mechanism
  turned out to be different from what that note assumed. Confirmed
  against Kodi's own source
  (`xbmc/cores/VideoPlayer/DVDInputStreams/DVDFactoryInputStream.cpp`):
  any `pvr://recordings/...` path is demuxed through
  `CInputStreamPVRRecording`, which calls the addon's
  `OpenRecordedStream()`/`ReadRecordedStream()`/`SeekRecordedStream()`/
  `LengthRecordedStream()` -- **but only if `GetRecordingStreamProperties()`
  leaves `PVR_STREAM_PROPERTY_STREAMURL` unset.** An earlier version of
  this note claimed STREAMURL is *never* consulted for a recording and is
  harmless to populate regardless; that turned out to be wrong -- a real
  failure (see the API-key note below) was root-caused to Kodi's generic
  `CCurlFile` opening a populated STREAMURL directly, bypassing these
  callbacks (and their retry logic) entirely, confirmed via a live
  `kodi.log`. This addon never implemented those callbacks originally, so
  Kodi's default
  `OpenRecordedStream()` (`return false;`) meant every single recording
  playback attempt failed immediately, with no network request even
  attempted. Fixed by implementing real byte-range HTTP reads
  (`DispatcharrClient::OpenRecordingStream()`/`ReadRecordingStream()`/
  `SeekRecordingStream()`/`GetRecordingStreamLength()`) against
  `/api/channels/recordings/{id}/file/`, using the `X-API-Key` header
  (see the permissions note below for why that's required at all).
  Confirmed end-to-end against a real completed recording: real playback
  progress, correct duration, and working seeks (verified via
  `Player.Seek`).
  **In-progress recordings are not supported by this fix** -- `/file/`
  redirects to an HLS playlist (`.../hls/index.m3u8`) while a recording
  is still being written, and each individual `.ts` segment inside that
  playlist independently requires the same `X-API-Key` header, which
  Kodi's own HLS demuxer has no way to know to send for segments it
  discovers by parsing the playlist itself. `OpenRecordingStream()`
  detects this case (by content-type/URL) and fails with a clear error
  instead of trying and silently corrupting playback. Watching a
  recording while Dispatcharr is still actively writing it remains
  unsupported; wait for it to finish.
- Both endpoints above (recording file and the HLS redirect target) also
  confirmed to return a flat **403 for a fully anonymous request**,
  despite their schema listing anonymous access (`{}`) as one of the
  allowed security schemes -- a Bearer token or `X-API-Key` header is
  actually required. A JWT access token expires after 30 minutes
  (confirmed by decoding one -- `exp - iat` -- see the login note above),
  too short for most recordings, so this addon generates a Dispatcharr
  API key on first use via `POST /api/accounts/api-keys/generate/` and
  persists it to its own `api_key` setting. Regenerating that endpoint
  replaces the account's previous key (confirmed: two calls returned two
  different keys) -- **account-wide, not per-installation**, which matters
  once more than one Kodi install shares the same Dispatcharr account; see
  "Known limitations with more than one Kodi client" below for the
  self-heal this addon now does about it.
- **`ReadRecordingStream()` used to open a brand-new libcurl easy handle
  (fresh TCP connection, fresh TLS handshake if HTTPS) for every single
  demuxer read**, rather than reusing one across the life of an open
  recording. Negligible on a low-latency LAN/Ethernet link, but confirmed
  (via a companion session's real measurements over WiFi, 10-15ms jittery
  RTT to the same host) to starve playback on a higher-latency link even
  with plenty of raw bandwidth for the recording's bitrate: one bulk
  100MB range request over a single connection measured 68.5 MB/s, but 60
  sequential 64KB reads with a fresh connection each (matching the old
  per-read pattern) measured only 1.13 MB/s effective throughput -- barely
  above the ~6.9 Mbps a real recording needed, and reproduced live as
  `CVideoPlayerAudio::Process - stream stalled` a few seconds into
  playback. Fixed by keeping one persistent `CURL*` in
  `RecordingStreamState`, reused across reads so libcurl's own connection
  cache lets keep-alive apply, and only torn down on a transport-level
  error (in case a long-idle keep-alive connection went stale) or on
  `CloseRecordingStream()`. Verified on the Windows/Ethernet side by
  watching `netstat` during live playback: one connection stayed
  `ESTABLISHED` for the full duration of an 8-second sampling window
  instead of new ports cycling through `ESTABLISHED`/`TIME_WAIT` on every
  read.
- The same per-call fresh-connection cost also applied to `Request()`, the
  helper behind essentially every other API call (login, `GetChannels()`,
  `GetRecordings()`, `AddTimer()`'s `CreateOneTimeRecording()`, etc.) --
  not as hot a path as recording reads, but a companion session found that
  a single "Record" press fires several of these in a row, and on WiFi
  each one is independently exposed to a connection-setup latency spike:
  measured 20ms/call under calm conditions but 1.8-10s before Kodi's own
  "recording started" notification appeared under worse ones, on
  identical code across repeated runs -- pointing at intermittent
  connection setup, not a deterministic slow path. Unlike
  `ReadRecordingStream()`, `Request()` can't just reuse one `CURL*`: Kodi's
  PVR API calls into this client from multiple threads (see the class
  comment in `DispatcharrClient.h`), and a single easy handle isn't safe
  for concurrent use. Fixed with a `CURLSH` share object (connection/DNS/
  TLS-session cache) applied to every easy handle this client creates,
  with mutex-backed lock/unlock callbacks -- libcurl doesn't lock a share
  object internally, that's on the application. Verified no regressions
  across channels/recordings/timers/`AddTimer()`/playback after the
  rebuild.
  This closed most, but confirmed not all, of the gap: the companion
  session's post-fix WiFi timing was 2/3 runs at ~0.1s (matching the raw
  ~20ms API latency) but one run at 8.4s, still in the original
  complaint's range. They ruled out the network path for that outlier --
  a 30-second/60-packet ping to the Dispatcharr host in a calm window
  right after showed 0% loss, 6-17ms throughout, no anomaly -- and their
  read (not confirmed, no lower-level instrumentation attempted) is macOS
  WiFi radio power-save/idle-wake behavior: if the radio dozes during a
  quiet spell between guide navigation and the record press, the next
  transmission can eat a real multi-second wake latency no HTTP-layer fix
  touches, and a sustained ping (which itself keeps the radio busy)
  wouldn't reproduce it. Left as a documented, likely-environmental
  caveat rather than chased further in the addon -- if confirmed later,
  the fix would be periodic background keep-alive traffic to prevent the
  radio from idling, but that's a real battery-life cost to pay for an
  unconfirmed cause and an already-rare (1 of 3 runs, likely rarer in
  normal use than in back-to-back test cycles) case.
- Unrelated discovery while testing the fix above: Kodi can reject
  `PVR.AddTimer` outright with "The PVR backend does not allow to record
  this event" for some EPG broadcasts and not others, with **zero** log
  output from this addon (confirmed: no `AddOnLog: pvr.dispatcharrai`
  line at all) -- meaning the rejection happens entirely in Kodi core,
  before ever reaching `AddTimer()`. Not investigated further (out of
  scope, and the exact same broadcastid succeeded cleanly and instantly
  moments later), but worth knowing so a rejected recording isn't
  mistaken for an addon bug: check for a scheduling conflict on that
  channel first (a channel already mid-recording will reject an
  overlapping one, which explains at least one case seen).
- A freshly-created recording can briefly show as `"Recording <id>"`
  instead of its real title, until Dispatcharr's own async enrichment
  (`custom_properties.program.title`, see above) catches up and a later
  refresh picks it up. Kodi already has the correct title *before* this
  addon is ever called, though: `CPVRTimerInfoTag::CreateFromEpg()`
  populates it from the EPG tag the user pressed "Record" on, and
  `AddTimer()` was just discarding it (`CreateOneTimeRecording()`'s
  `title` parameter went unused, deliberately, to avoid the
  custom_properties-replace-not-merge trap noted above). Fixed by caching
  that title client-side (`DispatcharrClient`'s `PendingTitle`, matched by
  channel, not also start time -- Dispatcharr silently clamps a recording's
  stored `start_time` to the moment it actually began for an
  already-airing EPG event, confirmed against a real one, so exact-time
  matching missed the single most common case: "Record" on something
  currently on) and using it in `GetRecordings()` in place of the
  `"Recording <id>"` fallback. Live-tested against several real EPG
  broadcasts (including an already-airing one) and confirmed the correct
  title end to end with no regressions -- but this server's own
  enrichment turned out to be fast enough in testing (both for
  already-airing and future-scheduled recordings) that the exact race
  this fixes couldn't be reliably reproduced live; the fix is
  correct-by-construction (a pure fallback, only consulted when the
  server-provided title is still empty) rather than confirmed against a
  reproduced failure the way most fixes in this file are.

## Known limitations with more than one Kodi client

Not bugs in this addon -- inherent to running multiple, fully independent
Kodi installations (e.g. one on Windows, one on macOS) against the same
Dispatcharr server, worth writing down since it's easy to mistake for one:

- **A recording created on one Kodi instance doesn't appear on another
  until that other instance happens to refresh.** This addon has no
  push/notification channel from Dispatcharr (it's a plain REST poller),
  and `TriggerTimerUpdate()`/`TriggerRecordingUpdate()` only tell *that
  specific running addon instance's* Kodi to re-fetch -- they have no way
  to reach a separate Kodi installation's separate addon instance.
  Confirmed: a recording created via one Kodi's guide didn't appear on a
  second, independently-running Kodi until that second instance was
  restarted. Restarting (or waiting for Kodi's own periodic PVR refresh)
  on the second instance is the only way to see it sooner.
- **Playback resume position doesn't carry over between Kodi
  instances.** Kodi's "resume from where you left off" bookmark is
  stored in that Kodi installation's own local video database, not
  anywhere this addon controls or Dispatcharr is aware of -- watching
  partway through a recording on one device has no way to inform a
  different device's Kodi where to resume. Kodi's PVR API does have
  purpose-built hooks for exactly this
  (`GetRecordingLastPlayedPosition`/`SetRecordingLastPlayedPosition`,
  meant for backends that track resume position server-side instead of
  relying on Kodi's local database), which this addon doesn't currently
  implement. It's a real, legitimate feature to add if cross-device
  resume matters, but not attempted here yet -- it would need a place to
  actually store the position server-side, and `custom_properties` is the
  only candidate on the `Recording` object, whose write semantics
  (whether a `PATCH` merges or replaces the field) need to be verified
  against a **disposable test recording** before ever touching a real
  one, given `custom_properties` was already confirmed to be fully
  replaced rather than merged on `POST` create (see above).
- **One install's addon can silently invalidate another install's stored
  API key, breaking recording playback with no obvious cause.** Dispatcharr
  keeps exactly one active API key per account (see the permissions note
  above); if two Kodi installs share an account and each generates its own
  key once, whichever install last regenerated invalidates the other's
  stored copy. Confirmed end-to-end (Windows + macOS installs against the
  same account, same recording): the macOS side got `CCurlFile ... Failed:
  HTTP returned code 401` on a key that was valid when it was generated,
  while a fresh `generate/` call from either side proved the account only
  keeps one key alive at a time. Fixed by making `OpenRecordingStream()`/
  `ReadRecordingStream()` treat a 401 as "this key was invalidated
  elsewhere," not a hard failure: they call `GenerateApiKey()` and retry
  once before giving up, and `PVRDispatcharr` re-persists the new key to
  the addon's settings if it changed. This doesn't stop the two installs
  from continuing to invalidate each other's cached key -- it just makes
  that invisible, since whichever side needs the key next self-heals
  within one HTTP round-trip instead of failing outright. Verified by
  deliberately corrupting a live install's stored key and confirming
  playback still succeeded, with the corrected key written back to
  `settings.xml` automatically.

## Still unconfirmed (verify before relying on in production)

- Whether a one-time recording that doesn't match any real EPG programme
  (a fully manual/custom time range with nothing airing to auto-enrich
  from) ever gets a title *from Dispatcharr itself*. Less consequential
  than it used to be: the pending-title cache above now shows whatever
  title Kodi's manual-timer dialog was given regardless, so this only
  matters for whether Dispatcharr's own data independently agrees once
  its enrichment (or lack thereof) resolves -- not tested.
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
