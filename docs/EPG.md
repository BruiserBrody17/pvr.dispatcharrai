# EPG data

*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

## Confirmed channel JSON fields (`GET /api/channels/channels/`)

The response is a **bare JSON array**, not paginated/wrapped in
`{results: [...]}` (at least on the instance this was checked against) --
`DispatcharrClient::GetChannels()`'s `results`-or-bare-array handling covers
this correctly either way, so no change was needed there.

| Field | Notes |
|---|---|
| `id` | integer, channel's own id |
| `uuid` | string, used in the live-stream URL (confirmed against `/proxy/ts/stream/{channel_uuid}` -- see the endpoint table in [API_NOTES.md](API_NOTES.md)) |
| `name` | string |
| `channel_number` | number (observed as a float, e.g. `10687.0`) |
| `channel_group_id` | **bare integer**, not a nested `channel_group` object |
| `tvg_id` | bare string field directly on the channel, not nested under `epg_data` |
| `logo_id` | integer, FK to a separate Logo object -- **there is no `logo_url` field on Channel** (that field exists on the Stream model instead, which is a different object) |

`DispatcharrClient::GetChannels()` already handles the nested-object variants
defensively as a fallback, but the flat/bare forms above are what a real
instance actually returns.

## Rich EPG data (posters, new/premiere/live badges, cast, genre) from XMLTV

Confirmed live against a real instance after the user added Schedules
Direct as an EPG source in Dispatcharr and mapped it to a channel: the
`/output/epg` XMLTV Dispatcharr generates already carries all of this per
programme when the underlying EPG source provides it -- it isn't
Dispatcharr-specific or Schedules-Direct-specific, just standard XMLTV
elements this addon wasn't reading yet. `XmlTvParser` originally only
extracted title/sub-title/desc/category(×1)/xmltv_ns episode-num;
`GetEPGForChannel()` only set title/plot outline/plot/genre
description/series/episode number. Extended both to close the gap with
what TVHeadend's own `pvr.hts` shows for the same kind of source:

- `<icon src="...">` (a per-**programme** poster, distinct from the
  channel's own logo) → `PVREPGTag::SetIconPath()`. This is what actually
  produces poster art in Kodi's guide, not the channel icon.
- `<new/>` / `<premiere/>` / `<live/>` (empty presence-only elements) →
  `EPG_TAG_FLAG_IS_NEW` / `_PREMIERE` / `_LIVE` via `SetFlags()`.
  `EPG_TAG_FLAG_IS_SERIES` is set heuristically (season/episode number
  present, or any category text contains "Series") since XMLTV has no
  dedicated series-flag element.
- `<category>` (0+, repeatable -- confirmed live with up to 4 on one
  programme, e.g. `["Series", "Sports non-event", "News", "Sports talk"]`)
  → all joined into `GenreDescription`, plus a best-effort keyword scan
  (`MapCategoriesToGenreType()` in `PVRDispatcharr.cpp`) against Kodi's
  ETSI EN 300 468 `EPG_EVENT_CONTENTMASK_*` values so the guide gets
  genre-based colour coding instead of every programme showing as "Other /
  Unknown" -- confirmed live: `["Series", "Sports non-event", ...]` now
  resolves to `Genre: Sports` in Kodi's own EPG info panel, not the
  generic fallback.
- `<credits>` sub-elements (confirmed live: `producer`/`actor` populated
  by this source; `director`/`writer`/`adapter`/`presenter`/`guest`/
  `commentator`/`composer`/`editor` also parsed for robustness even
  though unconfirmed against this particular source) → `director`/
  `writer` get their own fields, everyone else buckets into `Cast`
  (comma-joined, matching `EPG_STRING_TOKEN_SEPARATOR`).
- `<date>` → both `Year` (leading 4 digits) and `FirstAired` (verbatim).
- `<sub-title>` → both `PlotOutline` (unchanged, existing behavior) and
  the new `EpisodeName` (confirmed live: e.g. "Men's & Women's Second
  Round" for a tennis broadcast).

**Not mapped, deliberately:** `<star-rating>` (confirmed absent -- 0
occurrences -- across a live 39MB/~well over 50k-programme fetch from
this source, so nothing to verify against) and parental `<rating>`
(present, but each `system` attribute is a different rating board --
Australian Classification Board was the one seen live -- with no clean,
non-misleading mapping to Kodi's single integer `ParentalRating` without
a per-system lookup table this wasn't worth building for the first pass).
`<previously-shown>` (present, confirmed) is also not mapped to anything
-- its absence is already implied by not setting `EPG_TAG_FLAG_IS_NEW`,
and there's no dedicated Kodi field for "last repeat date" distinct from
`FirstAired`.

**A real gotcha hit during live verification, worth remembering for next
time:** after rebuilding and redeploying the addon, `PVR.GetBroadcasts`
kept returning the old, pre-change field values (empty cast, `"Other /
Unknown"` genre) for several minutes even though the new DLL was
confirmed loaded (`SECTION:LoadDLL` in `kodi.log`, right size on disk) and
`EnsureEpgLoaded()`'s own staleness check should force a fetch on a fresh
addon instance. Root cause: Kodi's *own* EPG database is a separate cache
layer on top of whatever the addon returns, keyed off the channel's
persistent unique id (not the ephemeral per-session `channelid` JSON-RPC
exposes) -- with 7 days of guide already cached from a prior session, its
own `epg.epgupdate` interval (120 min default) meant it didn't feel a need
to re-poll the addon yet. Settings → PVR & Live TV → Guide → **Clear
data** forces an immediate full re-fetch through the addon and is the
reliable way to verify an EPG-mapping change live without waiting out the
interval.

## Background loading

`EnsureChannelsLoaded()`/`EnsureEpgLoaded()` (`PVRDispatcharr.cpp`) used to
be called only lazily, inline, on whichever Kodi-owned thread first asked
after the cache went stale -- meaning a real fetch (channel list +
channel groups, or a full XMLTV guide fetch+parse) could block that
calling thread. Fine on a small install, but confirmed live against a
~9,000-channel instance that this is a genuinely slow operation (~7
seconds) worth moving off Kodi's calling thread.

Fixed with `StartChannelEpgRefreshThread()`, a background thread (started
in the constructor, joined in the destructor -- same pattern as the
recording refresh thread documented in `docs/RECORDINGS.md`) that calls
the same two functions itself, immediately on start and then every 10
minutes (`kChannelEpgRefreshCheckMinutes`, not user-configurable -- it's
just how often the *check* happens, not how often a real fetch happens;
that's still governed by `channel_refresh_hours`/`epg_refresh_hours` as
before). `EnsureChannelsLoaded()`/`EnsureEpgLoaded()` themselves are
unchanged other than now returning `bool` (did this call actually
perform a fetch, vs. find the cache still fresh) -- Kodi's own calling
threads still call them directly and still get a correct answer either
way, so a fetch racing the very first moment after construction (before
the background thread's first pass completes) still works exactly as
before, just no longer the common case. When a background pass finds the
cache stale and actually refreshes it, it also calls
`TriggerChannelUpdate()`/`TriggerChannelGroupsUpdate()`/`TriggerEpgUpdate()`
(once per known channel -- confirmed against kodi-dev-kit's `PVR.h` that
there's no bulk/whole-guide trigger) so Kodi picks up the change promptly
rather than waiting out its own separate `epg.epgupdate` polling interval
mentioned above.

**Confirmed live**, debug logging enabled: `kodi.log` showed the
background thread's own log lines
(`background thread refreshed channels/groups`, then
`background thread refreshed EPG`) on a distinct thread id from both the
addon's construction (`creating PVR client instance`) and the realtime-
update thread, completing a few seconds after startup with no gap in
responsiveness elsewhere in Kodi. Verified the actual data too, not just
that the calls didn't error: `PVR.GetChannels` returned all 8,919
channels and `PVR.GetBroadcasts` against a normal channel (not one of the
placeholder "LIVE EVENT NN - NO EVENT" ones, which have no programming to
return) came back with a full day-plus of real programme data.

