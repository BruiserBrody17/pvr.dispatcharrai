"""
Recording EDL -- a Dispatcharr plugin.

Exposes a completed recording's comskip-generated .edl file (commercial
break markers) over Dispatcharr's plugin run/ API, so a client with no
filesystem access to /data/recordings (this plugin was designed alongside
pvr.dispatcharrai, a Kodi PVR addon) can still fetch the data and show
skip/highlight markers on Kodi's own seekbar.

Why this exists as its own plugin rather than reading the file directly
over HTTP: confirmed against Dispatcharr's own source
(apps/channels/api_views.py's RecordingViewSet) that the only file-serving
action, `file`, always serves exactly custom_properties["file_path"] --
no way to redirect it at a sibling file -- and there's no generic static
route reaching /data/recordings/... either (dispatcharr/urls.py only
exposes Django's own MEDIA_ROOT, an unrelated path inside the app tree,
confirmed via dispatcharr/settings.py). Plugins can't register their own
URL routes either (confirmed via apps/plugins/loader.py -- no
route-registration hook exists), so the plugin run/ API -- already used
by this project's other companion plugin, timeshift_buffer -- is the only
reachable mechanism. This plugin runs inside Dispatcharr's own process,
so it can read the file straight off the same disk Dispatcharr itself
wrote it to, no HTTP round-trip to the recording's storage needed.

Kept as its own small plugin rather than folded into timeshift_buffer:
unrelated concern (comskip/recordings vs. live-channel buffering), no
shared state, no background processes or extra port to expose -- a
single stateless read-a-file-and-parse-it action, installable
independently of whether someone wants server-side live timeshift at
all.

comskip's .edl format (confirmed against docker/comskip.ini's own
[Output] section and apps/channels/tasks.py's own parsing of it): plain
text, one entry per line, whitespace-separated
`<start_seconds> <end_seconds> <type>`, times as fractional seconds.
Dispatcharr's own comskip.ini sets edl_skip_field=3, so every line's type
is already Kodi's own PVR_EDL_TYPE_COMBREAK (3) -- passed through as
whatever the file actually says rather than hardcoded, in case a
differently-configured instance ever writes something else.

Only ever populated for a completed recording in comskip "mark" mode
(custom_properties.comskip.mode == "mark"): in the default "cut" mode,
Dispatcharr physically removes the commercials via ffmpeg and deletes the
.edl file once done (confirmed in tasks.py's comskip_process_recording),
so there's nothing left to fetch by the time a recording shows
status: completed. get_edl returns an empty entries list rather than an
error in that case (and for any recording comskip never touched at all)
-- "no markers" is the normal, expected outcome for most recordings, not
a failure.
"""

from pathlib import Path


def _parse_edl(text: str):
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            start_sec = float(parts[0])
            end_sec = float(parts[1])
            # Missing type column defaults to 3 (Kodi commercial-break
            # marker) -- matches what Dispatcharr's own comskip.ini always
            # writes anyway (edl_skip_field=3), just tolerating a file that
            # for some reason only has two columns.
            edl_type = int(float(parts[2])) if len(parts) >= 3 else 3
        except ValueError:
            continue
        entries.append(
            {
                "start": int(round(start_sec * 1000)),
                "end": int(round(end_sec * 1000)),
                "type": edl_type,
            }
        )
    return entries


class Plugin:
    name = "Recording EDL"
    version = "0.1.0"
    description = (
        "Exposes a completed recording's comskip .edl (commercial break "
        "markers) over the plugin run/ API, for clients with no direct "
        "filesystem access to /data/recordings."
    )
    author = "BruiserBrody17"
    help_url = "https://github.com/BruiserBrody17/pvr.dispatcharrai/tree/master/dispatcharr-plugin/recording_edl"

    # See timeshift_buffer/plugin.py's own comment on this same pattern:
    # plugin.json's fields/actions are only read for the not-yet-trusted
    # preview; once trusted/loaded, this class is what's actually
    # introspected, so this is the real source of truth.
    fields = [
        {
            "id": "about",
            "label": "About",
            "type": "info",
            "description": (
                "Called by a client (e.g. pvr.dispatcharrai's recording "
                "playback) via the plugin run/ API, not usually by hand. "
                "No settings needed -- nothing to configure here."
            ),
        },
    ]

    actions = [
        {
            "id": "get_edl",
            "label": "Get Recording EDL",
            "description": "Returns the comskip EDL entries for a recording. Params: recording_id (required).",
        },
    ]

    def run(self, action: str, params: dict, context: dict):
        logger = context.get("logger")

        if action != "get_edl":
            return {"status": "error", "message": f"Unknown action: {action}"}

        recording_id = params.get("recording_id")
        if not recording_id:
            return {"status": "error", "message": "recording_id is required"}

        from apps.channels.models import Recording

        try:
            recording = Recording.objects.get(pk=recording_id)
        except Recording.DoesNotExist:
            return {"status": "error", "message": f"No recording with id {recording_id}"}
        except (ValueError, TypeError):
            return {"status": "error", "message": f"Invalid recording_id: {recording_id!r}"}

        cp = recording.custom_properties or {}
        comskip = cp.get("comskip") or {}
        edl_filename = comskip.get("edl")
        file_path = cp.get("file_path")

        if not edl_filename or not file_path:
            # Nothing to fetch -- comskip hasn't run, found nothing, or ran
            # in "cut" mode and already deleted the .edl file (see module
            # docstring). Not an error: just no markers for this recording.
            return {"status": "ok", "entries": []}

        edl_path = Path(file_path).parent / edl_filename
        try:
            text = edl_path.read_text(errors="replace")
        except OSError as exc:
            if logger:
                logger.warning("recording_edl: could not read %s: %s", edl_path, exc)
            return {"status": "ok", "entries": []}

        return {"status": "ok", "entries": _parse_edl(text)}
