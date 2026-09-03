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

Use the "Get Recording EDL" button on the Plugins page, or call it
directly:

```bash
curl -X POST http://<host>:9191/api/plugins/plugins/recording_edl/run/ \
  -H "Authorization: Bearer <admin JWT>" \
  -H "Content-Type: application/json" \
  -d '{"action": "get_edl", "params": {"recording_id": 112}}'
```
