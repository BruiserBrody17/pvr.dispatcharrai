# Recording EDL (Dispatcharr plugin)

Exposes a completed recording's comskip-generated `.edl` file (commercial
break markers) over Dispatcharr's plugin `run/` API, so `pvr.dispatcharrai`
can show skip/highlight markers on Kodi's own seekbar for a recording that
was processed with Dispatcharr's built-in comskip integration.

## Why this needs a plugin at all

Confirmed against Dispatcharr's own source, not assumed: there is no
HTTP-reachable way to fetch a recording's `.edl` file otherwise.
`RecordingViewSet`'s `file` action (`apps/channels/api_views.py`) always
serves exactly `custom_properties["file_path"]` -- there's no way to point
it at a sibling file -- and there's no generic static route reaching
`/data/recordings/...` either (`dispatcharr/urls.py` only exposes Django's
own `MEDIA_ROOT`, an unrelated path inside the app tree). Plugins can't
register their own URL routes either (confirmed via
`apps/plugins/loader.py` -- no route-registration hook exists), so the
plugin `run/` API -- the same mechanism `pvr.dispatcharrai`'s other
companion plugin, `timeshift_buffer`, already uses -- is the only
reachable option. Running inside Dispatcharr's own process, this plugin
can read the file straight off the same disk Dispatcharr wrote it to.

## How it works

One action, `get_edl` (`{"recording_id": <id>}`):

1. Looks up the `Recording` row via Django's ORM (`apps.channels.models`)
   and reads `custom_properties.comskip.edl` (the `.edl` filename) and
   `custom_properties.file_path` (used only to find the containing
   directory -- the `.edl` file lives alongside the recording, confirmed
   against a real recording's own `custom_properties`).
2. If either is missing -- comskip never ran, found nothing, or ran in
   the default "cut" mode (which physically removes commercials via
   ffmpeg and deletes the `.edl` file once done, confirmed in
   `apps/channels/tasks.py`'s `comskip_process_recording`) -- returns
   `{"status": "ok", "entries": []}`. Not an error: most recordings
   legitimately have nothing to report.
3. Otherwise reads and parses the file. comskip's EDL format (confirmed
   against `docker/comskip.ini`'s own `[Output]` section, which sets
   `edl_skip_field=3`, and against `tasks.py`'s own parsing of the same
   file): plain text, one entry per line, whitespace-separated
   `<start_seconds> <end_seconds> <type>`. Returned as
   `{"status": "ok", "entries": [{"start": <ms>, "end": <ms>, "type": <int>}, ...]}`
   -- times converted to milliseconds, `type` passed through as whatever
   the file actually says (Dispatcharr's own config always writes `3`,
   Kodi's own commercial-break marker type, but this doesn't assume that).

No settings, no background process, no extra port -- a single stateless
read-a-file-and-parse-it action. Kept separate from `timeshift_buffer`
rather than added to it: unrelated concern (comskip/recordings vs.
live-channel buffering), installable independently of whether you want
server-side live timeshift at all.

## Orphaned `.edl`/`.logo.txt` cleanup

A confirmed Dispatcharr-core bug: deleting a recording
(`RecordingViewSet.destroy()` in `apps/channels/api_views.py`) only
removes `custom_properties["file_path"]` and, if present, an in-progress
HLS staging directory -- it never removes the comskip "mark"-mode `.edl`
file, nor the `.logo.txt` file comskip also writes alongside it. Both are
left behind under `/data/recordings` forever, and since `destroy()`'s own
empty-folder pruning runs once at delete time (before removing them would
even be possible), the show/season folder is left behind too.

The `scrub_orphaned_sidecars` action finds and removes exactly those two
sidecar types, but only when no other file in the same directory shares
their base name -- i.e. only when the recording they belonged to is
actually gone. After removing sidecars, it also sweeps for any directory
left empty -- not just ones the sidecar removal itself happened to empty
out, but any directory that was already empty going in (e.g. a
show/season folder some earlier, unrelated deletion left behind).

**Scan roots are derived strictly from your DVR Settings, never the bare
`/data/recordings` root.** Each of DVR Settings' four path templates --
*TV Path Template*, *TV Fallback Template*, *Movie Path Template*, *Movie
Fallback Template* -- is inspected, and the scan root for each is
everything up to its first `{placeholder}` (e.g. the stock
`TV_Shows/{show}/...` template gives a root of
`/data/recordings/TV_Shows`; an absolute
`/mnt/media/TV/{show}/...` template gives `/mnt/media/TV`, since
`tasks.py`'s `_build_output_paths()` uses an absolute template as-is
instead of joining it under `/data/recordings` -- confirmed supported by
Dispatcharr's own comment there, "so users can structure their library
under /data as desired"). **This action never scans `/data/recordings`
itself, and never descends into or removes a dot-prefixed directory
(`.dvr_*_hls` staging directories, and apparently others such as
`.timeshift`) at any depth, regardless of what a template resolves to.**

This is a hard-won fix, not a design preference. An earlier version of
this action always included the bare `/data/recordings` root in its scan,
reasoning that it was harmless since Dispatcharr's own code also treats
it as the recordings root. In practice this made the empty-directory
sweep walk into, and remove once it found them empty, Dispatcharr's own
unrelated top-level entries living there -- a `.dvr_*_hls` staging
directory and a `.timeshift` directory, on a real Dispatcharr/Unraid
install, requiring a Dispatcharr restart to recreate `.timeshift`. Two
independent fixes now guard against a repeat: the scan roots are computed
so they can never equal `/data/recordings` even for a degenerate template
with no subdirectory of its own, and every directory walk separately
refuses to touch anything dot-prefixed no matter which root it started
from.

Confirmed safe against `tasks.py`'s own `comskip_process_recording`:
comskip never runs until `file_path` already exists, and "cut" mode's own
replacement of `file_path` with the commercial-free output is an atomic
`os.replace` -- there's no window where `file_path` is transiently absent
while the `.edl` still exists. So an orphaned sidecar unambiguously means
the recording was deleted, never "still processing."

Uses Dispatcharr's own `confirm` dialog convention (same as
`timeshift_buffer`'s destructive actions) since this permanently deletes
files -- clicking the button in the UI prompts before running.

## `.dvr_*_hls` staging directories -- diagnostics only, no cleanup (yet)

Deliberately out of scope for `scrub_orphaned_sidecars`, and much riskier
than a stray `.edl`/`.logo.txt` sidecar: `.dvr_<recording_id>_hls` is the
per-recording HLS segment staging directory `tasks.py`'s
`_build_output_paths()` creates alongside a recording's video while it's
being written and converted. Under normal operation Dispatcharr already
manages its own lifecycle correctly:

- On a successful HLS-to-MKV concat, Dispatcharr waits out any active HLS
  viewer's heartbeat window, then removes the directory itself and clears
  `custom_properties["_hls_dir"]`.
- On a **failed** concat (direct AND its MP4-intermediate fallback both
  fail), Dispatcharr deliberately **keeps** the directory, logging
  *"Keeping HLS segments for recovery"* -- it's the only surviving copy
  of that recording's actual video.
- Deleting a Recording (`RecordingViewSet.destroy()`) does correctly
  remove its `_hls_dir` too, in a background thread. But that thread is a
  bare `daemon=True` Python thread with no persistence or retry -- if
  Dispatcharr restarts or crashes between the DB row being deleted and
  that thread finishing, the directory is orphaned permanently, with no
  Recording row left to ever reference it again.

So a `.dvr_*_hls` directory found lingering could be: an active
in-progress recording, deliberately-preserved failed-merge recovery data,
or a genuine orphan -- and the first two must never be touched. The
`list_dvr_hls_staging_dirs` action is a **read-only** diagnostic: it finds
every `.dvr_*_hls` directory under `/data/recordings` (recursively, not
limited to the DVR path template locations `scrub_orphaned_sidecars` is
scoped to, since a staging directory could in principle turn up anywhere)
and reports, per directory, a best-effort classification:

| classification | meaning |
| --- | --- |
| `active` | A Recording row with `status: recording` still points its `_hls_dir` here. Never touch. |
| `preserved_failure` | A Recording row with `remux_success: false` still points its `_hls_dir` here -- the only surviving copy. Never touch. |
| `referenced` | A Recording row exists but the signal above isn't confirmed (different status, or its `_hls_dir` doesn't match this path). Needs manual review. |
| `orphaned` | No Recording row with that id exists at all -- the only category consistent with the directory being a genuine leftover. |

It never deletes anything. Results show up in the result toast (a short
summary plus a per-directory breakdown) and in the full `directories`
list for a caller using the REST API directly.

Once you've reviewed the output, `delete_orphaned_dvr_hls_dirs` removes
only the directories classified `orphaned` -- nothing else, empty or not.
An empty directory isn't, on its own, evidence of being safe to delete:
a brand-new recording's `.dvr_*_hls` directory is created before ffmpeg
writes its first segment, so a genuinely active recording can look empty
for its first few seconds -- which is exactly why this only ever acts on
the `orphaned` classification (no Recording row referencing it at all),
never on directory size. Uses the same `confirm` dialog convention as
`scrub_orphaned_sidecars`. Does not prune parent directories left empty
by a deletion -- run `scrub_orphaned_sidecars` for that, which already
covers any parent within a DVR path template's own scope.

## Installing

1. Copy this directory to `data/plugins/recording_edl/` on the host (or
   `/app/data/plugins/recording_edl/` inside the container) -- same
   mechanism as `timeshift_buffer`, see that plugin's own README for the
   zip-import alternative and the folder-name-must-match-exactly gotcha.
2. In Dispatcharr's UI, open the Plugins page, click refresh, enable
   "Recording EDL" (accept the trust-warning modal).
3. **The account calling `get_edl` must be a Dispatcharr admin account**
   (`user_level >= 10`) -- same requirement as `timeshift_buffer`, and
   already satisfied if you're using that plugin too, since
   `pvr.dispatcharrai` uses one configured account for both.

Nothing else to configure -- no storage path, no port to map, no
attribution headers.

## Testing manually

Use the "Get Recording EDL" button on the Plugins page (after pasting a
recording id into the "Test recording ID" setting), or call it directly:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "get_edl", "params": {"recording_id": 112}}'
```

Use "Scrub Orphaned EDL/Logo Files" on the Plugins page (prompts for
confirmation first), or:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "scrub_orphaned_sidecars", "params": {}}'
```

Use "List DVR HLS Staging Directories" on the Plugins page (read-only, no
confirmation needed), or:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "list_dvr_hls_staging_dirs", "params": {}}'
```

Use "Delete Orphaned DVR HLS Directories" on the Plugins page (prompts
for confirmation first), or:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "delete_orphaned_dvr_hls_dirs", "params": {}}'
```
