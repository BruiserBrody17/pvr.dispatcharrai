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
  **In-progress recordings are not supported by this byte-range fix** --
  `/file/` redirects to an HLS playlist (`.../hls/index.m3u8`) while a
  recording is still being written, and each individual `.ts` segment
  inside that playlist independently requires the same `X-API-Key`
  header, which Kodi's own HLS demuxer has no way to know to send for
  segments it discovers by parsing the playlist itself.
  `OpenRecordingStream()` detects this case (by content-type/URL) and
  fails with a clear error instead of trying and silently corrupting
  playback. See the separate `inputstream.ffmpegdirect`-based path below
  for how this is actually solved when opted into.
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
- **In-progress recording playback, via `inputstream.ffmpegdirect`
  (`enable_inprogress_playback` setting, off by default, experimental).**
  Confirmed query-param auth is **not** a usable alternative to the
  `X-API-Key` header for the HLS segment endpoints (`?api_key=` and
  `?X-API-Key=` both got a flat 403; only the real header works), so
  fixing this needed something that could attach a header to every
  segment fetch, not just the manifest. `inputstream.ffmpegdirect`'s
  plain pass-through mode does that -- confirmed by reading its actual
  source, not just its docs -- but two details matter, both found by
  reading `FFmpegStream.cpp` directly rather than guessing from the two
  already-reverted attempts elsewhere in this addon (live TV's
  `stream_mode: timeshift` and catch-up's `timeshift`/`catchup`, both
  reverted for *seeking* reasons that don't apply to a forward-only,
  still-growing in-progress recording):
  1. A plain `http(s)://` URL defaults to `inputstream.ffmpegdirect`'s
     `OpenWithCURL()` code path, not `OpenWithFFmpeg()` -- confirmed via
     source that `OpenWithCURL()` sets no header options at all when
     opening the format context, silently reproducing the exact same
     segment-auth failure this was meant to fix. Must explicitly set
     `inputstream.ffmpegdirect.open_mode=ffmpeg` to force the code path
     that actually calls `GetFFMpegOptionsFromInput()`.
  2. Even in FFmpeg-native mode, `GetFFMpegOptionsFromInput()` only maps
     a fixed allowlist of standard HTTP header names to real headers --
     anything else (including `X-API-Key`) is silently dropped unless
     prefixed with a literal `!`, which it strips before using the rest
     as the header name. Confirmed by a real failed attempt logging
     `ignoring header option 'X-API-Key'` with the plain name, and a
     second attempt with `!X-API-Key` succeeding.
  No `stream_mode` is set at all (neither `timeshift` nor `catchup`) --
  the recording's own HLS playlist is already a valid, correctly
  segmented structure; the only actual gap was header propagation to
  segments, not anything either specialized mode addresses.
  `GetRecordingStreamProperties()` checks the recording's live
  `isInProgress` status (via `GetRecordings()`) before taking this path
  at all; a completed recording is unaffected and still goes through
  `OpenRecordingStream()`/etc. as before.
  Verified end-to-end against a real in-progress recording: real video
  rendering (confirmed via screenshot, not just JSON-RPC state), playback
  time advancing in real time, and over a minute of continuous playback
  with no stalls.

  **Follow-up attempt (`is_realtime_stream=false`) turned out to be
  incomplete -- it fixed the advertised seek *capability* but not the
  actual join *position*, and the earlier "verified working" claim below
  was wrong.** Original theory, confirmed by reading `FFmpegStream.cpp`
  directly: `GetCapabilities()` only advertises
  `INPUTSTREAM_SUPPORTS_SEEK`/`PAUSE`/`ITIME` when `is_realtime_stream` is
  false, and Kodi's `CVideoPlayer` only performs its normal "seek to the
  requested start position on open" behaviour when seeking is advertised
  as supported. Fixed by setting both `PVR_STREAM_PROPERTY_ISREALTIMESTREAM`
  and `inputstream.ffmpegdirect.is_realtime_stream` to `false`. This part
  held up: seeking genuinely works once this is set (see below).

  What didn't hold up: the "starts at the true beginning" verification.
  It was checked only via Kodi's own JSON-RPC `Player.GetProperties`
  (`time`/`percentage`), which display position *relative to wherever the
  stream happens to begin*, not relative to the recording's true absolute
  start -- so a demuxer that joins near the live edge still reports
  `time: 0:11` right after open, because Kodi labels wherever playback
  starts as "0". That's not evidence of anything; it's what Kodi always
  shows at the start of any stream. The real join point is only visible in
  ffmpegdirect's raw `av_dump_format` log line
  (`Duration: N/A, start: <seconds>, ...`), which wasn't checked at the
  time.

  Caught when the user reported the bug still happening on a currently-
  recording game, re-tested live, and that dump line read
  `Duration: N/A, start: 9681.617944, bitrate: N/A`. Cross-checked against
  the recording's real `start_time` from Dispatcharr's API
  (`2026-08-31T23:39:00Z`) versus wall-clock time at the moment of the
  test (~02:19 UTC next day): elapsed time since recording start was
  ~2h40m (9600s), matching the logged `start: 9681.6` almost exactly.
  **libavformat's HLS demuxer is joining the still-growing (no-
  `#EXT-X-ENDLIST`) playlist at the current wall-clock live edge,
  independent of `is_realtime_stream`.** That property only ever
  controlled ffmpegdirect's *advertised* seek capability, never
  libavformat's own automatic join-point selection for a no-`ENDLIST`
  playlist (governed by its own `live_start_index` option, confirmed
  earlier -- see the `GetFFMpegOptionsFromInput()` note above -- to have
  no reachable passthrough through any property this addon can set).
  Whichever recording happened to be tested when this was first "verified"
  most likely also joined near its own live edge; it just wasn't caught
  because the only check was Kodi's relative-position display.

  The only known way to stop libavformat from applying live-edge-join
  logic at all is to make the playlist look like a complete, closed VOD
  list -- i.e., inject `#EXT-X-ENDLIST` into a copy of the playlist before
  handing it to ffmpeg. VOD-shaped HLS is always demuxed from segment 0
  with a full seek range, sidestepping `live_start_index` entirely rather
  than trying to override it (there is no property this addon can set
  that reaches it directly, confirmed via `GetFFMpegOptionsFromInput()`'s
  source). Real, accepted trade-off: once marked `ENDLIST`, ffmpeg treats
  the list as complete and stops polling for newly-appended segments, so a
  single playback session no longer tails the recording live -- catching
  up on brand-new content needs a stop/replay to re-fetch a fresh, larger
  snapshot (and per the resume-point finding above, that replay starts
  over from position 0 rather than where the last session left off).

  **First implementation attempt -- a `data:` URI built from a one-time
  fetch-and-rewrite of the playlist -- failed outright, and not for a
  reason specific to this addon or ffmpegdirect.** Implemented
  `GetInProgressRecordingStreamUrl()` to fetch the live playlist itself,
  rewrite every segment reference to an absolute URL (a data: URI has no
  base path for a relative reference to resolve against), append
  `#EXT-X-ENDLIST`, base64-encode the result, and hand ffmpegdirect
  `data:application/vnd.apple.mpegurl;base64,<payload>|!X-API-Key=<key>`
  as STREAMURL. `kodi.log` confirmed the rewrite itself worked correctly
  (the base64 payload decodes to a well-formed playlist with absolute
  `http://` segment URLs and a trailing `#EXT-X-ENDLIST`), but ffmpegdirect
  logged `Error, could not open file data:application/...` immediately.
  Root-caused by reading Kodi's own `CURL::Parse()` (`xbmc/URL.cpp:72`):
  it hard-requires the literal substring `"://"` to recognise a protocol
  at all (`strURL.find("://")`) -- a standard `data:` URI, correctly per
  RFC 2397, has no `"://"` anywhere in it, so Kodi's parser never
  recognises it as a protocol and falls into a `.zip`/`.apk` archive-path
  fallback that just treats the whole string as a literal filename
  instead. This isn't a struct size limit (`INPUTSTREAM_PROPERTY`'s
  `m_strValue`/`m_strURL` are plain `const char*`, not fixed buffers --
  checked and ruled out first) or anything ffmpeg-side -- it's that
  `PVR_STREAM_PROPERTY_STREAMURL`'s pipe-delimited
  `url|option=value` convention is built entirely on top of Kodi's own
  generic `CURL` class, which cannot represent a bare `data:` URI at all.
  **A `data:` URI is therefore not viable through this property, full
  stop -- not just for this addon, for any Kodi PVR/inputstream addon
  using STREAMURL this way.**

  **Implemented and confirmed working: a tiny local HTTP listener inside
  this addon's own process (`LocalPlaylistServer`), serving the rewritten
  playlist at `http://127.0.0.1:<port>/playlist/<id>.m3u8` instead of a
  data: URI.** A real `http://` URL parses through Kodi's `CURL` class
  exactly like the original live one did. Loopback-only, OS-assigned
  ephemeral port (`bind()` to `INADDR_LOOPBACK`, port `0`, then
  `getsockname()` for the actual port); started in `PVRDispatcharr`'s
  constructor only when `enable_inprogress_playback` is on, stopped in the
  destructor -- no listening socket held open for installs that never use
  this feature. Raw sockets, not curl (curl is client-only and can't
  listen): platform-conditional `winsock2.h`/`ws2tcpip.h` vs.
  `sys/socket.h`/`unistd.h`, same pattern as `WebSocketClient.h`, reusing
  the `ws2_32` link already added for that. Single connection at a time,
  no keep-alive -- a fresh connection per request is simpler and
  libavformat's HLS demuxer doesn't need one to reload a playlist
  repeatedly.

  First shipped version served one pre-computed snapshot per recording
  (`SetPlaylist()`, called once from `GetRecordingStreamProperties()`),
  fixing the join-position/seek problem at the cost of a session never
  tailing new segments recorded after it started -- reopening got a
  fresh, larger snapshot, but not the same session continuing to grow.
  **Superseded by a dynamic, per-request design (`SetPlaylistProvider()`)
  that fixes that too, confirmed working.** Read directly from
  libavformat's own `hls.c` (not guessed) to find the mechanism:
  `select_cur_seq_no()`'s live-edge-join computation --
  `FFMAX(pls->n_segments + live_start_index, 0)`, `live_start_index`
  defaulting to `-3` -- only ever runs on the very first segment
  selection, and clamps to the true first segment whenever the playlist
  has 3 or fewer segments listed *at that moment*, regardless of how much
  has actually been recorded. Separately, as long as a playlist never
  claims `#EXT-X-ENDLIST`, the same file's reload logic
  (`!pls->finished` gating a reload-interval check) keeps re-fetching it
  throughout playback on its own -- the actual mechanism newly-recorded
  segments get picked up by, entirely independent of the one-time join
  decision.

  `DispatcharrClient::FetchInProgressPlaylistSnapshot(recordingId,
  truncateForInitialJoin, error)` (renamed from
  `GetInProgressRecordingStreamUrl()`) is now called fresh on every HTTP
  request `LocalPlaylistServer` receives for that recording, not once at
  open: `truncateForInitialJoin` (true only for that recording's actual
  first request, tracked by the server) caps the rewritten playlist to 3
  segment entries to force the clamp above to land on the true first
  segment; every request after that gets the full, untruncated history.
  Whether to finally append `#EXT-X-ENDLIST` is decided fresh on every
  call too, from a live `GetRecordings()` check of the recording's
  current `isInProgress` state -- not fixed at open time -- so a session
  that's still open when the underlying recording actually finishes
  correctly transitions from "keep tailing" to "reach a clean end" on its
  own, without needing to be reopened. `PVRDispatcharr` registers a
  provider lambda wrapping this (still handling the api-key-persist
  dance the old one-shot code did), and still attaches `!X-API-Key` as a
  pipe-option on the outer STREAMURL for ffmpegdirect's own segment
  fetches, exactly as before.

  **The "revealing full history from the second request onward is safe"
  claim above turned out to be wrong, and was shipped on insufficient
  evidence -- corrected here, along with the actual fix.** Original
  verification checked this addon's own `isFirstRequest=1, lines=13,
  hasEndlist=0, containsSeg00000=1` log line (proving what this addon
  *served*) and Kodi's relative `Player.GetProperties` time display, but
  never rechecked the one genuinely conclusive signal used earlier in
  this file -- ffmpegdirect's own `av_dump_format` `start:` value -- for
  this specific design. That gap hid a real bug for weeks of testing on
  short (1-3 minute) recordings, where "true position 0" and "wherever
  the demuxer actually landed" were too close together to visibly
  distinguish. A user testing against an 11+ minute recording caught it
  cleanly: playback consistently showed different content on every
  attempt, all matching whatever was live at that moment. Rechecking the
  raw `start:` line confirmed it precisely --
  `start: 118.931` on a recording that was ~2 minutes old at open,
  `start: 433.875` at ~7.2 minutes, `start: 734.182` at ~12.2 minutes --
  matching the recording's current age each time, not 0, despite this
  addon's own logging correctly showing every single first request as
  truncated-and-starting-from-`seg_00000`.
  Root cause: `select_cur_seq_no()`'s live-edge join computation, while
  documented as a one-time operation on the very first segment selection,
  can in practice get re-applied several times across libavformat's own
  rapid reloads while it's still probing/settling in right after open. A
  first request capped to 3 segments correctly forced the *first*
  application of that computation to land on segment 0 -- but the second
  request revealing the *entire* history at once (jumping from 13 lines
  to sometimes 300+) meant that if the computation got re-applied again
  before probing settled, it would use that much larger count and land
  far from 0 instead.
  Fixed by growing the revealed segment cap gradually instead of jumping
  straight to the full history on request two: `LocalPlaylistServer`
  tracks a small, growing per-recording cap
  (`kInitialMaxSegments`/`kMaxSegmentsGrowthStep`, both 3) instead of a
  one-time `isFirstRequest` boolean, and `FetchInProgressPlaylistSnapshot()`
  takes that cap directly as an `int` rather than a bool. Every reload's
  segment count now stays close to the previous one's, so no matter how
  many times the join computation actually gets re-applied during the
  settling window, it can never land far from wherever it last was.
  A second bug turned up applying this fix, caught before shipping by
  deliberately testing the finish-transition case again: applying the
  still-growing cap *unconditionally* combined badly with appending
  `#EXT-X-ENDLIST` once a recording finishes -- if the cap hadn't yet
  caught up to the true segment count when the recording ended, the
  response would falsely declare an artificially truncated prefix (e.g.
  57 of several hundred true segments) "the complete file," cutting
  playback off at ~2 minutes into what was actually a much longer
  recording. Fixed by only applying the cap while still in progress; once
  finished, the cap is ignored and the full true history is revealed in
  the same response that finally appends `ENDLIST` (safe unconditionally,
  since a finished/`ENDLIST`-terminated playlist takes hls.c's simple
  `return pls->start_seq_no` path and never reaches the live-edge
  computation at all).
  Re-verified end-to-end with the actual conclusive signal this time:
  against a ~10-minute-old recording, `start: 14.013` (not ~600s);
  against a ~19.5-minute-old one, `start: 13.829` (not ~1170s). Confirmed
  continuous, gapless playback throughout via repeated `Player.GetProperties`
  polling against wall-clock elapsed time. Confirmed the finish transition
  separately: stopping the underlying recording mid-session produced a
  response with the full ~170-segment true history (not capped) alongside
  `hasEndlist=1`, and playback continued normally past where the old,
  buggy version would have cut off.

  **Real, accepted trade-off, and different from the one-time-snapshot
  version's trade-off:** a still-growing (no-`ENDLIST`) playlist reports
  an unknown duration to libavformat (`Duration: N/A` in its own
  `av_dump_format` line, versus a real finite value once `ENDLIST`
  finally appears), and per the duration-metadata finding elsewhere in
  this file, Kodi's own PVR layer appears to gate `canseek` on having a
  known total duration independent of what the inputstream addon
  advertises -- confirmed live (`canseek: false` throughout an actively-
  tailing session in this round of testing, where the prior static-
  snapshot version's `canseek: true` came from always presenting a
  finite, `ENDLIST`-terminated duration immediately). Kodi also queries
  `GetCapabilities()` once, at open, and doesn't re-query it mid-session
  -- so even though the *same* session correctly reaches a clean end once
  the recording finishes and `ENDLIST` appears, it doesn't retroactively
  gain seek support for whatever's left of that session; only a fresh
  `Player.Open()` after the recording has actually finished gets normal
  VOD treatment with seek. In short: this version trades seek-while-still-
  recording for not needing to stop and reopen to keep watching new
  content -- the opposite trade-off from the one-time-snapshot version it
  replaced, not a strict improvement on every axis.

  **Revisited once the join-position bug above was fixed, to check whether
  seek could now also be recovered without giving up live-tailing --
  confirmed this is a genuine, inherent architectural trade-off, not
  something left to fix.** Investigation initially chased a promising
  alternative theory: a freshly-created recording's PVR-level `runtime`
  can briefly show a tiny placeholder value (`5` seconds observed) rather
  than its real scheduled duration, self-correcting a while later once
  Dispatcharr's own async EPG-matching settles it (confirmed directly:
  the same recording read `runtime: 5` moments after creation and
  `runtime: 2394` -- matching its real ~40-minute scheduled length -- when
  checked again later). This raised the possibility that every earlier
  `canseek: false` result during live-tailing had been confounded by
  testing against recordings still carrying that placeholder, rather than
  reflecting the live-tailing design itself.
  Ruled out by testing again against a recording confirmed to already
  have its correct, settled PVR-level duration (`runtime: 1252`, sane) at
  the moment of open: `canseek` was still `false`. The placeholder-
  duration behaviour is real (worth fixing or at least being aware of
  separately, since it can misrepresent a recording's length in Kodi's UI
  for a while after creation) but is not what gates seek during live
  playback.
  Traced the real mechanism instead by reading `FFmpegStream`'s handling
  of stream times directly: it only populates start/end time information
  when `!IsRealTimeStream()` (always true here, since `is_realtime_stream`
  is set to `false`), but the end time it reports is
  `m_pFormatContext->duration` -- which stays unknown for as long as
  libavformat's HLS demuxer doesn't know the playlist is finished, i.e.
  for as long as `#EXT-X-ENDLIST` is withheld to keep live-tailing
  working. So `GetCapabilities()` genuinely does advertise
  `INPUTSTREAM_SUPPORTS_SEEK` throughout -- the addon-level capability
  flag was never the blocker -- but Kodi-core, receiving that
  capability alongside an unknown/invalid total duration, correctly
  declines to actually offer seeking: there's no way to seek to a
  percentage or timestamp of a length that isn't known.
  This is a hard architectural conflict, not a bug: an HLS demuxer's
  notion of duration is derived from summing the durations of every
  segment *up to whatever the playlist currently lists as complete*, and
  that concept is fundamentally incompatible with "duration unknown
  because more might still be appended," which is exactly what
  live-tailing depends on. Getting both simultaneously -- seek while a
  recording is still actively being written -- isn't achievable within
  this ffmpeg/libavformat-based approach; the two require contradictory
  answers to "does this stream have a known end."

  **The same root cause also rules out Kodi automatically resuming
  mid-session playback of a still-in-progress recording, confirmed by a
  direct test, not just inferred.** Played an in-progress recording for
  ~25 seconds (of a recording with well over 1000 seconds left on its
  schedule -- nowhere near naturally ending), then stopped it via
  `Player.Stop` -- a genuine mid-playback interruption, not the
  natural-end-of-file case documented earlier in this file.
  `PVR.GetRecordingDetails` afterward showed the exact same outcome as
  that natural-EOF case: `playcount: 1`, `resume: {position: -1.0}` --
  marked fully watched, no bookmark saved at all -- and reopening landed
  back at true position 0, not ~25 seconds in. Kodi's own
  save-a-bookmark-vs-mark-watched decision on stop is a comparison
  against the total duration (how far in, as a fraction of the whole,
  counts as "essentially finished" vs. "still partway through") -- and
  that duration is unknown for exactly the same reason seeking doesn't
  work, so Kodi can't tell a 2%-in stop from a 98%-in one and appears to
  default to treating any stop as complete. Combined with the earlier,
  separately-confirmed finding that there is no Kodi-exposed way to
  write an arbitrary resume point for a `pvr://` path at all
  (`Files.SetFileDetails` fails unconditionally for that scheme), this
  means there is currently no way -- automatic or manual -- to have a
  session resume from where an earlier one left off while the underlying
  recording is still being written. The only two options while still in
  progress are: start over from the true beginning each time (current
  behaviour), or track the position yourself outside Kodi and seek to it
  manually -- which itself doesn't work either, per the seek finding
  above.

  **Since seek and live-tailing are permanently mutually exclusive per
  playback session (not just currently unimplemented together), added an
  explicit choice instead of picking one behaviour for everyone: pressing
  Play on an in-progress recording showed a blocking selection dialog
  ("Play live" vs. "Play from start (seek)") before
  `GetRecordingStreamProperties()` returns, and the answer decided which
  of the two designs above that playback session used.** (Superseded a
  few commits later by the context-menu-based design described further
  down this section, once cancelling this dialog turned out to always
  trigger Kodi's own "Playback failed" report -- kept here as the
  as-shipped history of how the choice was first implemented and verified,
  not the current behaviour.) Confirmed safe
  to call `kodi::gui::dialogs::Select::Show()` -- a synchronous, blocking
  call -- directly from inside that callback: it's invoked directly in
  response to the user pressing Play, the same circumstance Kodi's own
  native resume-point prompt already blocks in.
  "Play live" registers the existing gradual-cap `SetPlaylistProvider()`
  callback unchanged. "Play from start" instead calls a new one-shot
  method, `DispatcharrClient::FetchInProgressRecordingSeekableSnapshot()`
  (shares its actual HTTP fetch-with-401-retry logic with
  `FetchInProgressPlaylistSnapshot()` via a small private helper,
  `FetchRawInProgressPlaylist()`), which calls `RewritePlaylist()` with no
  segment cap and `appendEndlist` forced true unconditionally --
  deliberately skipping the gradual-cap dance the live mode needs
  entirely, since an always-ENDLIST-terminated response is unconditionally
  safe to reveal in one shot (`pls->finished=true` takes hls.c's simple,
  always-start-at-0 path, never reaching the live-edge join computation
  the cap exists to bound) and libavformat never reloads a finished
  playlist anyway, so this mode's provider is in practice only ever
  invoked once per session regardless of how it's implemented. Cancelling
  the dialog (`Select::Show()` returning `-1`) returns `PVR_ERROR_FAILED`
  from `GetRecordingStreamProperties()` -- confirmed live this cleanly
  aborts opening with no player started, rather than falling back to
  either mode silently.
  Verified all three paths live: choosing "Play live" reproduced the
  already-established live-tailing behaviour exactly
  (`maxSegments`/`hasEndlist=0` diagnostic logging, `canseek: false`);
  choosing "Play from start" gave a real known `totaltime` (`2:10`) and
  `canseek: true`, and an actual `Player.Seek` to 1:00 succeeded and
  continued playing correctly afterward; cancelling produced
  `Player.GetActivePlayers: []` -- no player started at all -- confirmed
  via `kodi.log` showing a clean `CVideoPlayer::CloseFile()` rather than
  a hang or crash.
  New localised strings `#30042`-`#30044` (dialog heading, the two option
  labels) in `strings.po`; no new settings.xml entries -- this is a
  per-playback choice, not a persistent preference, and only appears at
  all when `enable_inprogress_playback` is already on.

  **A real user report caught a second, more subtle bug in the gradual-cap
  fix above: "Play live" could still start ~12 seconds into the recording
  instead of at true position 0, specifically on the very first playback
  attempt after the mode-choice dialog was added.** Reproduced precisely
  via the same `av_dump_format` `start:` signal used throughout this
  investigation: `start: 13.427` for "Play live" vs. `start: 1.427` for
  "Play from start" on the same recording, back to back -- an exact
  3-segment (12-second) offset, not a vague "somewhere off." Root cause:
  the gradual-cap fix above grows the cap starting from the very first
  request (`kInitialMaxSegments + kMaxSegmentsGrowthStep` on request two),
  and it turns out even *one* step of growth is sometimes enough for a
  second application of `select_cur_seq_no()`'s live-edge join computation
  -- still occurring within libavformat's settling window, just one reload
  later -- to use the grown 6-segment count instead of the original
  3-segment one, landing `FFMAX(6-3,0)=3` segments (12s) off 0 instead of
  0. Fixed by holding the cap at `kInitialMaxSegments` for several requests
  (`kHoldRequestsAtInitialCap`, 4) before allowing any growth at all --
  `LocalPlaylistServer` now tracks a per-recording request count rather
  than a next-cap value, and a small `ComputeMaxSegments(requestIndex)`
  helper derives the cap from it. Growth only starts once re-application of
  the join computation is no longer occurring in practice, going by the
  margin observed above the single re-application actually caught.
  Re-verified end-to-end after the fix, same methodology: "Play live" and
  "Play from start" opened back to back against the same in-progress
  recording now both report `start: 1.400000` -- identical, not offset --
  confirming the join computation landed on true position 0 for "Play
  live" this time.

  **The mode-choice dialog itself was removed and replaced with a
  context-menu design, which also eliminates the "Playback failed" dialog
  above as a side effect rather than working around it.** Root cause of
  that dialog (confirmed via Kodi-core source, not guessed):
  `CPVRPlaybackState::StartPlayback()` calls `GetRecordingStreamProperties()`
  but never actually checks its `PVR_ERROR` return value -- it only
  inspects whether any stream properties were set. Returning
  `PVR_ERROR_FAILED` on cancel (as the dialog-based design did) was
  therefore indistinguishable, from Kodi-core's point of view, from any
  other kind of failure to produce stream properties: with nothing to
  open, `CVideoPlayer::CloseFile()` sets `m_error = true` (since this
  wasn't a user-initiated stop, `m_bCloseRequest` is false), which fires
  `OnPlayBackError()` and, via `GUI_MSG_PLAYBACK_ERROR`, Kodi's generic
  `HELPERS::ShowOKDialogText` "Playback failed" dialog (strings
  #16026/#16027). There is no cancel-safe value in Kodi's `PVR_ERROR`
  enum, and no separate "user cancelled, don't report an error" signal
  available to a PVR client addon at this call site -- an architectural
  gap in Kodi-core's PVR playback path that can't be worked around from
  inside `GetRecordingStreamProperties()` alone. Fixed at the design level
  instead, per explicit user direction, once presented with the trade-off:
  plain Play on an in-progress recording no longer prompts at all -- it
  goes straight to "Play from start" (the seekable one-shot snapshot,
  matching what the earlier dialog's default-highlighted option already
  was) -- and "Play live" moved to a `PVR_MENUHOOK_RECORDING` context-menu
  entry (`CallRecordingMenuHook()`) instead of a second dialog option.
  With no dialog on the Play path at all, there's nothing left to cancel
  and no way to trigger the "Playback failed" report.

  A binary PVR addon has no API to start playback itself, though (no
  `PlayMedia`/`ExecuteBuiltin`-equivalent exposed to `kodi::addon::CInstancePVRClient`
  -- confirmed by reading through `kodi-dev-kit`'s `AddonToKodiFuncTable_kodi`
  general-purpose function table, which has nothing playback-related), so
  the menu hook can't just open the item live directly the way selecting
  the old dialog's option did. It arms a single pending-recording-id flag
  (`m_pendingLiveModeRecordingId`) instead and shows a
  `kodi::QueueNotification` telling the user to press Play now;
  `GetRecordingStreamProperties()` consumes it (one-shot, whether or not
  it actually matches the id being opened) the next time it's called, and
  falls back to "Play from start" otherwise. Also confirmed via source
  (`PVRContextMenus.cpp`'s `PVRClientMenuHook::IsVisible()`) that
  `PVR_MENUHOOK_RECORDING` has no per-item visibility hook back to the
  addon -- it shows on every recording's context menu indiscriminately,
  completed ones included -- so `CallRecordingMenuHook()` re-checks
  `isInProgress` itself and shows a different, explanatory notification
  (without arming anything) when invoked on a recording that isn't
  actually in progress.

  Verified live end-to-end, including the specific failure this replaced
  a first, broken attempt at: plain `Player.Open` on an in-progress
  recording went straight to `Fullscreen video` with zero dialogs,
  `canseek: true` and a real `totaltime` (confirming "Play from start" by
  default, matching the design). Selecting "Play live" from the context
  menu on that same recording, confirmed present in the menu via GUI
  navigation, then pressing Play again produced `canseek: false`, no
  `totaltime`, and the gradual-cap `hasEndlist=0` diagnostic log line seen
  earlier in this file -- confirming the arm/consume flag actually
  switched modes correctly. Selecting "Play live" on a *completed*
  recording instead queued the explanatory notification and armed nothing,
  confirmed by tracing `id`/`inProgress` through a temporary diagnostic
  log line before removing it. One real bug caught and fixed mid-verification:
  an initial live A/B run through this exact same sequence appeared to show
  the arm silently failing (a second `Player.Open` also came back
  "Play from start"-shaped) -- diagnostic logging on both
  `CallRecordingMenuHook()` and the consuming side in
  `GetRecordingStreamProperties()` showed the ids actually matching
  correctly once added, so the first run's failure is attributed to GUI
  focus landing on a different one of the two identically-titled test
  recordings than intended, not a real logic bug -- flagged here rather
  than asserted with full confidence, since the diagnostic run that would
  have proven that explanation conclusively wasn't repeated.

  Two secondary things noticed along the way, neither investigated
  further this session: the placeholder-duration behaviour described
  above (Dispatcharr-side, not confirmed against its own source, but
  consistent with the API_NOTES entry on this addon's own periodic-
  refresh design existing partly to smooth over exactly this kind of
  post-creation correction); and the real-time-updates WebSocket
  appearing not to reconnect after a Dispatcharr outage-and-recovery
  during this investigation (only one "connected" log line the whole
  session, from well before the outage) -- if confirmed as a real gap in
  the reconnect-on-drop logic (as opposed to, say, the connection
  surviving the outage fine and just not having anything new to report),
  it would mean an addon install stays silently on the periodic-refresh-
  only fallback until restarted, worth a dedicated look later.

  **The macOS-vs-Windows seek discrepancy investigated earlier is probably
  not a real platform difference -- more likely the same class of
  duration-metadata issue described next, not yet re-tested under that
  hypothesis.** `canseek` on Windows tracked whether Kodi had a sane,
  already-known recording duration: `true` against a long-established
  recording with a normal scheduled runtime, `false` against a recording
  whose duration Kodi's PVR data showed as an obviously-wrong ~6 seconds
  (see below) even though Dispatcharr's real `end_time` gave a normal
  ~3h duration -- suggesting Kodi-core gates seek permission on having a
  known total duration, independent of whatever the inputstream addon
  advertises. The macOS clean-room test that ruled out caching used a
  *brand-new* recording created via the API specifically for that test --
  exactly the kind of recording most likely to still be carrying a
  placeholder duration at open time (see below). Version mismatch and
  property-plumbing were still correctly ruled out as explanations, but
  "genuine macOS platform gap" was likely the wrong conclusion; worth
  re-testing on macOS against a long-established recording with a
  known-correct duration before trusting that conclusion further.

  **Confirmed via live testing: a natural end-of-file during in-progress
  playback gets marked "watched" with no resume bookmark at all, and
  there is no way to manually correct this through Kodi's exposed API.**
  Reproduced live: started a fresh recording, waited 5 minutes (confirming
  the live-edge-join bug above also applies to a very short recording --
  the join point, `start: 349.86`, landed almost exactly at the 5-minute
  mark, since with only ~5 minutes of content total there's barely any
  "behind the live edge" room to join into), then stopped the recording
  early to let it finalize and let playback run to a genuine end.
  `kodi.log` showed a clean `CVideoPlayer::Process - eof reading from
  demuxer` / `OnPlayBackEnded` (not an error, not a user-initiated stop),
  followed immediately by `CSaveFileState::DoWork - Marking video item
  ... as watched`. `PVR.GetRecordingDetails` afterward confirmed
  `playcount: 1`, `resume: {position: -1.0, total: 0.0}` -- fully watched,
  no bookmark, even though only a few minutes of real content ever
  existed. Reaching a clean EOF, as opposed to a user-initiated
  `Player.Stop`, is what triggers this "fully watched" classification.

  Tried the obvious fix -- `Files.SetFileDetails` to write an explicit
  `resume: {position, total}` directly -- and it fails unconditionally for
  any `pvr://` path, confirmed architecturally, not just by trial and
  error: `FileOperations.cpp`'s `SetFileDetails()` gates on
  `CFileUtils::Exists(file)` before doing anything else, which calls
  through to `CFile::Exists()` -- and `xbmc/filesystem/FileFactory.cpp`
  explicitly returns `nullptr` for the `pvr://` protocol
  (`else if (url.IsProtocol("pvr")) return nullptr;`), meaning Kodi's
  generic VFS layer has no file handler for PVR paths at all. Verified
  this is the actual cause (not a malformed request) by testing
  progressively simpler calls -- even `{file, media}` alone, and even
  against a deliberately fake path, produced the identical
  `-32602 Invalid params` -- and by checking Kodi's own JSON-RPC error
  codes confirm permission failures (`BadPermission`) are a distinct code
  from this, ruling out a permission-tier explanation instead.

  Net conclusion: there is currently no Kodi-exposed way to directly edit
  a PVR recording's resume point to an arbitrary value. The only way to
  get an accurate bookmark is to stop playback yourself (via
  `Player.Stop`, or the normal "stop" remote/GUI action) *before* it
  reaches a genuine end-of-file -- Kodi's ordinary mid-playback stop
  bookmark-save behaviour does still work for PVR paths (that's the same
  mechanism the normal "resume from where you left off" prompt already
  relies on); it's only the JSON-RPC *write* path that's blocked for
  `pvr://`. This has a real, if awkward, workaround for the in-progress-
  playback UX problem described above: whatever eventually opens/manages
  this playback should stop the player a little before it would naturally
  hit the end of the current snapshot, rather than letting it run out on
  its own.

  **Separately-noticed, likely pre-existing bug: a recording's Kodi-
  visible duration can be stuck far too low (6 seconds observed against a
  real ~3-hour scheduled game) even though Dispatcharr's own `start_time`/
  `end_time` for that same recording are correct.** `TimeFromIso()`
  parsing was checked directly against the real API response and is
  correct (handles both the `Z` and `+00:00` suffix styles fine). Most
  likely explanation: Dispatcharr sets a short placeholder `end_time` at
  the moment a recording is created (e.g. for a manually-triggered "record
  now" without a fixed stop time) and extends it shortly after via EPG
  matching, and this addon's periodic refresh thread (`recording_refresh_
  minutes`, default 5) or the real-time-updates feed hasn't caught the
  correction yet by the time it's checked. Not confirmed against
  Dispatcharr's own source, and not chased further this session -- flagged
  here since it may explain some of the seek-capability inconsistency
  above, not because it's confirmed as a real ongoing bug.
  Gap found by a companion session's real multi-install testing: unlike
  `OpenRecordingStream()`/`ReadRecordingStream()`, there's no way to
  self-heal a stale API key *after the fact* here -- the URL (with the
  key baked in) is handed to a separate addon process once, with no
  401-retry hook the way this addon's own HTTP client has. Fixed with a
  proactive check instead of a reactive one:
  `GetInProgressRecordingStreamUrl()` does a cheap live probe (a tiny
  ranged GET, mirroring `OpenRecordingStream()`'s own probe) against the
  exact URL it's about to build, and regenerates the key first if that
  comes back 401, before ever baking it into the URL ffmpegdirect will
  use standalone. `PVRDispatcharr` persists the regenerated key the same
  way it does for the other two paths. Adds one extra request before
  in-progress playback starts; accepted as a fair trade for closing a gap
  that's already been hit repeatedly in real testing across two
  installs sharing one account.
- **A recording/timer deleted (or otherwise changed) with no local Kodi
  action to react to it kept showing in Kodi indefinitely -- a "phantom"
  recording only a full Kodi restart would clear.** Root cause: every
  `TriggerRecordingUpdate()`/`TriggerTimerUpdate()` call in this addon is
  reactive, firing only right after this addon's own `AddTimer()`/
  `DeleteTimer()`/etc. -- there was no periodic check independent of local
  activity, unlike the lazy staleness check channels/EPG already have.
  Anything that changed the recordings list another way (a different Kodi
  install sharing the account, a direct Dispatcharr API call, a recording
  finishing on its own) had nothing to prompt Kodi to notice. Fixed with a
  background thread (started in the constructor, cleanly joined in the
  destructor) that calls both triggers every `recording_refresh_minutes`
  (default 5, configurable) regardless of local activity. Verified
  end-to-end: created a recording directly via Dispatcharr's API (not
  through this addon, matching how the phantom was actually produced
  during testing), confirmed it appeared in Kodi, deleted it directly via
  the API again, confirmed it was still showing immediately afterward
  (reproducing the bug), then confirmed it disappeared on its own after
  the refresh interval elapsed with no Kodi restart.
- **Real-time recording/timer updates (`enable_realtime_updates` setting,
  off by default, experimental) -- no Dispatcharr plugin needed at all.**
  The periodic refresh above is still a poll; asked to look at genuine
  push instead, confirmed by reading Dispatcharr's own source
  (`dispatcharr/consumers.py`, `dispatcharr/asgi.py`,
  `dispatcharr/jwt_ws_auth.py`, `core/utils.py`) that its backend already
  runs a real Django Channels WebSocket server at `ws(s)://host:port/ws/`,
  authenticated by the *exact same JWT access token* this addon already
  obtains via `/api/accounts/token/` (passed as a `?token=` query
  parameter -- confirmed the token is only checked once, at connect time,
  by `JWTAuthMiddleware`, so an already-open connection keeps working past
  the token's own 30-minute expiry). This is the same channel Dispatcharr's
  own frontend uses, not something added for this addon -- no plugin, no
  server-side change, nothing to install or enable on the Dispatcharr side.
  Confirmed (by reading `apps/channels/tasks.py`/`api_views.py`) that every
  recording lifecycle event this addon cares about is already broadcast on
  it, wrapped as `{"type": "update", "data": {..., "type": "<event>",
  ...}}`: `recording_started`, `recording_ended`, `recording_stopped`,
  `recording_extended`, `recording_updated`, `recording_cancelled`,
  `recordings_refreshed`.
  Implemented as a hand-rolled minimal RFC 6455 client
  (`src/WebSocketClient.h`/`.cpp`) rather than using libcurl's own native
  WebSocket support (`CURLOPT_WS_OPTIONS`/`curl_ws_recv()`), which needs
  curl >= 7.86 (added October 2022) -- the prebuilt Windows curl this addon
  links against (see `docs/BUILDING.md`) is 7.67.0, and Linux/macOS builds
  link whatever system libcurl happens to be installed, not guaranteed to
  have it either. Built instead on `CURLOPT_CONNECT_ONLY`, a much older,
  stable curl feature that hands over a connected (and, for `wss://`,
  already TLS-terminated) socket and lets the caller speak whatever
  protocol it wants over `curl_easy_send()`/`curl_easy_recv()` -- works on
  any curl new enough to build this addon at all. Implements just enough
  of RFC 6455 to open a connection, receive text frames (with simple
  fragmented-message reassembly), and answer ping frames; no
  permessage-deflate, no client-initiated fragmentation, since Dispatcharr
  needs neither for these small JSON payloads. On Windows this needed an
  explicit `ws2_32` link (`CMakeLists.txt`) -- curl handles its own Winsock
  linkage internally but doesn't propagate it to a consumer that also
  calls raw Winsock functions (`select()`) itself.
  Runs alongside, not instead of, the periodic-poll thread above: if the
  WebSocket can't connect or a connection drops and stays down (reconnects
  with exponential backoff, capped at 60s), the poll still gets there
  eventually. Verified end-to-end against the live server: created a
  recording directly via Dispatcharr's API (bypassing this addon
  entirely) and saw the `recording_updated` push arrive **less than one
  second** later, with the recording already visible in Kodi by the next
  check; deleted it the same way and saw `recording_cancelled` arrive
  **1 millisecond** after the delete call. The connection also correctly
  reacted to unrelated events from other concurrent activity on the same
  shared Dispatcharr account during testing (a `recording_started`/
  `recording_ended` pair neither created nor expected), confirming it
  reflects real account-wide activity, not just this install's own
  actions -- exactly the cross-install gap the periodic refresh above was
  built to narrow, now closed to sub-second latency when this is enabled.
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

## Known Kodi-core quirks (confirmed not this addon's bug)

- **A completed recording's duration/"runtime" can be permanently wrong in
  Kodi's recordings list and info panel -- stuck at some small value from
  early in the recording, even though this addon reports the correct
  final duration on every single call.** Confirmed this addon is not the
  cause: added a temporary diagnostic log directly in
  `PVRDispatcharr::GetRecordings()` and confirmed, in the same Kodi
  session that displayed the wrong value, that this addon computed and
  handed Kodi the exact correct duration (matching Dispatcharr's live API
  exactly) on every call -- Kodi's own JSON-RPC `runtime` property still
  reported the old, wrong number regardless.
  Root cause, confirmed by reading Kodi's own source
  (`xbmc/video/VideoInfoTag.cpp`): `CVideoInfoTag::GetDuration()` --
  inherited by `CPVRRecording`, since a PVR recording *is* a video
  library item -- doesn't just return the duration this addon sets via
  `SetDuration()`. It prefers a separately-tracked, *stream-probed*
  duration (`m_streamDetails.GetVideoDuration()`, populated by Kodi's own
  player/library code when it actually opens and reads the file's real
  media streams) unless that probed value is under 60% of the tag's own
  duration. If Kodi probed this recording's file early -- while it was
  still being written, so only a couple of minutes existed on disk -- and
  persisted that small probed duration, it can keep winning over this
  addon's correct value indefinitely.
  This explains why a full Kodi restart doesn't fix it, unlike a plain
  "phantom recording" (see below): recordings themselves are **not**
  persisted locally (confirmed: Kodi's own PVR database, `TV46.db`, has
  no `recordings` table at all -- the in-memory list is rebuilt fresh
  from this addon every session), but the *stream-probed* duration lives
  in Kodi's separate, persistent video library database, keyed by the
  recording's synthetic file path, which survives every restart.
  **Confirmed (not just plausible) to be the exact same root cause as a
  related symptom**: Kodi's Home screen "Recent recordings" widget shows
  full media flags (resolution/codec/audio format, e.g. `H.264` `1080 HD`
  `DOLBY` `5.1`) for the affected recording but only bare runtime for an
  unaffected one -- reproduced side by side on the same install, same
  widget, hovering each item in turn. Both symptoms come from the same
  `m_streamDetails` field: populated, the media flags show *and* the
  small probed duration can win; empty, neither happens.
  Narrowed down further which playback path actually populates it: the
  one affected recording here was specifically played through this
  addon's **in-progress playback feature**
  (`enable_inprogress_playback`, routed through the separate
  `inputstream.ffmpegdirect` addon) while it was still being written; the
  unaffected one was only ever played through this addon's native
  completed-recording path (`OpenRecordedStream()`/`CInputStreamPVRRecording`).
  That's a real, mechanistic difference, not a coincidence: it means
  **using the in-progress playback feature to check in on a recording
  early has a lasting side effect on how Kodi displays it later, once
  it's completed** -- a permanently wrong duration in the library view,
  even though the recording itself and its actual playback are both
  completely fine. Not fixable from this addon's side (see above), but
  worth knowing before using that feature on a recording you also plan
  to watch normally once it's done.
  Not something this addon can fix or work around: there's no PVR client
  API to tell Kodi "forget the stream details/library metadata you
  cached for this file," and this addon doesn't (and shouldn't) touch
  Kodi's video library database directly. Actual playback is unaffected
  either way -- confirmed separately that the real file plays with its
  correct, full-length duration once actually opened; this is purely a
  library/metadata display quirk for an item Kodi has cached stream
  details for.

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
  Originally confirmed with no fix at all: a recording created via one
  Kodi's guide didn't appear on a second, independently-running Kodi until
  that second instance was restarted. Substantially mitigated now (see the
  periodic recordings/timers refresh below) -- every install triggers on
  its own schedule regardless of the others' activity, so the wait is
  bounded by `recording_refresh_minutes` instead of "until next restart."
  Not eliminated: still a poll, not a push, so there's still up to that
  many minutes of lag, and two installs mid-refresh-cycle can briefly
  disagree.
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
