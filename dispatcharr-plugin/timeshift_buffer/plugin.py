"""
Timeshift Buffer -- a Dispatcharr plugin.

Records a rolling, per-channel HLS-style buffer to disk so a client (this
plugin was designed alongside pvr.dispatcharrai, a Kodi PVR addon) can
pause/rewind live TV without needing a local, on-device buffer the way
inputstream.ffmpegdirect's own timeshift mode provides today.

Design notes (see docs/API_NOTES.md in pvr.dispatcharrai and the
conversation that produced this draft for the full reasoning):

- Reads from Dispatcharr's own live proxy (/proxy/ts/stream/<uuid>) rather
  than re-fetching from the upstream provider directly. Confirmed via
  apps/proxy/live_proxy's source that multiple viewers of one channel
  already share a single upstream connection, so this plugin acting as
  "one more viewer" doesn't cost an extra connection against whatever
  concurrent-stream limit the upstream provider enforces, as long as
  someone (a real viewer, or this buffer itself) is already/also watching.
- Confirmed stream_ts (the view behind that URL) is @permission_classes
  ([AllowAny]), gated only by network_access_allowed(request, "STREAMS")
  -- unrestricted by default. A loopback request from inside this plugin's
  own process needs no API key/token. If you've deliberately narrowed the
  STREAMS network-access setting to exclude localhost, add it back or this
  plugin can't reach the proxy.
- ffmpeg's own segment muxer does almost all the hard work: -segment_wrap
  recycles old segment filenames instead of growing forever, and
  -segment_list/-segment_list_size/-segment_list_flags +live maintains a
  sliding-window HLS playlist natively. No Python-side trimming loop is
  needed for the common case -- only an idle-timeout reaper (below), since
  nothing else would ever stop a buffer once started.
- Segment files must NOT live under Django's MEDIA_ROOT directly (that
  resolves to <app dir>/media, which -- confirmed against the project's
  own docker-compose.yml -- isn't under the one volume (./data:/data) the
  container actually bind-mounts, so it wouldn't survive a container
  recreate and wouldn't benefit from redirecting it to real storage the
  way this project's recordings path already can be). Files live under the
  configurable storage_path (default /data/timeshift) instead.
- Originally tried serving those files by symlinking MEDIA_ROOT/timeshift
  -> storage_path, relying on Django's existing static(MEDIA_URL, ...)
  route. Confirmed live against a real instance that this doesn't work:
  dispatcharr/urls.py's catch-all SPA route
  (path("<path:unused_path>", TemplateView...)) is concatenated BEFORE the
  appended static() patterns, and Django tries patterns in order, so the
  catch-all wins for every /media/... request and returns the React app
  shell instead of the file -- MEDIA_ROOT is effectively unreachable
  directly in this deployment mode, a routing quirk in Dispatcharr itself,
  not something this plugin can fix from the outside. Since plugins can't
  register their own URL routes either (confirmed via apps/plugins/loader.py
  -- no route-registration hook exists), this plugin instead runs its own
  minimal HTTP server (see BufferHTTPServer below), bound to its own port
  (http_port setting) directly on files under storage_path. That port needs
  to be exposed through your container config, the same way 9191 already
  is -- this is the one real infrastructure requirement beyond installing
  the plugin.
- Idle-timeout liveness comes from the HTTP server itself, not from a
  client explicitly calling the heartbeat action: every successful file
  fetch (playlist or segment) refreshes last_heartbeat. This matters
  because a Kodi PVR addon using plain STREAMURL passthrough for live
  channels (no OpenLiveStream/CloseLiveStream) gets no callback at all for
  "the user stopped watching" -- but inputstream.ffmpegdirect re-fetches a
  live .m3u8 on an interval for as long as playback continues and simply
  stops once it doesn't, so the request stream to this server already *is*
  the liveness signal, with nothing extra required from whatever's playing
  the stream.

Verified live end-to-end against a real Dispatcharr instance and a real
pvr.dispatcharrai build: buffer capture, this plugin's own HTTP serving,
and a real channel opening and playing cleanly through
inputstream.ffmpegdirect are all confirmed working. One real finding from
that pass: the live buffer's rolling playlist (Duration: N/A, by design --
that's what lets it keep tailing new content) means Kodi's own PVR layer
reports canseek: false and a Player.Seek call fails outright, confirmed
live, not just a stale UI report -- the same root cause already documented
in pvr.dispatcharrai's docs/API_NOTES.md for in-progress-recording "Play
live": Kodi-core requires a known, *finite* duration to permit seeking at
all, independent of whatever INPUTSTREAM_SUPPORTS_SEEK the inputstream
addon itself advertises, and a perpetually-growing buffer can never
provide one by definition.

snapshot_buffer (below) is the fix for that, mirroring the same trade-off
pvr.dispatcharrai already applies to in-progress recordings ("Play live"
tails forever with no seek; "Play from start" is a one-shot, finite,
ENDLIST-terminated snapshot with real seek but stops following new
content). It **copies** the buffer's currently-listed segment files into a
separate, non-recycled snapshot/ subdirectory rather than just writing a
differently-shaped playlist against the same live files -- the live
buffer's own -segment_wrap keeps recycling those original files in the
background the whole time a snapshot might be watched, so referencing them
directly would risk the exact "a segment gets overwritten while a client
still has it queued up" race this project already got bitten by once
before (see LocalPlaylistServer.cpp's gradual-cap fix in pvr.dispatcharrai
for that history) -- copying sidesteps it entirely by giving the snapshot
its own segment files the live recording can never touch.

snapshot_buffer itself is confirmed live: playback opens correctly and
Kodi reports canseek: true. Seeking within it, however, was confirmed
live to fail 100% of the time (every position tried, both directions) --
not the "imprecise" behavior this project's other ffmpegdirect-routed
seeking (catch-up, in-progress recordings) has, but every seek producing
"unknown position after seek" in inputstream.ffmpegdirect's own log,
immediately followed by the demuxer hitting EOF. Root-caused by reading
inputstream.ffmpegdirect's own source: pvr.dispatcharrai's server-side
stream properties never set inputstream.ffmpegdirect.stream_mode, so
ffmpegdirect falls back to its generic FFmpegStream class and a plain
av_seek_frame() -- unlike this addon's catch-up feature (stream_mode:
catchup, a specialized FFmpegCatchupStream with its own seek logic) or
its local live-timeshift mode (stream_mode: timeshift, TimeshiftStream's
own local buffer and seek), neither of which hits this. The generic
class's seek needs a global target PTS to actually exist within some
segment's own PTS space -- _start_ffmpeg's -reset_timestamps 1 (removed
below) made every segment restart its PTS near zero, so no segment's
local timeline ever actually contained the computed global target.
Removed it (keeping timestamps continuous across segments, the way
real-world HLS packaging normally does) as a fix attempt for that, then
redeployed and re-tested live against a freshly-started buffer (old one
explicitly stopped first, so the test genuinely exercised the new
command): identical failure, same log signature, same stuck speed: 0.
That rules out -reset_timestamps as the cause. The flag stays removed
(continuous timestamps aren't harmful and are closer to normal HLS
practice) but the actual root cause of the av_seek_frame failure was
never chased further, because the real fix turned out to be architectural,
not another guess at ffmpegdirect's own seek internals -- see below.

SUPERSEDED, not just patched: pvr.dispatcharrai no longer routes the live
buffer through inputstream.ffmpegdirect (a STREAMURL) at all. It now
exposes the buffer via Kodi's own PVR_STREAM_PROPERTY-free
OpenLiveStream/ReadLiveStream/SeekLiveStream API instead, using Kodi's
native internal demuxer for MPEG-TS parsing and seek refinement --
the same mechanism this addon's completed-recording playback already
relied on successfully throughout this whole history. That needed two
things from this plugin: Range support in the file server (do_GET, see
below -- individual segment files are Range-read directly, no HLS
playlist involved at all for this path), and the get_live_manifest
action (also below), which exposes the buffer's currently-known segments
with a stable, HLS-media-sequence-derived `sequence` number so a client
can merge repeated fetches into one consistent, growing byte-address
space as the rolling window advances, instead of re-deriving fresh
(and silently shifting) offsets on every call. Confirmed live: real
pause/rewind/fast-forward/live-follow from plain Play on a real channel,
including a 95-second rewind that spanned several manifest refreshes.
The old snapshot_buffer/"Instant replay from buffer" workaround this
section describes is retired on the pvr.dispatcharrai side (plain Play
now gets everything it offered and more), though the action itself is
left in this plugin as a standalone capability -- see its own docstring.
"""

import json
import mimetypes
import os
import shutil
import signal
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

# ---------------------------------------------------------------------------
# Redis-backed state. A plain Python module-level dict would NOT be shared
# across uWSGI worker processes -- the worker that handles start_buffer may
# not be the one that later handles stop_buffer or heartbeat. Redis is the
# same shared-state mechanism apps/proxy/live_proxy itself uses for exactly
# this kind of cross-worker coordination (see RedisKeys/ChannelService in
# that app for the established pattern this mirrors).
# ---------------------------------------------------------------------------

_REDIS_PREFIX = "timeshift_buffer:"
_REDIS_LEADER_KEY = _REDIS_PREFIX + "reaper_leader"
_REDIS_LEADER_TTL = 30  # seconds; the reaper thread renews this while alive
# Generous headroom past any reasonable idle_timeout_seconds -- a self-healing
# backstop in case the reaper thread itself dies or Redis outlives a container
# restart while the ffmpeg processes it was tracking don't: worst case, a
# buffer state entry (and whatever it points at) disappears on its own
# instead of lingering forever pointing at a dead PID.
_BUFFER_STATE_TTL = 600

_reaper_thread = None
_reaper_stop_event = None


def _redis():
    # core.utils.RedisClient, the same pattern core code uses inside
    # apps/timeshift/api_views.py (update_catchup_session_position ->
    # _trigger_timeshift_stats_update). Confirmed safe for plugin use by
    # extensive live use this session -- buffer state and reaper
    # leader-election both rely on it working correctly across worker
    # processes, and did throughout testing.
    from core.utils import RedisClient

    return RedisClient.get_client()


def _buffer_key(channel_uuid):
    return f"{_REDIS_PREFIX}buffer:{channel_uuid}"


def _get_buffer_state(channel_uuid):
    raw = _redis().get(_buffer_key(channel_uuid))
    return json.loads(raw) if raw else None


def _set_buffer_state(channel_uuid, state):
    _redis().set(_buffer_key(channel_uuid), json.dumps(state), ex=_BUFFER_STATE_TTL)


def _delete_buffer_state(channel_uuid):
    _redis().delete(_buffer_key(channel_uuid))


def _list_buffer_keys():
    return [k.decode() if isinstance(k, bytes) else k for k in _redis().keys(_buffer_key("*"))]


# ---------------------------------------------------------------------------
# Storage path
# ---------------------------------------------------------------------------


def _channel_dir(storage_path: str, channel_uuid: str) -> Path:
    return Path(storage_path) / str(channel_uuid)


# ---------------------------------------------------------------------------
# Minimal HTTP server for serving buffer files.
#
# Plugins can't register routes on Dispatcharr's own Django app (confirmed
# via apps/plugins/loader.py), and MEDIA_ROOT turned out to be unreachable
# in practice (see the module docstring). So this plugin serves storage_path
# itself, on its own port. Deliberately stdlib-only (http.server) rather
# than pulling in a dependency, matching Plugins.md's "keep dependencies
# minimal" guidance -- this only ever needs to serve small text playlists
# and a handful-of-seconds .ts segments to a single kind of client, nothing
# that needs a real web framework.
# ---------------------------------------------------------------------------

_http_server = None
_http_server_thread = None
_http_server_storage_path = None


class _BufferRequestHandler(BaseHTTPRequestHandler):
    """Serves GET /<channel_uuid>/<filename> straight from storage_path.

    No directory listing, no write support. Does support Range requests
    (added for the growing-live-buffer byte-stream path -- see
    get_live_manifest below and pvr.dispatcharrai's DispatcharrClient,
    which mirrors its already-proven recording-playback Range-read pattern
    against individual segment files here instead of one Dispatcharr-served
    recording file)."""

    server_version = "TimeshiftBufferHTTP/0.1"

    def log_message(self, fmt, *args):
        logger = getattr(self.server, "plugin_logger", None)
        if logger:
            logger.debug("timeshift_buffer http: " + fmt, *args)

    def _resolve_path(self):
        """Returns (channel_uuid, filesystem_path), or (None, None) if the
        request doesn't map to a real file under storage_path."""
        # Manual traversal guard even though .resolve() below would also
        # catch it -- fail fast and obviously rather than relying solely on
        # path resolution semantics for something serving network requests.
        raw = unquote(urlparse(self.path).path)
        parts = raw.strip("/").split("/")
        if ".." in parts or len(parts) < 2:
            return None, None
        channel_uuid = parts[0]

        storage_root = Path(self.server.storage_path).resolve()
        candidate = (storage_root / raw.lstrip("/")).resolve()
        if storage_root not in candidate.parents and candidate != storage_root:
            return None, None
        return channel_uuid, candidate

    def _touch_heartbeat(self, channel_uuid):
        # Every successful fetch (playlist or segment) counts as "someone's
        # still watching" -- this is what lets the idle reaper work without
        # the Kodi addon (or any other client) needing to separately call
        # the heartbeat action on some timer it doesn't naturally have.
        # inputstream.ffmpegdirect re-fetches a live .m3u8 on an interval for
        # as long as playback continues and simply stops once it doesn't, so
        # this request stream IS the liveness signal, not just a proxy for
        # one. Best-effort: a Redis hiccup here shouldn't fail the actual
        # file response.
        try:
            state = _get_buffer_state(channel_uuid)
            if state:
                state["last_heartbeat"] = time.time()
                _set_buffer_state(channel_uuid, state)
        except Exception:
            logger = getattr(self.server, "plugin_logger", None)
            if logger:
                logger.exception("timeshift_buffer: heartbeat-on-fetch failed for %s", channel_uuid)

    @staticmethod
    def _parse_range(range_header, file_size):
        """Parses a single-range "bytes=X-Y" / "bytes=X-" header value.
        Returns (start, end) inclusive, or None if absent/unparseable (caller
        falls back to serving the whole file) or (False, False) if the range
        is unsatisfiable (caller sends 416)."""
        if not range_header or not range_header.startswith("bytes="):
            return None
        spec = range_header[len("bytes="):].split(",")[0].strip()  # first range only; multi-range unsupported
        if "-" not in spec:
            return None
        start_str, _, end_str = spec.partition("-")
        try:
            if start_str == "":
                # "bytes=-N" -- last N bytes.
                suffix_len = int(end_str)
                if suffix_len <= 0:
                    return None
                start = max(0, file_size - suffix_len)
                end = file_size - 1
            else:
                start = int(start_str)
                end = int(end_str) if end_str != "" else file_size - 1
        except ValueError:
            return None
        if start < 0 or start >= file_size or end < start:
            return False, False
        return start, min(end, file_size - 1)

    def do_GET(self):
        channel_uuid, target = self._resolve_path()
        if target is None or not target.is_file():
            self.send_error(404, "Not found")
            return

        content_type = mimetypes.guess_type(str(target))[0]
        if target.suffix == ".m3u8":
            content_type = "application/vnd.apple.mpegurl"
        elif target.suffix == ".ts":
            content_type = "video/mp2t"
        content_type = content_type or "application/octet-stream"

        try:
            file_size = target.stat().st_size
            range_result = self._parse_range(self.headers.get("Range"), file_size)
            if range_result == (False, False):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return

            with target.open("rb") as f:
                if range_result is None:
                    data = f.read()
                    status = 200
                    content_range = None
                else:
                    start, end = range_result
                    f.seek(start)
                    data = f.read(end - start + 1)
                    status = 206
                    content_range = f"bytes {start}-{end}/{file_size}"
        except OSError:
            # Segment got recycled by ffmpeg's -segment_wrap between the
            # playlist listing it and this request reading it -- a real,
            # expected race for a live-recycling buffer, not a bug. Treat
            # it the same as "not there right now."
            self.send_error(404, "Not found")
            return

        self._touch_heartbeat(channel_uuid)

        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(data)))
        if content_range:
            self.send_header("Content-Range", content_range)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self):
        channel_uuid, target = self._resolve_path()
        if target is None or not target.is_file():
            self.send_error(404, "Not found")
            return
        self._touch_heartbeat(channel_uuid)
        self.send_response(200)
        self.send_header("Content-Length", str(target.stat().st_size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()


def _ensure_http_server_running(storage_path: str, port: int, logger):
    global _http_server, _http_server_thread, _http_server_storage_path

    if _http_server is not None:
        if _http_server_storage_path == storage_path and _http_server.server_port == port:
            return  # already running with the same config
        logger.info("timeshift_buffer: http server config changed, restarting")
        _stop_http_server(logger)

    try:
        server = ThreadingHTTPServer(("0.0.0.0", port), _BufferRequestHandler)  # noqa: S104 -- must be reachable from outside the container by design
    except OSError as exc:
        logger.error("timeshift_buffer: couldn't bind http server on port %d: %s", port, exc)
        return

    server.storage_path = storage_path
    server.plugin_logger = logger
    server.daemon_threads = True

    thread = threading.Thread(target=server.serve_forever, name="timeshift_buffer_http", daemon=True)
    thread.start()

    _http_server = server
    _http_server_thread = thread
    _http_server_storage_path = storage_path
    logger.info("timeshift_buffer: serving %s on 0.0.0.0:%d", storage_path, port)


def _stop_http_server(logger):
    global _http_server, _http_server_thread, _http_server_storage_path
    if _http_server is None:
        return
    try:
        _http_server.shutdown()
        _http_server.server_close()
    except Exception:
        logger.exception("timeshift_buffer: error stopping http server")
    _http_server = None
    _http_server_thread = None
    _http_server_storage_path = None


# ---------------------------------------------------------------------------
# ffmpeg process management
# ---------------------------------------------------------------------------


def _proxy_url(channel_uuid: str, base_url: str) -> str:
    # Confirmed reachable and working live, extensively, against the
    # deployment this was developed against. Assumes nginx itself listens
    # on this loopback address/port (matching the externally-mapped port in
    # docker-compose.yml); the modular compose config routes the web
    # service through a Unix socket behind nginx rather than necessarily
    # exposing a plain TCP port on its own, so this default may still need
    # adjusting on a differently-shaped deployment -- see the
    # internal_base_url setting's own help text.
    return f"{base_url.rstrip('/')}/proxy/ts/stream/{channel_uuid}"


def _stream_owner_auth_header(settings_dict: dict, logger):
    """Returns an `Authorization: Bearer ...\\r\\n` header (ffmpeg's -headers
    format) for the stream_owner_username setting, or None if that setting
    is blank or resolving it fails. stream_ts() (apps/proxy/live_proxy/
    views.py) is decorated @api_view, so it's a DRF view -- request.user is
    only ever populated from DEFAULT_AUTHENTICATION_CLASSES (JWTAuthentication
    plus this project's own ApiKeyAuthentication), never from a plain
    unauthenticated GET the way ffmpeg's own -i connection makes one.
    Without this, the connection registers with user=None, and
    StreamConnectionCard.jsx (frontend) shows any uid that's falsy or the
    string '0' as 'Anonymous' -- confirmed by reading both files, not
    guessed. Generating the token directly via rest_framework_simplejwt
    (this plugin runs in-process with Django, same as any other app) avoids
    an extra HTTP round trip through the same login flow pvr.dispatcharrai
    itself uses against /api/accounts/token/."""
    username = (settings_dict.get("stream_owner_username") or "").strip()
    if not username:
        return None
    try:
        from django.contrib.auth import get_user_model
        from rest_framework_simplejwt.tokens import RefreshToken

        User = get_user_model()
        user = User.objects.get(username=username)
        access_token = str(RefreshToken.for_user(user).access_token)
        return f"Authorization: Bearer {access_token}\r\n"
    except Exception:
        # Best-effort: a buffer that streams anonymously is still a working
        # buffer. Don't let a typo'd username or an import hiccup block
        # start_buffer entirely.
        logger.exception(
            "timeshift_buffer: couldn't mint a stream-owner token for user %r -- "
            "buffer will stream anonymously (check stream_owner_username matches "
            "an existing Dispatcharr username)",
            username,
        )
        return None


def _start_ffmpeg(channel_uuid: str, settings_dict: dict, logger) -> dict:
    storage_path = settings_dict.get("storage_path", "/data/timeshift")
    segment_seconds = int(settings_dict.get("segment_seconds", 6))
    buffer_minutes = int(settings_dict.get("buffer_minutes", 60))
    base_url = settings_dict.get("internal_base_url", "http://127.0.0.1:9191")
    http_port = int(settings_dict.get("http_port", 9192))

    visible_segments = max(1, (buffer_minutes * 60) // segment_seconds)
    # Wrap (filename reuse) past a larger count than what the playlist
    # advertises as visible, so a client that just requested an old segment
    # has headroom before ffmpeg overwrites that same filename in place --
    # the same class of "don't reveal/rely on something about to move under
    # you" caution this project already applied to its own gradual-cap fix
    # for in-progress-recording playback (see pvr.dispatcharrai's
    # LocalPlaylistServer.cpp), just via a size margin here instead of a
    # request-count hold.
    wrap_segments = visible_segments * 2

    channel_dir = _channel_dir(storage_path, channel_uuid)
    channel_dir.mkdir(parents=True, exist_ok=True)
    playlist_path = channel_dir / "live.m3u8"
    # Deliberately a bare relative filename, not channel_dir / "...": ffmpeg
    # writes whatever this template evaluates to verbatim into the segment
    # list. An absolute path here would put absolute filesystem paths in
    # the .m3u8, which an HTTP client can't resolve as a URI against the
    # playlist's own URL. Run with cwd=channel_dir below so the actual
    # files still land in the right place on disk.
    segment_pattern = "seg_%05d.ts"

    # Deliberately NOT -reset_timestamps 1: that flag makes every segment's
    # own PTS restart near zero, which is fine for a client that just plays
    # segments back-to-back but breaks inputstream.ffmpegdirect's seeking on
    # a snapshot_buffer-produced ENDLIST playlist -- confirmed live (100%
    # reproduction across forward/backward seeks at multiple positions):
    # av_seek_frame computes a global target PTS that, with resets, no
    # single segment's local PTS space actually contains, so ffmpegdirect
    # logs "unknown position after seek" and the demuxer hits EOF instead of
    # landing on the target. Leaving timestamps continuous across segments
    # (matching how real-world HLS packagers do it) is what a seek needs to
    # actually resolve to a real position within the file.
    auth_header = _stream_owner_auth_header(settings_dict, logger)

    cmd = [
        "ffmpeg",
        "-nostdin",
        "-loglevel", "warning",
    ]
    if auth_header:
        # Must precede -i: ffmpeg applies -headers to the input that
        # follows it, not globally.
        cmd += ["-headers", auth_header]
    cmd += [
        "-i", _proxy_url(channel_uuid, base_url),
        "-c", "copy",
        "-f", "segment",
        "-segment_time", str(segment_seconds),
        "-segment_wrap", str(wrap_segments),
        "-segment_list", str(playlist_path),
        "-segment_list_size", str(visible_segments),
        "-segment_list_flags", "+live",
        "-segment_list_type", "m3u8",
        segment_pattern,
    ]

    log_path = channel_dir / "ffmpeg.log"
    log_file = open(log_path, "ab")  # noqa: SIM115 -- lifetime tied to the subprocess, closed on stop
    proc = subprocess.Popen(  # noqa: S603 -- fixed argv, no shell, no user-controlled binary
        cmd,
        cwd=str(channel_dir),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,  # own process group, so SIGTERM below doesn't touch the plugin's own process
    )

    logger.info(
        "timeshift_buffer: started ffmpeg pid=%s for channel %s (buffer=%dmin, segment=%ds, visible=%d, wrap=%d)",
        proc.pid, channel_uuid, buffer_minutes, segment_seconds, visible_segments, wrap_segments,
    )

    return {
        "channel_uuid": str(channel_uuid),
        "pid": proc.pid,
        "started_at": time.time(),
        "last_heartbeat": time.time(),
        "storage_path": storage_path,
        "playlist_path": str(playlist_path),  # on-disk path, for local debugging (ffmpeg.log lives next to it)
        "http_port": http_port,
        # Path component only -- the plugin can't know its own externally-
        # reachable hostname from inside the container. The caller builds
        # the full URL as http://<whatever host it already uses>:<http_port><playlist_route>.
        "playlist_route": f"/{channel_uuid}/live.m3u8",
        "log_path": str(log_path),
    }


def _stop_ffmpeg(state: dict, logger):
    pid = state.get("pid")
    if not pid:
        return
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass  # already gone
    except Exception:
        logger.exception("timeshift_buffer: failed to signal ffmpeg pid %s", pid)
        return

    # Give it a moment to exit cleanly (flush the segment list/moov, etc.)
    # before escalating -- mirrors this project's own Plugins.md-documented
    # stop() pattern (track a pid, SIGTERM it, log the outcome).
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            os.killpg(pid, 0)  # signal 0: check it's still alive, don't actually signal
        except ProcessLookupError:
            return
        time.sleep(0.2)

    logger.warning("timeshift_buffer: ffmpeg pid %s didn't exit after SIGTERM, sending SIGKILL", pid)
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _remove_channel_files(state: dict, logger):
    # shutil.rmtree rather than a flat glob+unlink+rmdir: a channel
    # directory can contain the snapshot/ subdirectory created by
    # _create_snapshot(), and the flat approach would leave that behind
    # (non-recursive glob wouldn't touch it, then rmdir() would fail on a
    # non-empty directory) -- this cleans up both in one pass regardless of
    # whether a snapshot was ever taken for this channel.
    channel_dir = Path(state["storage_path"]) / state["channel_uuid"]
    try:
        shutil.rmtree(channel_dir)
    except FileNotFoundError:
        pass
    except OSError:
        logger.exception("timeshift_buffer: couldn't fully clean up %s", channel_dir)


def _get_live_manifest(state: dict, logger) -> dict:
    """Builds a byte-addressable manifest of the buffer's currently-listed
    (live.m3u8) segments -- filename, byte size, duration, and cumulative
    byte/time offsets -- so a client can treat the rolling live buffer as
    one growing, seekable byte stream (Range-reading individual segment
    files directly, see _BufferRequestHandler's Range support) instead of
    going through inputstream.ffmpegdirect's HLS-seek machinery, which
    pvr.dispatcharrai's docs/TIMESHIFT.md documents as confirmed broken for
    this kind of buffer. Mirrors _create_snapshot's own live.m3u8 parsing
    (see its comments for the recycling-race handling) but returns a
    manifest instead of freezing a copy -- read fresh from disk on every
    call rather than cached, since the whole point is reflecting how far
    the buffer has grown since the caller last asked.

    The rolling window means "byte offset 0" in THIS response corresponds
    to whatever's currently oldest -- a later call's "byte offset 0" will
    be different content once the window has advanced. A client that wants
    a stable address space across repeated calls (pvr.dispatcharrai does,
    to avoid its own position bookkeeping going stale mid-playback) can't
    just concatenate offsets naively; each segment also carries an absolute
    `sequence` number (HLS's own #EXT-X-MEDIA-SEQUENCE plus its position in
    the list), which is stable for the life of the buffer regardless of how
    the visible window slides, and is what a client should key its own
    merged/cumulative table on instead of list position."""
    channel_dir = Path(state["storage_path"]) / state["channel_uuid"]
    live_playlist_path = channel_dir / "live.m3u8"
    if not live_playlist_path.is_file():
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet")

    lines = live_playlist_path.read_text(encoding="utf-8", errors="replace").splitlines()

    media_sequence = 0
    for line in lines:
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            try:
                media_sequence = int(line[len("#EXT-X-MEDIA-SEQUENCE:"):].strip())
            except ValueError:
                pass
            break

    segments = []
    cumulative_bytes = 0
    cumulative_ms = 0
    list_index = 0  # position within the m3u8's own segment list, before any drops
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("#EXTINF:") and i + 1 < len(lines) and not lines[i + 1].startswith("#"):
            seg_name = lines[i + 1].strip()
            sequence = media_sequence + list_index
            list_index += 1
            try:
                size = (channel_dir / seg_name).stat().st_size
            except OSError:
                # Recycled between the playlist listing it and this stat --
                # the same race _create_snapshot already tolerates. Drop it
                # rather than fail the whole manifest over one segment (but
                # list_index/sequence still advanced above, so later
                # segments keep their true, stable sequence numbers).
                i += 2
                continue
            try:
                duration_ms = int(round(float(line[len("#EXTINF:"):].rstrip(",")) * 1000))
            except ValueError:
                duration_ms = 0
            segments.append({
                "filename": seg_name,
                "sequence": sequence,
                "byte_offset": cumulative_bytes,
                "byte_size": size,
                "time_offset_ms": cumulative_ms,
                "duration_ms": duration_ms,
            })
            cumulative_bytes += size
            cumulative_ms += duration_ms
            i += 2
        else:
            i += 1

    if not segments:
        raise RuntimeError("no segments currently available -- the buffer may be too new")

    return {
        "media_sequence": media_sequence,
        "segments": segments,
        "total_bytes": cumulative_bytes,
        "total_duration_ms": cumulative_ms,
    }


def _create_snapshot(state: dict, logger) -> str:
    """Freezes the buffer's currently-listed segments into a separate,
    non-recycled snapshot/ subdirectory with an ENDLIST-terminated
    playlist, so a client gets real seek within a finite window instead of
    the live buffer's endless-but-unseekable tail. Returns the new
    playlist's path component (same convention as start_buffer's
    playlist_route). Raises RuntimeError on failure -- callers translate
    that into the usual {"status": "error", "message": ...} shape."""
    channel_dir = Path(state["storage_path"]) / state["channel_uuid"]
    live_playlist_path = channel_dir / "live.m3u8"
    if not live_playlist_path.is_file():
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet")

    lines = live_playlist_path.read_text(encoding="utf-8", errors="replace").splitlines()

    snapshot_dir = channel_dir / "snapshot"
    if snapshot_dir.exists():
        shutil.rmtree(snapshot_dir)
    snapshot_dir.mkdir(parents=True)

    # Deliberately reuses the live playlist's own text verbatim (headers,
    # #EXTINF durations, everything) rather than re-deriving any of it --
    # the only things that actually change are: drop any segment whose file
    # didn't make it (see below), copy the ones that did into snapshot_dir,
    # and append ENDLIST. Less code, and guarantees the snapshot's segment
    # timing exactly matches what ffmpeg itself already reported rather
    # than a second, potentially-inconsistent computation of the same
    # numbers.
    out_lines = []
    copied_count = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("#EXTINF:") and i + 1 < len(lines) and not lines[i + 1].startswith("#"):
            seg_name = lines[i + 1].strip()
            src = channel_dir / seg_name
            try:
                shutil.copy2(src, snapshot_dir / seg_name)
            except OSError:
                # Recycled by the still-running live buffer's -segment_wrap
                # between the live playlist listing it and this copy --
                # exactly the race this function exists to avoid exposing
                # to a client, caught here instead: drop this one entry
                # rather than fail the whole snapshot over it.
                i += 2
                continue
            out_lines.append(line)
            out_lines.append(seg_name)
            copied_count += 1
            i += 2
        else:
            if not line.startswith("#EXT-X-ENDLIST"):
                out_lines.append(line)
            i += 1

    if copied_count == 0:
        raise RuntimeError(
            "no segments could be copied for the snapshot -- the buffer may be too new, or "
            "every currently-listed segment was recycled before it could be copied"
        )

    out_lines.append("#EXT-X-ENDLIST")

    snapshot_playlist_path = snapshot_dir / "snapshot.m3u8"
    snapshot_playlist_path.write_text("\n".join(out_lines) + "\n", encoding="utf-8")

    logger.info(
        "timeshift_buffer: created snapshot for channel %s with %d segments",
        state["channel_uuid"], copied_count,
    )
    return f"/{state['channel_uuid']}/snapshot/snapshot.m3u8"


# ---------------------------------------------------------------------------
# Idle reaper -- the one thing that actually needs a background loop, since
# nothing else ever stops a buffer once started. Leader-elected via Redis
# (SET NX with a TTL, renewed while alive) so only one worker process's
# thread is actually reaping at a time even though every worker loads this
# plugin module independently.
# ---------------------------------------------------------------------------


def _reaper_loop(settings_getter, logger, stop_event: threading.Event):
    client = _redis()
    my_token = f"{os.getpid()}:{time.time()}"

    while not stop_event.is_set():
        try:
            got_leadership = client.set(_REDIS_LEADER_KEY, my_token, nx=True, ex=_REDIS_LEADER_TTL)
            if not got_leadership:
                current = client.get(_REDIS_LEADER_KEY)
                current_str = current.decode() if isinstance(current, bytes) else current
                if current_str == my_token:
                    got_leadership = True
                    client.expire(_REDIS_LEADER_KEY, _REDIS_LEADER_TTL)

            if got_leadership:
                idle_timeout = int(settings_getter().get("idle_timeout_seconds", 120))
                now = time.time()
                for key in _list_buffer_keys():
                    raw = client.get(key)
                    if not raw:
                        continue
                    state = json.loads(raw)
                    if now - state.get("last_heartbeat", 0) > idle_timeout:
                        logger.info(
                            "timeshift_buffer: reaping idle buffer for channel %s (no heartbeat for %ds)",
                            state["channel_uuid"], int(now - state.get("last_heartbeat", 0)),
                        )
                        _stop_ffmpeg(state, logger)
                        _remove_channel_files(state, logger)
                        _delete_buffer_state(state["channel_uuid"])
        except Exception:
            logger.exception("timeshift_buffer: reaper tick failed")

        stop_event.wait(15)


def _ensure_reaper_running(settings_getter, logger):
    global _reaper_thread, _reaper_stop_event
    if _reaper_thread is not None and _reaper_thread.is_alive():
        return
    _reaper_stop_event = threading.Event()
    _reaper_thread = threading.Thread(
        target=_reaper_loop,
        args=(settings_getter, logger, _reaper_stop_event),
        name="timeshift_buffer_reaper",
        daemon=True,
    )
    _reaper_thread.start()


# ---------------------------------------------------------------------------
# Plugin class
# ---------------------------------------------------------------------------


class Plugin:
    name = "Timeshift Buffer"
    version = "0.1.0"
    description = (
        "Server-side rolling live-TV buffer per channel, so clients can "
        "pause/rewind live playback without a local on-device buffer."
    )
    author = "BruiserBrody17"
    help_url = "https://github.com/BruiserBrody17/pvr.dispatcharrai/tree/master/dispatcharr-plugin/timeshift_buffer"

    # The single source of truth for fields/actions -- confirmed live that
    # plugin.json's own copies (which Plugins.md's Quick Start example
    # duplicates alongside these, but this project doesn't) are never
    # actually read: PluginImportAPIView hardcodes an empty fields/actions
    # preview for a not-yet-trusted plugin regardless of plugin.json, and
    # once trusted/loaded, the running Plugin class (here) is what's
    # actually introspected. Tested directly: stripping fields/actions out
    # of plugin.json entirely while leaving this class untouched produced
    # an identical plugin listing.
    fields = [
        {
            "id": "about", "label": "About", "type": "info",
            "description": (
                "Started/stopped per channel by a client (e.g. "
                "pvr.dispatcharrai's live-timeshift setting) via the plugin "
                "run/ API, not usually by hand. The buttons below are for "
                "manual testing and emergency cleanup."
            ),
        },
        {
            "id": "storage_path", "label": "Buffer storage path", "type": "string",
            "default": "/data/timeshift",
            "help_text": (
                "Container path where segment files are written. Point this "
                "at a Docker volume mapped to real storage (the same way "
                "you'd map /data/recordings) -- do NOT leave this under "
                "Dispatcharr's own app directory, since continuous rolling "
                "writes don't belong on a small/fast appdata volume."
            ),
        },
        {
            "id": "buffer_minutes", "label": "Buffer length (minutes)", "type": "number",
            "default": 60,
            "help_text": (
                "How far back a viewer can rewind. Drives both "
                "segment_list_size (what the playlist advertises) and "
                "segment_wrap (when old segment files get reused)."
            ),
        },
        {
            "id": "segment_seconds", "label": "Segment length (seconds)", "type": "number",
            "default": 6,
            "help_text": "ffmpeg -segment_time. Shorter segments seek more precisely but multiply the number of files on disk.",
        },
        {
            "id": "idle_timeout_seconds", "label": "Idle timeout (seconds)", "type": "number",
            "default": 120,
            "help_text": "Stop a channel's buffer automatically if no heartbeat action arrives for this long.",
        },
        {
            "id": "max_concurrent_buffers", "label": "Max concurrent channel buffers", "type": "number",
            "default": 4,
            "help_text": "Safety cap -- each active buffer is a real ffmpeg process plus continuous disk writes.",
        },
        {
            "id": "internal_base_url", "label": "Internal base URL", "type": "string",
            "default": "http://127.0.0.1:9191",
            "help_text": (
                "How the plugin reaches Dispatcharr's own live proxy from "
                "inside the container. The default works for a standard "
                "docker-compose setup; if your deployment routes the web "
                "service differently (e.g. through a Unix socket behind "
                "nginx rather than a plain TCP port), adjust this to match."
            ),
        },
        {
            "id": "stream_owner_username", "label": "Attribute streams to (username)", "type": "string",
            "default": "",
            "help_text": (
                "Dispatcharr username to credit each buffer's ffmpeg "
                "connection to in the Stats screen -- without this, every "
                "buffer shows as 'Anonymous' there, confirmed live: "
                "stream_ts() (apps/proxy/live_proxy/views.py) is a DRF "
                "view, so it only resolves request.user from a JWT "
                "Authorization header, and ffmpeg's own -i connection to "
                "/proxy/ts/stream/<uuid> carries none by default. Leave "
                "blank to keep the current anonymous behavior. Set to an "
                "admin username (the same account already required to "
                "call this plugin's own actions works) to have the plugin "
                "mint a short-lived access token server-side (via "
                "rest_framework_simplejwt, no extra HTTP round trip) and "
                "pass it to ffmpeg as an Authorization header."
            ),
        },
        {
            "id": "http_port", "label": "Buffer server port", "type": "number",
            "default": 9192,
            "help_text": (
                "Port this plugin's own file server listens on (playlists "
                "and segments are served directly by the plugin, not "
                "through Dispatcharr's normal web port -- confirmed live "
                "that Dispatcharr's /media/ static route is unreachable in "
                "this deployment mode, see plugin.py's module docstring). "
                "Must be mapped through your container config the same way "
                "9191 already is, or clients outside the container can't "
                "reach it."
            ),
        },
        {
            "id": "test_channel_uuid", "label": "Test channel UUID", "type": "string",
            "default": "",
            "help_text": (
                "Only used for the manual-test buttons below, as a "
                "fallback when no channel_uuid param is supplied (plugin "
                "action buttons can't take click-time input) -- paste a "
                "channel's UUID here, save, then use Start/Stop Test "
                "Buffer. The real integration (a client calling run/ over "
                "the REST API) passes channel_uuid directly and ignores "
                "this field entirely."
            ),
        },
        {
            "id": "active_buffers", "label": "Active buffers", "type": "info",
            "description": 'Run "List Active Buffers" to see current state.',
        },
    ]

    actions = [
        {
            "id": "start_buffer", "label": "Start Buffer (manual test)",
            "description": "Starts a rolling buffer for a channel. Params: channel_uuid (required).",
            "button_label": "Start Test Buffer",
        },
        {
            "id": "stop_buffer", "label": "Stop Buffer",
            "description": "Stops a channel's buffer and removes its segment files. Params: channel_uuid (required).",
            "button_label": "Stop Test Buffer",
            "confirm": {"required": True, "title": "Stop buffer?", "message": "This ends the rolling buffer for the given channel and deletes its segment files."},
        },
        {
            "id": "heartbeat", "label": "Heartbeat",
            "description": "Refreshes a channel's idle timeout. Params: channel_uuid (required). Not required for normal use -- every file fetch through this plugin's HTTP server already refreshes it. Kept for a client that wants to explicitly signal 'still active' without an in-flight request (e.g. mid-pause), and for manual testing.",
        },
        {
            "id": "snapshot_buffer", "label": "Snapshot Buffer (manual test)",
            "description": (
                "Freezes the buffer's currently-recorded segments into a real, ENDLIST-terminated "
                "seekable playlist (params: channel_uuid, required -- start_buffer must already be "
                "running for it). The live buffer itself keeps recording in the background; this "
                "creates a separate, non-recycled snapshot that a client can seek within, at the "
                "cost of that snapshot not following new content -- the same trade-off as "
                "pvr.dispatcharrai's in-progress-recording 'Play from start' vs. 'Play live'."
            ),
            "button_label": "Take Test Snapshot",
        },
        {
            "id": "get_live_manifest", "label": "Get Live Manifest (manual test)",
            "description": (
                "Returns a byte-addressable manifest (segment filenames, byte sizes, durations, "
                "cumulative offsets) of the buffer's currently-listed segments (params: channel_uuid, "
                "required -- start_buffer must already be running). Used by pvr.dispatcharrai to treat "
                "the rolling live buffer as one growing, seekable byte stream via Range reads against "
                "individual segments, instead of routing through inputstream.ffmpegdirect's HLS-seek "
                "path (confirmed broken for this kind of buffer, see docs/TIMESHIFT.md)."
            ),
            "button_label": "Get Test Manifest",
        },
        {
            "id": "list_buffers", "label": "List Active Buffers",
            "description": "Shows every currently-running buffer and its age.",
            "button_label": "Refresh List",
        },
        {
            "id": "stop_all", "label": "Stop All Buffers",
            "description": "Emergency cleanup: stops every active buffer and removes all segment files.",
            "button_label": "Stop Everything",
            "button_variant": "filled",
            "button_color": "red",
            "confirm": {"required": True, "title": "Stop all buffers?", "message": "This ends every active rolling buffer right now, for every channel and every viewer currently using one."},
        },
    ]

    def run(self, action: str, params: dict, context: dict):
        settings_dict = context.get("settings", {})
        logger = context.get("logger")

        storage_path = settings_dict.get("storage_path", "/data/timeshift")
        Path(storage_path).mkdir(parents=True, exist_ok=True)
        _ensure_http_server_running(storage_path, int(settings_dict.get("http_port", 9192)), logger)
        _ensure_reaper_running(lambda: settings_dict, logger)

        if action == "start_buffer":
            return self._start_buffer(params, settings_dict, logger)
        if action == "stop_buffer":
            return self._stop_buffer(params, settings_dict, logger)
        if action == "heartbeat":
            return self._heartbeat(params, settings_dict, logger)
        if action == "snapshot_buffer":
            return self._snapshot_buffer(params, settings_dict, logger)
        if action == "get_live_manifest":
            return self._get_live_manifest_action(params, settings_dict, logger)
        if action == "list_buffers":
            return self._list_buffers()
        if action == "stop_all":
            return self._stop_all(logger)

        return {"status": "error", "message": f"Unknown action: {action}"}

    def stop(self, context: dict):
        """Called when the plugin is disabled, deleted, or reloaded."""
        logger = context.get("logger")
        if _reaper_stop_event is not None:
            _reaper_stop_event.set()
        self._stop_all(logger)
        _stop_http_server(logger)

    # -- action implementations --------------------------------------------
    #
    # Dispatcharr's plugin action buttons (per Plugins.md) don't support
    # entering a parameter at click-time -- clicking one just calls
    # run(action, {}, context) with params empty. That's fine for the real
    # integration (a REST caller supplies params directly), but it means a
    # human manually testing via the Plugins page has no way to type in a
    # channel_uuid before pressing "Start Test Buffer". _resolve_channel_uuid
    # falls back to the test_channel_uuid setting field for that case, so
    # manual testing is: paste a UUID into that field, save settings, then
    # use the buttons (which will act on whatever's currently saved there).

    @staticmethod
    def _resolve_channel_uuid(params, settings_dict):
        return params.get("channel_uuid") or settings_dict.get("test_channel_uuid")

    def _start_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (pass it as a param, or paste one into the test_channel_uuid setting for manual testing)",
            }

        existing = _get_buffer_state(channel_uuid)
        if existing:
            existing["last_heartbeat"] = time.time()
            _set_buffer_state(channel_uuid, existing)
            return {
                "status": "ok",
                "http_port": existing["http_port"],
                "playlist_route": existing["playlist_route"],
                "already_running": True,
            }

        max_concurrent = int(settings_dict.get("max_concurrent_buffers", 4))
        if len(_list_buffer_keys()) >= max_concurrent:
            return {
                "status": "error",
                "message": f"Already at max_concurrent_buffers ({max_concurrent})",
            }

        try:
            state = _start_ffmpeg(channel_uuid, settings_dict, logger)
        except FileNotFoundError:
            return {"status": "error", "message": "ffmpeg not found in this container"}
        except Exception as exc:
            logger.exception("timeshift_buffer: failed to start buffer for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        _set_buffer_state(channel_uuid, state)
        return {
            "status": "ok",
            "http_port": state["http_port"],
            "playlist_route": state["playlist_route"],
            "already_running": False,
        }

    def _stop_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {"status": "error", "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)"}

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "ok", "message": "no buffer was running"}

        _stop_ffmpeg(state, logger)
        _remove_channel_files(state, logger)
        _delete_buffer_state(channel_uuid)
        return {"status": "ok"}

    def _heartbeat(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {"status": "error", "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)"}

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "error", "message": "no buffer running for this channel"}

        state["last_heartbeat"] = time.time()
        _set_buffer_state(channel_uuid, state)
        return {"status": "ok"}

    def _snapshot_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {"status": "error", "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)"}

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "error", "message": "no buffer running for this channel -- call start_buffer first"}

        try:
            route = _create_snapshot(state, logger)
        except RuntimeError as exc:
            return {"status": "error", "message": str(exc)}
        except Exception as exc:
            logger.exception("timeshift_buffer: snapshot creation failed for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        # Counts as activity on the channel's own liveness clock too --
        # taking a snapshot is itself a sign this buffer is actively being
        # used, and the reaper's cleanup already covers the snapshot/
        # subdirectory it just created (see _remove_channel_files).
        state["last_heartbeat"] = time.time()
        _set_buffer_state(channel_uuid, state)

        return {"status": "ok", "http_port": state["http_port"], "playlist_route": route}

    def _get_live_manifest_action(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {"status": "error", "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)"}

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "error", "message": "no buffer running for this channel -- call start_buffer first"}

        try:
            manifest = _get_live_manifest(state, logger)
        except RuntimeError as exc:
            return {"status": "error", "message": str(exc)}
        except Exception as exc:
            logger.exception("timeshift_buffer: manifest build failed for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        # Same liveness-signal treatment as snapshot_buffer -- asking for the
        # manifest is itself a sign this buffer is actively being watched.
        state["last_heartbeat"] = time.time()
        _set_buffer_state(channel_uuid, state)

        return {
            "status": "ok",
            "http_port": state["http_port"],
            "channel_uuid": channel_uuid,
            "segment_route_prefix": f"/{channel_uuid}/",
            **manifest,
        }

    def _list_buffers(self):
        buffers = []
        now = time.time()
        for key in _list_buffer_keys():
            raw = _redis().get(key)
            if not raw:
                continue
            state = json.loads(raw)
            buffers.append({
                "channel_uuid": state["channel_uuid"],
                "age_seconds": int(now - state.get("started_at", now)),
                "idle_seconds": int(now - state.get("last_heartbeat", now)),
                "http_port": state.get("http_port"),
                "playlist_route": state.get("playlist_route"),
            })
        return {"status": "ok", "buffers": buffers}

    def _stop_all(self, logger):
        stopped = []
        for key in _list_buffer_keys():
            raw = _redis().get(key)
            if not raw:
                continue
            state = json.loads(raw)
            _stop_ffmpeg(state, logger)
            _remove_channel_files(state, logger)
            _delete_buffer_state(state["channel_uuid"])
            stopped.append(state["channel_uuid"])
        return {"status": "ok", "stopped": stopped}
