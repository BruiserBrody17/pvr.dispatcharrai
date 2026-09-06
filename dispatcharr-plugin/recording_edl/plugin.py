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

Also exposes scrub_orphaned_sidecars: a confirmed Dispatcharr-core bug is
that deleting a recording (RecordingViewSet.destroy() in
apps/channels/api_views.py) only removes custom_properties["file_path"]
and, if present, an in-progress HLS staging dir -- it never removes the
comskip "mark"-mode .edl file, nor the .logo.txt file comskip also writes
alongside it, both left behind under /data/recordings forever. This also
silently defeats destroy()'s own _prune_empty_parents() cleanup, since the
show/season folder never actually ends up empty. This action finds and
removes exactly those two sidecar types when the recording they belong to
is genuinely gone (no other file in the same directory shares their base
name), then sweeps for any directory left empty -- by that removal or
already empty beforehand. Confirmed safe against tasks.py's own
comskip_process_recording: comskip never runs until file_path already
exists, and "cut" mode's own file_path replacement is an atomic os.replace
(no window where file_path is transiently absent while the .edl still
exists), so an orphaned sidecar unambiguously means the recording was
deleted, not "still processing."

Scoped strictly to where DVR Settings' four path templates (TV Path
Template, TV Fallback Template, Movie Path Template, Movie Fallback
Template) actually resolve to -- never the bare /data/recordings root,
and never a dot-prefixed directory at any depth. This is a hard-won
lesson, not a stylistic choice: an earlier version of this action always
scanned all of /data/recordings, which on 2026-09-05 caused its
empty-directory sweep to walk into (and remove, once found empty)
Dispatcharr's own .dvr_*_hls staging directories and a .timeshift
directory living at that same top level -- neither of which any DVR path
template controls, and neither of which this action has any business
reasoning about. See _dvr_sidecar_scan_roots and _is_under_dotted_dir.
"""

import re
import shutil
from pathlib import Path

# Matches Dispatcharr's own hardcoded library_root/recordings_root
# (tasks.py's _build_output_paths, api_views.py's RecordingViewSet) --
# every DVR path template is joined under this unless the template itself
# is absolute (see _dvr_sidecar_scan_roots below).
_RECORDINGS_ROOT = Path("/data/recordings")

# Ordered longest-suffix-first so ".logo.txt" (a compound extension) is
# checked before anything that would only strip its trailing ".txt".
_SIDECAR_SUFFIXES = (".logo.txt", ".edl")


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


def _sidecar_base_name(filename: str):
    """The recording's own base filename a sidecar was generated from, or
    None if `filename` isn't one of the two comskip sidecar types. Can't
    use Path.stem/splitext for .logo.txt -- that would only strip the
    trailing .txt and leave ".logo" attached, making it look unrelated to
    a same-named .mkv/.ts video that has a plain single-extension stem."""
    for suffix in _SIDECAR_SUFFIXES:
        if filename.endswith(suffix):
            return filename[: -len(suffix)]
    return None


def _is_under_dotted_dir(path: Path, root: Path) -> bool:
    """True if any path component between `root` and `path` starts with
    "." -- Dispatcharr's own internal/working directories (.dvr_*_hls
    staging dirs, and apparently others such as .timeshift) are named
    exactly this way and must never be descended into, read as an
    "empty" candidate, or removed by this action, regardless of what a
    DVR path template resolves to. A second, independent layer of
    protection on top of _dvr_sidecar_scan_roots deliberately no longer
    including the bare /data/recordings root -- belt and suspenders after
    a real incident on 2026-09-05 where a .dvr_*_hls staging directory
    and a .timeshift directory were both wiped by an earlier version of
    this action's empty-directory sweep."""
    return any(part.startswith(".") for part in path.relative_to(root).parts)


def _dvr_sidecar_scan_roots():
    """Directories that a recording (and therefore its .edl/.logo.txt
    sidecars) could actually land in, derived from DVR Settings' four
    path templates -- TV Path Template, TV Fallback Template, Movie Path
    Template, Movie Fallback Template (tv_template, tv_fallback_template,
    movie_template, movie_fallback_template in CoreSettings.get_dvr_settings())
    -- rather than assuming everyone left them at their "TV_Shows/"Movies/"
    defaults. Confirmed in tasks.py's _build_output_paths(): a template is
    joined under the hardcoded /data/recordings library_root UNLESS the
    template itself starts with "/", in which case it's used as-is --
    Dispatcharr's own comment there ("so users can structure their
    library under /data as desired") confirms this is a supported way to
    point a template at a separate library entirely, so a scan limited to
    only /data/recordings would silently miss those recordings' sidecars.

    For each template, the scan root is everything up to its first "{"
    placeholder (trimmed back to the last complete path separator) --
    e.g. "TV_Shows/{show}/S{season:02d}E{episode:02d}.mkv" joined under
    /data/recordings gives a root of /data/recordings/TV_Shows, while an
    absolute "/mnt/media/TV/{show}/..." gives /mnt/media/TV.

    Deliberately does NOT include the bare /data/recordings root itself
    unless a template resolves directly into it (no subdirectory at all
    -- rare). Confirmed live on 2026-09-05: an earlier version of this
    function always added /data/recordings as a root regardless, which
    made the empty-directory sweep walk (and remove, once it found them
    empty) Dispatcharr's own unrelated top-level entries living there --
    .dvr_*_hls staging directories and a .timeshift directory -- neither
    of which any DVR path template controls. This action must only ever
    touch the directories your templates actually point recordings into.
    """
    from core.models import CoreSettings

    roots = set()
    for template in (
        CoreSettings.get_dvr_tv_template(),
        CoreSettings.get_dvr_tv_fallback_template(),
        CoreSettings.get_dvr_movie_template(),
        CoreSettings.get_dvr_movie_fallback_template(),
    ):
        if not template:
            continue
        resolved = template if template.startswith("/") else f"{_RECORDINGS_ROOT}/{template}"
        prefix = resolved.split("{", 1)[0]
        slash_idx = prefix.rfind("/")
        candidate = Path(prefix[:slash_idx]) if slash_idx > 0 else None
        # A template with no subdirectory component at all (e.g. a bare
        # "{show}.mkv") resolves its candidate root to _RECORDINGS_ROOT
        # itself -- explicitly excluded, not just skipped when slash_idx
        # looks empty: that check alone does NOT catch this case (the
        # leading "/data/recordings/" already supplies a slash), and
        # missing this exact check is what let the 2026-09-05 incident
        # happen even after this function was supposedly no longer
        # supposed to add the bare root. A template configured this
        # unusually just isn't covered by this action.
        if candidate is not None and candidate != _RECORDINGS_ROOT:
            roots.add(candidate)

    # Drop any root already covered by another (itself or an ancestor)
    # already kept, so an overlapping template doesn't cause the same
    # directory to be walked, and its now-empty parents reasoned about
    # for pruning, more than once.
    deduped = []
    for root in sorted(roots, key=lambda p: len(p.parts)):
        if not any(root == kept or kept in root.parents for kept in deduped):
            deduped.append(root)
    return deduped


def _prune_empty_directories(root, logger):
    """Removes every directory under `root` (never `root` itself) left
    with zero entries -- not just ones a sidecar removal happened to
    empty out this run, but any directory that was already empty going
    in (e.g. a show/season folder some earlier, unrelated deletion left
    behind). Walked bottom-up (deepest paths first) in one pass: each
    directory's own emptiness is checked live via iterdir() at the point
    the loop reaches it, so a leaf removed earlier in the same pass makes
    its now-empty parent -- reached later, since it has fewer path parts
    -- eligible too, without needing a second pass."""
    removed = []
    errors = []

    if not root.is_dir():
        return removed, errors

    dirs = sorted(
        (p for p in root.rglob("*") if p.is_dir() and not _is_under_dotted_dir(p, root)),
        key=lambda p: len(p.parts),
        reverse=True,
    )
    for d in dirs:
        try:
            next(d.iterdir())
            continue  # not empty
        except StopIteration:
            pass
        except OSError as exc:
            errors.append(f"{d}: {exc}")
            continue

        try:
            d.rmdir()
        except OSError as exc:
            errors.append(f"{d}: {exc}")
            continue

        removed.append(str(d))
        if logger:
            logger.info("recording_edl: removed empty directory %s", d)

    return removed, errors


def _scrub_orphaned_recording_sidecars(logger):
    """Removes .edl/.logo.txt sidecars whose recording is gone, then, per
    scan root, sweeps for any directory left empty -- by that removal or
    already empty beforehand. See the module docstring for why "no other
    file in this directory shares the sidecar's base name" is a safe,
    unambiguous orphan test, and _dvr_sidecar_scan_roots for why this
    scans more than just the hardcoded /data/recordings root."""
    removed_files = []
    removed_dirs = []
    errors = []

    for root in _dvr_sidecar_scan_roots():
        if not root.is_dir():
            continue

        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if _is_under_dotted_dir(path, root):
                continue
            base = _sidecar_base_name(path.name)
            if base is None:
                continue

            try:
                siblings = list(path.parent.iterdir())
            except OSError as exc:
                errors.append(f"{path}: {exc}")
                continue

            has_owning_recording = any(
                other != path and other.is_file() and _sidecar_base_name(other.name) is None
                and other.stem == base
                for other in siblings
            )
            if has_owning_recording:
                continue

            try:
                path.unlink()
            except OSError as exc:
                errors.append(f"{path}: {exc}")
                continue

            removed_files.append(str(path))
            if logger:
                logger.info("recording_edl: removed orphaned sidecar %s", path)

        dirs_removed, dir_errors = _prune_empty_directories(root, logger)
        removed_dirs.extend(dirs_removed)
        errors.extend(dir_errors)

    return removed_files, removed_dirs, errors


_HLS_STAGING_DIR_RE = re.compile(r"^\.dvr_(\d+)_hls$")


def _classify_dvr_hls_dir(hls_dir: Path):
    """Best-effort, read-only classification of a .dvr_<id>_hls staging
    directory -- for reporting only, never used to decide anything
    destructive. Confirmed against tasks.py's run_recording and
    api_views.py's RecordingViewSet.destroy(): on a successful concat,
    Dispatcharr removes the directory itself (after waiting out an active
    HLS viewer's heartbeat window) and clears custom_properties["_hls_dir"];
    on a failed concat (direct AND the MP4-intermediate fallback both
    failed) it deliberately KEEPS the directory, logging "Keeping HLS
    segments for recovery" -- the only surviving copy of that recording's
    video; and deleting a Recording also removes its _hls_dir via a
    background daemon thread with no persistence or retry, so a
    Dispatcharr restart/crash between the DB delete and that thread
    finishing can leave a directory with no owning Recording row at all.

    classification is one of:
      "active"            -- status == "recording"; a recording in
                              progress. Never touch.
      "preserved_failure"  -- concat/remux failed; the only surviving copy
                              of this recording's video. Never touch.
      "referenced"         -- a Recording row still points _hls_dir at
                              this directory, but neither of the above
                              conditions is confirmed. Needs manual review.
      "orphaned"            -- no Recording row with this id exists at
                              all. The only category where a genuinely
                              deleted recording's leftover staging
                              directory is expected to land.
    """
    match = _HLS_STAGING_DIR_RE.match(hls_dir.name)
    if not match:
        return None
    recording_id = int(match.group(1))

    from apps.channels.models import Recording

    recording = Recording.objects.filter(id=recording_id).first()

    try:
        segment_count = sum(1 for f in hls_dir.iterdir() if f.is_file() and f.suffix == ".ts")
    except OSError:
        segment_count = None

    if recording is None:
        classification = "orphaned"
        detail = f"No Recording row with id {recording_id} -- its owning recording was deleted"
    else:
        cp = recording.custom_properties or {}
        own_hls_dir = cp.get("_hls_dir")
        status = cp.get("status", "")
        remux_success = cp.get("remux_success")
        same_dir = bool(own_hls_dir) and Path(own_hls_dir) == hls_dir

        if same_dir and status == "recording":
            classification = "active"
            detail = "Recording is currently in progress"
        elif same_dir and remux_success is False:
            classification = "preserved_failure"
            detail = "Concat/remux failed; Dispatcharr kept this as the only surviving copy"
        elif same_dir:
            classification = "referenced"
            detail = f"Recording {recording_id} (status={status!r}) still references this directory"
        else:
            classification = "referenced"
            detail = (
                f"Recording {recording_id} exists but its _hls_dir "
                f"({own_hls_dir!r}) doesn't match this path -- needs manual review"
            )

    return {
        "path": str(hls_dir),
        "recording_id": recording_id,
        "recording_exists": recording is not None,
        "classification": classification,
        "detail": detail,
        "segment_count": segment_count,
    }


def _list_dvr_hls_staging_dirs():
    """Read-only scan for every .dvr_*_hls staging directory anywhere
    under /data/recordings, each with its best-effort classification.
    Deliberately scans the whole recordings root (unlike the destructive
    scrub action) -- there's no deletion risk in reading a directory
    listing, only in acting on it, and a staging directory could in
    principle be found directly under the root as well as nested under a
    DVR path template's own subdirectory."""
    if not _RECORDINGS_ROOT.is_dir():
        return []

    results = []
    for path in sorted(_RECORDINGS_ROOT.rglob(".dvr_*_hls")):
        if not path.is_dir():
            continue
        info = _classify_dvr_hls_dir(path)
        if info:
            results.append(info)
    return results


def _delete_orphaned_dvr_hls_dirs(logger):
    """Deletes every .dvr_*_hls directory _classify_dvr_hls_dir calls
    "orphaned" -- no Recording row with that id exists at all -- and
    nothing else. Never touches "active" (a recording in progress),
    "preserved_failure" (concat failed, kept as the only surviving copy),
    or "referenced" (a Recording row exists but the signal isn't clean --
    needs manual review) directories, empty or not; see
    _classify_dvr_hls_dir's own docstring for the full reasoning behind
    each category. Classification is freshly recomputed here (via
    _list_dvr_hls_staging_dirs(), not a stale list from an earlier click),
    so this always acts on current state at the moment it actually runs.

    Deliberately does not prune parent directories left empty by a
    deletion -- scrub_orphaned_sidecars's own empty-directory sweep
    already covers that for any parent within a DVR path template's own
    scope, and reaching further than that here would repeat exactly the
    scope mistake _dvr_sidecar_scan_roots's own docstring documents."""
    removed = []
    errors = []
    for info in _list_dvr_hls_staging_dirs():
        if info["classification"] != "orphaned":
            continue
        path = Path(info["path"])
        try:
            shutil.rmtree(path)
        except OSError as exc:
            errors.append(f"{path}: {exc}")
            continue
        removed.append(str(path))
        if logger:
            logger.info("recording_edl: removed orphaned .dvr_*_hls directory %s", path)
    return removed, errors


class Plugin:
    name = "Recording EDL"
    version = "1.0.0"
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
                "playback) via the plugin run/ API, not usually by hand -- "
                "the field below is only for manually testing the action "
                "button."
            ),
        },
        {
            "id": "test_recording_id", "label": "Test recording ID", "type": "string",
            "default": "",
            "help_text": (
                "Only used by the manual-test button below (plugin action "
                "buttons can't take click-time input) -- paste a "
                "recording's numeric id here, save, then use Get Recording "
                "EDL. The real integration (a client calling run/ over the "
                "REST API) passes recording_id directly and ignores this "
                "field."
            ),
        },
    ]

    actions = [
        {
            "id": "get_edl",
            "label": "Get Recording EDL",
            "description": (
                "Returns the comskip EDL entries for a recording. Params: "
                "recording_id (required, pass it as a param, or paste one "
                "into the test_recording_id setting for manual testing)."
            ),
        },
        {
            "id": "scrub_orphaned_sidecars",
            "label": "Scrub Orphaned EDL/Logo Files",
            "description": (
                "Removes comskip .edl and .logo.txt files left behind by "
                "a deleted recording (Dispatcharr's own recording delete "
                "never removes them), then removes any now-or-already-"
                "empty directory under DVR Settings' TV/Movie Path and "
                "Fallback Template locations. Only ever removes a "
                "sidecar when no other file in its folder shares its "
                "base name, and never descends into or removes a "
                "dot-prefixed directory (.dvr_*_hls staging dirs, "
                "Dispatcharr/plugin-internal directories) -- scoped "
                "strictly to where your own DVR path templates point, "
                "nothing else under /data/recordings."
            ),
            "confirm": {
                "required": True,
                "title": "Scrub orphaned .edl/.logo.txt files?",
                "message": (
                    "Permanently deletes every .edl and .logo.txt file "
                    "with no matching recording left, and removes every "
                    "now-or-already-empty directory under your DVR "
                    "Settings' TV/Movie path templates. This cannot be "
                    "undone."
                ),
            },
        },
        {
            "id": "list_dvr_hls_staging_dirs",
            "label": "List DVR HLS Staging Directories",
            "description": (
                "Read-only diagnostic: finds every .dvr_*_hls staging "
                "directory under /data/recordings and reports its best-"
                "effort classification -- \"active\" (a recording in "
                "progress), \"preserved_failure\" (concat failed, kept as "
                "the only surviving copy), \"referenced\" (a Recording row "
                "points at it but neither of the above is confirmed -- "
                "needs manual review), or \"orphaned\" (no Recording row "
                "with that id exists at all). Never deletes anything -- "
                "for manual review before deciding what, if anything, is "
                "actually safe to clean up."
            ),
        },
        {
            "id": "delete_orphaned_dvr_hls_dirs",
            "label": "Delete Orphaned DVR HLS Directories",
            "description": (
                "Deletes only the .dvr_*_hls directories List DVR HLS "
                "Staging Directories classifies as \"orphaned\" -- no "
                "Recording row with that id exists at all. Never touches "
                "\"active\", \"preserved_failure\", or \"referenced\" "
                "directories, empty or not. Does not prune parent "
                "directories left empty by a deletion -- run Scrub "
                "Orphaned EDL/Logo Files for that."
            ),
            "confirm": {
                "required": True,
                "title": "Delete orphaned .dvr_*_hls directories?",
                "message": (
                    "Permanently deletes every .dvr_*_hls directory with "
                    "no Recording row referencing it at all. Active and "
                    "preserved-failure directories are never touched. "
                    "This cannot be undone."
                ),
            },
        },
    ]

    def run(self, action: str, params: dict, context: dict):
        logger = context.get("logger")
        settings_dict = context.get("settings", {})

        if action == "scrub_orphaned_sidecars":
            removed_files, removed_dirs, errors = _scrub_orphaned_recording_sidecars(logger)
            if not removed_files and not removed_dirs and not errors:
                message = "No orphaned .edl/.logo.txt files or empty directories found"
            else:
                message = f"Removed {len(removed_files)} orphaned file(s), {len(removed_dirs)} empty directory(s)"
                if errors:
                    message += f", {len(errors)} error(s) (see plugin log)"
            return {
                "status": "ok",
                "message": message,
                "removed_files": removed_files,
                "removed_directories": removed_dirs,
                "errors": errors,
            }

        if action == "list_dvr_hls_staging_dirs":
            dirs = _list_dvr_hls_staging_dirs()
            if not dirs:
                message = "No .dvr_*_hls staging directories found"
            else:
                counts = {}
                for d in dirs:
                    counts[d["classification"]] = counts.get(d["classification"], 0) + 1
                summary = ", ".join(f"{count} {label}" for label, count in counts.items())
                entries = "; ".join(
                    f"{Path(d['path']).name} ({d['classification']}, "
                    f"{d['segment_count'] if d['segment_count'] is not None else '?'} segs)"
                    for d in dirs
                )
                message = f"{len(dirs)} .dvr_*_hls dir(s) found ({summary}): {entries}"
            return {"status": "ok", "message": message, "directories": dirs}

        if action == "delete_orphaned_dvr_hls_dirs":
            removed, errors = _delete_orphaned_dvr_hls_dirs(logger)
            if not removed and not errors:
                message = "No orphaned .dvr_*_hls directories found"
            else:
                message = f"Removed {len(removed)} orphaned .dvr_*_hls director{'y' if len(removed) == 1 else 'ies'}"
                if errors:
                    message += f", {len(errors)} error(s) (see plugin log)"
            return {"status": "ok", "message": message, "removed": removed, "errors": errors}

        if action != "get_edl":
            return {"status": "error", "message": f"Unknown action: {action}"}

        recording_id = params.get("recording_id") or settings_dict.get("test_recording_id")
        if not recording_id:
            return {
                "status": "error",
                "message": "recording_id is required (pass it as a param, or paste one into the test_recording_id setting for manual testing)",
            }

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
            return {"status": "ok", "message": "No EDL file for this recording (comskip hasn't run, or ran in \"cut\" mode)", "entries": []}

        edl_path = Path(file_path).parent / edl_filename
        try:
            text = edl_path.read_text(errors="replace")
        except OSError as exc:
            if logger:
                logger.warning("recording_edl: could not read %s: %s", edl_path, exc)
            return {"status": "ok", "message": f"Could not read {edl_path.name} (see plugin log)", "entries": []}

        # "message" is what actually shows up in Dispatcharr's own result
        # toast -- confirmed against its frontend (PluginCard.jsx's
        # handlePluginRun()) that nothing else in an action's response is
        # ever rendered anywhere in that UI, same finding that led to
        # timeshift_buffer's own diagnostic actions all getting one too.
        entries = _parse_edl(text)
        message = "No EDL entries found" if not entries else f"{len(entries)} EDL entr{'y' if len(entries) == 1 else 'ies'} found"
        return {"status": "ok", "message": message, "entries": entries}
