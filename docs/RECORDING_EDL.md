*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Commercial-break markers (comskip EDL) on the recording seekbar

Requested after a user enabled Dispatcharr's built-in comskip integration
and noticed the resulting `.edl` file next to a recording had no visible
effect in Kodi -- no highlighted commercial-break regions on the seekbar.

Two independent gaps, confirmed against real source rather than assumed,
both needed fixing:

## Gap 1: this addon never implemented Kodi's EDL callback at all

`GetCapabilities()` declared `PVRCapabilities::SetSupportsRecordingEdl(false)`
unconditionally, so Kodi never even asked the addon for commercial-break
data regardless of what existed on disk. Fixed by implementing
`GetRecordingEdl()` and flipping that capability to `true` -- see Gap 2
for why it's safe to declare this unconditionally rather than behind a
setting.

## Gap 2: no HTTP-reachable way to fetch the `.edl` file's content at all

Even with the callback implemented, there was nothing to call: confirmed
against Dispatcharr's own source (not guessed) that `RecordingViewSet`'s
`file` action (`apps/channels/api_views.py`) always serves exactly
`custom_properties["file_path"]`, with no parameter to redirect it at a
sibling file, and there's no generic static route reaching
`/data/recordings/...` either -- `dispatcharr/urls.py` only exposes
Django's own `MEDIA_ROOT`, an unrelated path inside the app tree
(confirmed via `dispatcharr/settings.py`). Plugins can't register their
own URL routes either (confirmed via `apps/plugins/loader.py` -- no
route-registration hook exists), so the only reachable mechanism is the
same one this addon's other companion plugin, `timeshift_buffer`, already
uses: Dispatcharr's generic plugin `run/` API.

Fixed with a new, separate companion plugin,
[`dispatcharr-plugin/recording_edl/`](../dispatcharr-plugin/recording_edl/)
(**not** folded into `timeshift_buffer` -- unrelated concern, no shared
state, no background process or extra port needed, so a second small
plugin was cleaner than growing an already-large one). One action,
`get_edl` (`{"recording_id": <id>}`): looks up the `Recording` row via
Django's own ORM (running inside Dispatcharr's process, so it has real
filesystem access Dispatcharr's REST API doesn't expose), reads
`custom_properties.comskip.edl` (the `.edl` filename, confirmed present
on a real recording once comskip has run) and `custom_properties.file_path`
(used only to find the containing directory -- the `.edl` file lives
alongside the recording), and returns the parsed entries.

**comskip's `.edl` format**, confirmed against Dispatcharr's own
`docker/comskip.ini` (`[Output]` section) and `apps/channels/tasks.py`'s
own parsing of the same file: plain text, one entry per line,
whitespace-separated `<start_seconds> <end_seconds> <type>`. Dispatcharr's
ini sets `edl_skip_field=3`, so every line's type is already Kodi's own
`PVR_EDL_TYPE_COMBREAK` (3) -- the plugin passes the type field through
as-written rather than hardcoding it, but in practice it's always 3.

**Only ever populated in comskip "mark" mode.** Confirmed via
`apps/channels/tasks.py`'s `comskip_process_recording`: the *default*
mode is `"cut"`, which uses the same detected timestamps to physically
remove the commercials via ffmpeg and remux the file in place, then
**deletes the `.edl` file** once done (`os.remove(edl_path)`) --
`custom_properties.comskip` in that mode has no `mode` key at all by the
time a recording shows `status: completed`. Only `"mark"` mode (keep the
full recording, just flag the breaks) leaves the `.edl` file on disk to
be fetched. The plugin doesn't need to special-case this: in cut mode
`custom_properties.comskip.edl` is simply absent, and `get_edl` returns
an empty `entries` list either way -- "no markers" is the correct,
unremarkable outcome for a cut-mode recording (the commercials are
already gone from the file) as well as for the much more common case of
a recording comskip never touched at all.

Also confirmed via the same source: EDL data is never populated for an
**in-progress** recording -- comskip only ever runs against a file after
the recording finishes, triggered right at that point
(`comskip_process_recording.delay(recording_id)`), never mid-recording.
`GetRecordingEdl()` doesn't need its own in-progress check as a result;
a still-recording item's `custom_properties.comskip` simply won't exist
yet, and the plugin's normal "missing -> empty entries" path already
covers it.

**Confirmed live end-to-end**, against a real recording with comskip
already run in mark mode (`custom_properties.comskip: {"edl":
"S08E23.edl", "mode": "mark", "commercials": 3}`):
- Called the plugin's `get_edl` action directly first, in isolation, to
  confirm it before going through the addon: returned exactly 3 entries,
  type 3, with real millisecond timestamps matching the recording's own
  `commercials: 3` count.
- Then through the actual addon: `GetRecordingEdl()` fired on
  `Player.Open`, and `kodi.log` showed Kodi's own `CEdl` subsystem
  processing all three breaks --
  `CEdl::ReadPvr - Added break [00:08:03.700 - 00:11:31.670] found in PVR
  item for: ...` (and the other two, matching times exactly), plus
  automatic scene markers Kodi itself adds at each break boundary. This is
  the actual mechanism that drives the seekbar's highlighted regions, so
  this is as close to a direct confirmation as log output gets short of a
  visual screenshot.
- Playback itself unaffected throughout (`canseek: true`, normal
  progression) -- EDL fetch failure (confirmed separately, with the
  plugin not yet installed) also doesn't affect playback: logged at
  `ADDON_LOG_DEBUG` rather than `ADDON_LOG_ERROR`, since not installing
  this optional plugin is an entirely normal configuration, not a
  problem -- and `GetRecordingEdl()` returns `PVR_ERROR_NO_ERROR` with an
  empty list either way rather than surfacing the failure to Kodi.

## Why `SetSupportsRecordingEdl` is unconditional, not behind a setting

Unlike `enable_catchup_ffmpegdirect_seek` (which genuinely changes
behavior and fails outright if misconfigured), a missing or not-installed
`recording_edl` plugin just means every `GetRecordingEdl()` call comes
back empty -- confirmed live, no playback impact, no error surfaced to
the user, just a debug-level log line. There's no failure mode that
justifies making this an opt-in setting the way the server-side timeshift
and in-progress-recording features once were before they were also made
unconditional (see `docs/TIMESHIFT.md`/`docs/RECORDINGS.md`) -- this one
never needed the "still experimental, might not be ready" caveat those
did in the first place.
