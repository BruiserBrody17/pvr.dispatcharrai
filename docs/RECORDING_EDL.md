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

**Like every other companion-plugin action, `get_edl` needs a real
Dispatcharr admin account** -- this isn't specific to this plugin or to
EDL data: Dispatcharr's plugin `run/` API (`PluginRunAPIView`) is
admin-only for every plugin's every action, a blanket restriction
confirmed against Dispatcharr's own source and detailed in
[docs/TIMESHIFT.md](TIMESHIFT.md)'s "permission requirement" section.
Unlike `timeshift_buffer`, a non-admin account here fails soft rather
than hard: `GetRecordingEdl()` treats any `get_edl` failure the same as
"plugin not installed" (see below) -- logged at debug level, empty
entries returned, no playback impact -- so the only visible symptom is
recordings simply never showing commercial-break markers.

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

## Orphaned `.edl`/`.logo.txt` sidecar cleanup

A user pasted a real `/data/recordings` listing showing leftover `.edl`
and `.logo.txt` (comskip-generated) files with no recording left to own
them. Root cause, confirmed against Dispatcharr's own source: deleting a
recording (`RecordingViewSet.destroy()`) only removes
`custom_properties["file_path"]` and, if present, an in-progress HLS
staging directory -- it never removes either comskip sidecar file. Since
`destroy()`'s own empty-folder pruning runs once at delete time, before
removing these sidecars would even be possible, the show/season folder is
left behind too.

Added `scrub_orphaned_sidecars`: finds and removes `.edl`/`.logo.txt`
files with no other file in the same directory sharing their base name
(the only safe, unambiguous signal that the recording they belonged to is
actually gone), then sweeps for any directory left empty -- including
ones that were already empty going in, not just ones this removal
happened to empty out. Confirmed safe against `tasks.py`'s own
`comskip_process_recording`: comskip never runs before `file_path`
exists, and "cut" mode's own replacement of `file_path` is an atomic
`os.replace`, so there's no window where a sidecar could exist while its
recording is merely still being written.

**A real incident during development, not a hypothetical, drove the final
scan-root design.** An early version always included the bare
`/data/recordings` root in its scan on the reasoning that it was
harmless, matching Dispatcharr's own hardcoded `library_root`. Live on a
real install, the empty-directory sweep instead walked into and removed
Dispatcharr's own unrelated top-level entries there -- a `.dvr_*_hls`
staging directory and a `.timeshift` directory, the latter requiring a
Dispatcharr restart to recreate. Fixed with two independent guards, not
one: scan roots are now derived strictly from DVR Settings' four path
templates (never the bare `/data/recordings` root, even for a degenerate
template with no subdirectory of its own), and the directory walk
separately refuses to touch anything dot-prefixed at any depth regardless
of which root it started from -- confirmed live afterward against the
exact same layout that caused the original incident: the genuine orphan
was removed, `.dvr_*_hls` and `.timeshift` both survived.

## `.dvr_*_hls` staging directories: diagnose first, delete only what's provably safe

Deliberately kept out of `scrub_orphaned_sidecars` -- much higher stakes
than a stray sidecar file. `.dvr_<recording_id>_hls` is the per-recording
HLS segment staging directory `tasks.py` creates alongside a recording
while it's being written and converted. Dispatcharr manages its own
lifecycle correctly in the common cases (removes it after a successful
concat, once any active viewer's heartbeat window has passed; deliberately
*keeps* it, logging "Keeping HLS segments for recovery," when a concat
fails and its own MP4-intermediate fallback also fails, since it's then
the only surviving copy of that recording). But `RecordingViewSet.destroy()`'s
own cleanup of a deleted recording's `_hls_dir` runs on a bare
`daemon=True` thread with no persistence or retry -- if Dispatcharr
restarts or crashes in the gap between the DB row being deleted and that
thread finishing, the directory is orphaned permanently with nothing left
to ever reference it again.

Added `list_dvr_hls_staging_dirs` first, as a read-only diagnostic:
classifies every such directory it finds as `active` (a Recording row with
`status: recording` still points here), `preserved_failure` (a Recording
row with `remux_success: false` still points here -- the only surviving
copy), `referenced` (a Recording row exists but neither signal above is
confirmed -- needs manual review), or `orphaned` (no Recording row with
that id exists at all). Only once that was live and reviewed against real
data did `delete_orphaned_dvr_hls_dirs` get added, scoped strictly to the
`orphaned` classification -- deliberately not gated on whether a directory
is empty, since a brand-new recording's own staging directory is created
before ffmpeg writes its first segment, so a genuinely active recording
can legitimately look empty for its first few seconds. Emptiness was
never the safety signal; a real, current Recording row is.
