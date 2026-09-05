# Recording EDL (Dispatcharr plugin)

Exposes a completed recording's comskip-generated `.edl` file (commercial
break markers) over Dispatcharr's plugin `run/` API, so `pvr.dispatcharrai`
can show skip/highlight markers on Kodi's own seekbar. Also provides
cleanup for two Dispatcharr-core issues it never handles on its own: `.edl`/
`.logo.txt` sidecar files and `.dvr_*_hls` staging directories left behind
by a deleted or failed recording.

Root causes, verification, and the incidents that shaped the safety
scoping below are in [docs/RECORDING_EDL.md](../../docs/RECORDING_EDL.md)
in the main repo.

## Installing

1. Copy this directory to `data/plugins/recording_edl/` on the host (or
   `/app/data/plugins/recording_edl/` inside the container) -- same
   mechanism as `timeshift_buffer`, see that plugin's own README for the
   zip-import alternative and the folder-name-must-match-exactly gotcha.
2. In Dispatcharr's UI, open the Plugins page, click refresh, enable
   "Recording EDL" (accept the trust-warning modal).
3. The account calling these actions must be a Dispatcharr **admin**
   account (`user_level >= 10`) -- same requirement as `timeshift_buffer`.

Nothing else to configure -- no storage path, no port to map.

## Actions

| Action | What it does | Destructive? |
|---|---|---|
| `get_edl` | Returns a recording's comskip EDL entries (`{"recording_id": <id>}`). Empty list, not an error, if comskip never ran or ran in "cut" mode. | No |
| `scrub_orphaned_sidecars` | Removes `.edl`/`.logo.txt` files whose recording is gone, then prunes any directory left empty (including ones already empty going in). Scoped to your DVR Settings' path templates; never touches the bare `/data/recordings` root or anything dot-prefixed. | **Yes** -- confirm dialog |
| `list_dvr_hls_staging_dirs` | Classifies every `.dvr_*_hls` directory found as `active`, `preserved_failure`, `referenced`, or `orphaned` (see table below). Reports only, changes nothing. | No |
| `delete_orphaned_dvr_hls_dirs` | Deletes only directories `list_dvr_hls_staging_dirs` classified `orphaned` -- never based on whether a directory is empty. | **Yes** -- confirm dialog |

`.dvr_*_hls` classifications:

| classification | meaning |
| --- | --- |
| `active` | A Recording row with `status: recording` still points here. Never touched. |
| `preserved_failure` | Concat failed; Dispatcharr kept this as the only surviving copy. Never touched. |
| `referenced` | A Recording row exists but the signal above isn't confirmed. Needs manual review. |
| `orphaned` | No Recording row with that id exists at all -- the only category `delete_orphaned_dvr_hls_dirs` acts on. |

## Testing manually

Use the matching button on the Plugins page (destructive actions prompt
for confirmation first; `get_edl` needs a `recording_id` -- paste one
into the "Test recording ID" setting first), or call directly:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "get_edl", "params": {"recording_id": 112}}'
```

Swap `"action"` for `scrub_orphaned_sidecars`, `list_dvr_hls_staging_dirs`,
or `delete_orphaned_dvr_hls_dirs` (all take `"params": {}`) to run the
others the same way.
