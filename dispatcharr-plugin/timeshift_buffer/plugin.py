"""
Timeshift Buffer -- a Dispatcharr plugin.

Records a rolling, per-channel HLS-style buffer to disk so a client (this
plugin was designed alongside pvr.dispatcharr-unofficial, a Kodi PVR addon) can
pause/rewind live TV without needing a local, on-device buffer the way
inputstream.ffmpegdirect's own timeshift mode provides today.

Design notes (see docs/API_NOTES.md in pvr.dispatcharr-unofficial and the
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
  the plugin. Every request needs a per-buffer access token (see
  _check_access_token), issued only via the authenticated start_buffer
  action -- reachability alone doesn't grant access, since this server has
  no other auth of its own (Dispatcharr's own session/API-key auth doesn't
  apply to it; nothing here proxies through Dispatcharr's normal web port).
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
pvr.dispatcharr-unofficial build: buffer capture, this plugin's own HTTP serving,
and a real channel opening and playing cleanly are all confirmed working.

This plugin originally routed live playback through
`inputstream.ffmpegdirect` (a plain `STREAMURL`), with a `snapshot_buffer`
action (copy the buffer's currently-listed segments into a separate,
finite, `ENDLIST`-terminated playlist a client could seek within, since
Kodi gates `canseek` on a known duration a perpetually-growing live
playlist can never have) as a workaround for that route's own seeking,
which turned out to be broken outright, not just imprecise. That whole
approach is **superseded**: pvr.dispatcharr-unofficial now exposes the buffer via
Kodi's own `OpenLiveStream`/`ReadLiveStream`/`SeekLiveStream` API instead,
using Kodi's native internal demuxer directly -- real seeking within the
growing live buffer itself, no snapshot needed at all. That needed two
things from this plugin: Range support in the file server (`do_GET`, see
below -- individual segment files are Range-read directly, no HLS
playlist involved for this path), and the `get_live_manifest` action
(also below), which exposes the buffer's currently-known segments with a
stable, HLS-media-sequence-derived `sequence` number so a client can merge
repeated fetches into one consistent, growing byte-address space as the
rolling window advances. Confirmed live: real pause/rewind/fast-forward/
live-follow from plain Play, including a 95-second rewind spanning
several manifest refreshes. `snapshot_buffer` and the ffmpegdirect route
it existed for have both been removed entirely as a result -- see
pvr.dispatcharr-unofficial's own docs/TIMESHIFT.md for the full investigation this
summary compresses, including the exact `av_seek_frame` failure signature
that motivated moving off ffmpegdirect in the first place.
"""

import contextlib
import ipaddress
import json
import mimetypes
import os
import secrets
import shutil
import signal
import socket
import subprocess
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

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
    # Single choke point for every "this buffer is gone" path (stop_buffer,
    # stop_all, the reaper, dead-buffer cleanup in start_buffer) -- drops
    # this worker's own cached manifest state (see _get_live_manifest's
    # _manifest_cache) too, so it doesn't outlive the buffer it was for.
    # A future start_buffer for the same channel_uuid always begins with a
    # fresh ffmpeg process and a fresh live.m3u8, whose mtime/size will
    # essentially never coincidentally match a stale cache entry's, but
    # dropping it here is cheap and removes any doubt.
    with _manifest_cache_lock:
        _manifest_cache.pop(channel_uuid, None)


def _list_buffer_keys():
    return [k.decode() if isinstance(k, bytes) else k for k in _redis().keys(_buffer_key("*"))]


def _iter_buffer_states():
    """Yields the parsed state dict for every currently-tracked buffer,
    skipping any key whose value is already gone by the time it's read
    (a real, if narrow, race between _list_buffer_keys() listing it and
    this read -- TTL expiry or a concurrent delete)."""
    client = _redis()
    for key in _list_buffer_keys():
        raw = client.get(key)
        if not raw:
            continue
        yield json.loads(raw)


# ---------------------------------------------------------------------------
# Storage path
# ---------------------------------------------------------------------------


def _channel_dir(storage_path: str, channel_uuid: str) -> Path:
    # Validated as a well-formed UUID before ever being used as a path
    # segment. channel_uuid can reach here from caller-supplied run/
    # action params (_resolve_channel_uuid() rejects a non-UUID value
    # before it gets this far, in the normal case) or from Redis-
    # persisted buffer state (which a pre-fix version of this plugin
    # could have written unvalidated) -- checking here too, at the one
    # place every caller actually builds a filesystem path, closes both.
    # A value like "../recordings" would otherwise let start_buffer's
    # mkdir and stop_buffer's/the reaper's later shutil.rmtree() (see
    # _remove_channel_files()) operate entirely outside storage_path --
    # the same class of traversal _BufferRequestHandler._resolve_path()
    # already guards against for the HTTP file server, just not
    # previously applied here.
    try:
        uuid.UUID(str(channel_uuid))
    except (ValueError, AttributeError, TypeError) as exc:
        raise ValueError(f"invalid channel_uuid: {channel_uuid!r}") from exc
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

    Requires a valid ?token=... query param matching that buffer's own
    access_token (see _check_access_token) on every request -- this
    server has no other access control, and reachability alone (this port
    is meant to be exposed outside the container, same as Dispatcharr's
    own) previously meant readability. No directory listing, no write
    support. Does support Range requests
    (added for the growing-live-buffer byte-stream path -- see
    get_live_manifest below and pvr.dispatcharr-unofficial's DispatcharrClient,
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
        spec = range_header[len("bytes=") :].split(",")[0].strip()  # first range only; multi-range unsupported
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

    def _check_access_token(self, channel_uuid):
        # Requires the caller to already know this specific buffer's own
        # token, issued only via the authenticated start_buffer/heartbeat
        # plugin actions (see _start_buffer's own comment) -- this file
        # server has no other access control of its own (see the module
        # docstring's "Idle-timeout liveness" bullet for why it's a plain
        # unauthenticated-by-Dispatcharr-standards HTTP server at all).
        # Without this, reaching this port at all (which the plugin's own
        # docs ask users to expose the same way as Dispatcharr's main
        # port -- often the whole LAN, sometimes further) was enough to
        # read any channel's currently-buffered live segments with zero
        # Dispatcharr credentials.
        state = _get_buffer_state(channel_uuid)
        expected = state.get("access_token") if state else None
        if not expected:
            return False
        provided = parse_qs(urlparse(self.path).query).get("token", [None])[0]
        return provided is not None and secrets.compare_digest(provided, expected)

    def _resolve_and_authorize(self):
        """Shared do_GET/do_HEAD preamble: resolves the request path,
        checks the access token, and confirms the target file exists --
        sending the appropriate error response itself on any failure.
        Returns (channel_uuid, target) on success, (None, None) otherwise
        (caller should just return immediately in that case)."""
        channel_uuid, target = self._resolve_path()
        if channel_uuid is None:
            self.send_error(404, "Not found")
            return None, None
        if not self._check_access_token(channel_uuid):
            self.send_error(403, "Forbidden")
            return None, None
        if target is None or not target.is_file():
            self.send_error(404, "Not found")
            return None, None
        return channel_uuid, target

    def do_GET(self):
        channel_uuid, target = self._resolve_and_authorize()
        if channel_uuid is None:
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
        channel_uuid, target = self._resolve_and_authorize()
        if channel_uuid is None:
            return
        self._touch_heartbeat(channel_uuid)
        self.send_response(200)
        self.send_header("Content-Length", str(target.stat().st_size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()


class _BufferHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer with SO_REUSEPORT set before bind.

    _http_server/_http_server_thread/_http_server_storage_path below are
    plain module-level globals -- fine for state genuinely local to one
    process, but Dispatcharr runs multiple WSGI worker processes (this is
    exactly why buffer state itself lives in Redis instead, see this
    file's own top-of-module comment), so each worker has its own
    separate copy of these globals. Without SO_REUSEPORT, only the first
    worker process ever asked to run a plugin action successfully binds
    this port; every other worker's own first attempt fails with "Address
    already in use" -- confirmed live, not theoretical: this is the exact
    error a real instance logged, repeatedly, during ordinary use. Worse
    than just log spam: if the one worker that *did* bind successfully
    later dies or gets recycled (routine for a WSGI server under normal
    operation), port 9192 goes completely unserved until some other
    worker happens to retry and win the now-open race -- and confirmed
    live that this window lines up with a real pvr.dispatcharr-unofficial
    live-timeshift stall (its HTTP reads against this server fail outright
    for as long as nothing is listening).

    SO_REUSEPORT lets every worker process bind its own socket on the same
    port at once, with the kernel load-balancing incoming connections
    across all of them -- safe specifically because every worker's
    listener serves identical content (the same shared storage_path files
    on disk), unlike the buffer state itself (needs one true, coordinated
    value -- Redis) or the reaper thread (must not run redundantly in
    every worker -- its own Redis leader election below). There's no
    "wrong" worker to answer a GET here, so there's nothing to coordinate:
    any worker's listener dying just means the others keep serving,
    without a gap, and a freshly-spawned replacement worker binds
    successfully on its own first attempt too."""

    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        super().server_bind()


def _ensure_http_server_running(storage_path: str, port: int, logger):
    global _http_server, _http_server_thread, _http_server_storage_path

    if _http_server is not None:
        if _http_server_storage_path == storage_path and _http_server.server_port == port:
            return  # already running with the same config
        logger.info("timeshift_buffer: http server config changed, restarting")
        _stop_http_server(logger)

    try:
        server = _BufferHTTPServer(("0.0.0.0", port), _BufferRequestHandler)  # noqa: S104 -- must be reachable from outside the container by design
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


def _stream_attribution_headers(params: dict, logger):
    """Returns an ffmpeg `-headers` string (each line `\\r\\n`-terminated)
    that makes the buffer's ffmpeg connection to /proxy/ts/stream/<uuid>
    attribute correctly in Dispatcharr's own Stats screen, or None if
    neither piece of info is available. Two independent problems, both
    confirmed by reading Dispatcharr's own source, not guessed:

    1. stream_ts() (apps/proxy/live_proxy/views.py) is decorated
       @api_view, so it's a DRF view -- request.user is only ever
       populated from DEFAULT_AUTHENTICATION_CLASSES (JWTAuthentication),
       never from a plain unauthenticated GET the way ffmpeg's own -i
       connection makes one. Without an Authorization header the
       connection registers with user=None, and StreamConnectionCard.jsx
       (frontend) shows any uid that's falsy or the string '0' as
       'Anonymous'. `params["username"]` is the Dispatcharr account
       pvr.dispatcharr-unofficial (or whichever client called start_buffer) is
       already configured with -- generating a token for it directly via
       rest_framework_simplejwt (this plugin runs in-process with Django)
       avoids a second login flow.
    2. Because this buffer's ffmpeg runs server-side inside Dispatcharr's
       own container rather than on the viewer's device, its connection's
       real REMOTE_ADDR is the container's own loopback address --
       confirmed live showing as 127.0.0.1 in Stats regardless of which
       device is actually watching. dispatcharr/utils.py's get_client_ip()
       honors X-Real-IP directly once REMOTE_ADDR itself is trusted (which
       127.0.0.1 is, by default). Deliberately X-Real-IP and not
       X-Forwarded-For: get_client_ip()'s XFF handling walks the chain and
       *skips* any hop that's itself in a trusted/private range -- correct
       for its usual reverse-proxy case, but wrong here, since a home-LAN
       viewing device's own IP (e.g. 10.x/192.168.x) is private too and got
       silently skipped as if it were just another internal proxy, leaving
       REMOTE_ADDR (127.0.0.1) as the answer either way -- confirmed live,
       this was exactly why XFF alone didn't work. X-Real-IP has no such
       filtering: it's trusted at face value once REMOTE_ADDR itself is."""
    lines = []

    username = (params.get("username") or "").strip()
    if username:
        try:
            from django.contrib.auth import get_user_model
            from rest_framework_simplejwt.tokens import RefreshToken

            User = get_user_model()
            user = User.objects.get(username=username)
            access_token = str(RefreshToken.for_user(user).access_token)
            lines.append(f"Authorization: Bearer {access_token}\r\n")
        except Exception:
            # Best-effort: a buffer that streams anonymously is still a
            # working buffer. Don't let a bad username block start_buffer.
            logger.exception(
                "timeshift_buffer: couldn't mint a stream-owner token for user %r -- "
                "buffer will stream anonymously (check the caller's Dispatcharr "
                "account still exists)",
                username,
            )

    client_ip = (params.get("client_ip") or "").strip()
    if client_ip:
        try:
            ipaddress.ip_address(client_ip)
        except ValueError:
            # A bare IP is all X-Real-IP is for -- anything else (in
            # particular embedded \r\n, which .strip() above only trims
            # from the ends, not the middle) doesn't belong in an HTTP
            # header value at all. Found via a full-codebase security
            # review (2026-09-10): unvalidated, this let a caller smuggle
            # a second, pipelined request onto ffmpeg's connection to
            # Dispatcharr's own loopback proxy. See docs/TIMESHIFT.md's
            # "client_ip header injection" section for the full writeup.
            logger.warning(
                "timeshift_buffer: rejecting non-IP client_ip value %r "
                "(stream will start without X-Real-IP attribution)",
                client_ip,
            )
        else:
            lines.append(f"X-Real-IP: {client_ip}\r\n")

    return "".join(lines) or None


def _start_ffmpeg(channel_uuid: str, params: dict, settings_dict: dict, logger) -> dict:
    storage_path = settings_dict.get("storage_path", "/data/timeshift")
    segment_seconds = int(settings_dict.get("segment_seconds", 2))
    buffer_minutes = int(settings_dict.get("buffer_minutes", 60))
    base_url = settings_dict.get("internal_base_url", "http://127.0.0.1:9191")
    http_port = int(settings_dict.get("http_port", 9192))

    visible_segments = max(1, (buffer_minutes * 60) // segment_seconds)
    # Wrap (filename reuse) past a larger count than what the playlist
    # advertises as visible, so a client that just requested an old segment
    # has headroom before ffmpeg overwrites that same filename in place --
    # the same class of "don't reveal/rely on something about to move under
    # you" caution this project already applied to its own gradual-cap fix
    # for in-progress-recording playback (see pvr.dispatcharr-unofficial's
    # docs/RECORDINGS.md -- that mechanism has since been replaced by a
    # native-demuxer approach that doesn't need a cap at all, but the same
    # underlying caution still applies here), just via a size margin here
    # instead of a request-count hold.
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
    # segments back-to-back but confirmed live (100% reproduction across
    # forward/backward seeks) to break seeking against an earlier client
    # this plugin routed through (inputstream.ffmpegdirect, since removed --
    # see this file's own top-of-file design notes): its generic
    # av_seek_frame() computes a global target PTS that, with resets, no
    # single segment's local PTS space actually contains. Continuous
    # timestamps across segments (matching how real-world HLS packagers do
    # it) are what a byte-domain seek needs to resolve to a real position at
    # all, which is just as true for pvr.dispatcharr-unofficial's own native-demuxer
    # seeking today -- kept removed for that reason, not merely inherited
    # from the old client's own requirement.
    attribution_headers = _stream_attribution_headers(params, logger)

    cmd = [
        "ffmpeg",
        "-nostdin",
        "-loglevel",
        "warning",
    ]
    if attribution_headers:
        # Must precede -i: ffmpeg applies -headers to the input that
        # follows it, not globally.
        cmd += ["-headers", attribution_headers]
    cmd += [
        "-i",
        _proxy_url(channel_uuid, base_url),
        "-c",
        "copy",
        "-f",
        "segment",
        "-segment_time",
        str(segment_seconds),
        "-segment_wrap",
        str(wrap_segments),
        "-segment_list",
        str(playlist_path),
        "-segment_list_size",
        str(visible_segments),
        "-segment_list_flags",
        "+live",
        "-segment_list_type",
        "m3u8",
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
        proc.pid,
        channel_uuid,
        buffer_minutes,
        segment_seconds,
        visible_segments,
        wrap_segments,
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
    # stop() pattern (track a pid, SIGTERM it, log the outcome). 2s, not the
    # 5s this used to be: confirmed live (pvr.dispatcharr-unofficial's own
    # CloseLiveTimeshiftStream() now waits on this call synchronously, so
    # its own duration is directly what a user feels as "how long does
    # Stop take") that ffmpeg here can take close to the full deadline to
    # exit on SIGTERM alone -- no crash, no error, SIGKILL was never
    # needed, it just isn't prompt about it. Escalating to SIGKILL sooner
    # is safe for this specific pipeline regardless of why: a plain stream
    # copy (-c copy) writing segment files has nothing meaningful to lose
    # from an abrupt kill -- this plugin's own manifest only ever exposes
    # segments ffmpeg has already fully closed (see _get_live_manifest()),
    # so a segment truncated mid-write by SIGKILL was already invisible to
    # every client and gets cleaned up/overwritten normally either way.
    start = time.time()
    deadline = start + 2
    while time.time() < deadline:
        try:
            os.killpg(pid, 0)  # signal 0: check it's still alive, don't actually signal
        except ProcessLookupError:
            logger.debug(
                "timeshift_buffer: ffmpeg pid %s exited %.1fs after SIGTERM",
                pid,
                time.time() - start,
            )
            return
        time.sleep(0.2)

    logger.warning("timeshift_buffer: ffmpeg pid %s didn't exit after SIGTERM, sending SIGKILL", pid)
    with contextlib.suppress(ProcessLookupError):
        os.killpg(pid, signal.SIGKILL)


def _is_process_alive(pid) -> bool:
    """Signal 0 checks a pid's existence without actually signaling it --
    works cross-worker-process (unlike Popen.poll()/os.waitpid(), which only
    work for the process's own parent), same reasoning as _stop_ffmpeg()'s
    own liveness poll above. Only tells you the process is gone, not its
    exit code -- getting that needs being the parent, which this plugin's
    Redis-tracked pid generally isn't (it's whichever WSGI worker process
    happened to handle the original start_buffer call, not necessarily this
    one)."""
    if not pid:
        return False
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except Exception:
        # e.g. PermissionError against a recycled, unrelated pid -- assume
        # alive rather than reap something still running
        return True


class BufferFailedError(RuntimeError):
    """Distinguishes "ffmpeg already exited, this buffer will never produce
    a segment" from a plain RuntimeError's "not ready yet, keep waiting" --
    see _get_live_manifest()'s own comment on why the distinction matters."""


def _remove_channel_files(state: dict, logger):
    # shutil.rmtree rather than a flat glob+unlink+rmdir: simpler, and
    # robust to whatever this directory happens to contain rather than
    # assuming a flat file list.
    try:
        channel_dir = _channel_dir(state["storage_path"], state["channel_uuid"])
    except ValueError:
        # A corrupted/pre-fix Redis entry, not a real channel -- nothing
        # safe to remove. Caller (_teardown_buffer) still proceeds to
        # delete the Redis state itself either way, so this doesn't get
        # stuck retrying the same bad entry forever.
        logger.error(
            "timeshift_buffer: refusing to remove files for invalid channel_uuid %r", state.get("channel_uuid")
        )
        return
    try:
        shutil.rmtree(channel_dir)
    except FileNotFoundError:
        pass
    except OSError:
        logger.exception("timeshift_buffer: couldn't fully clean up %s", channel_dir)


def _teardown_buffer(state: dict, logger):
    """Stops ffmpeg, removes its segment files, and deletes the tracked
    state for a buffer -- the full "this buffer is done" sequence shared
    by the reaper, stop_buffer, a fatal get_live_manifest failure, and
    stop_all."""
    _stop_ffmpeg(state, logger)
    _remove_channel_files(state, logger)
    _delete_buffer_state(state["channel_uuid"])


def _find_orphaned_channel_dirs(storage_path: str, min_age_seconds: int) -> list:
    """Directories directly under storage_path with no matching
    Redis-tracked buffer state, old enough to rule out a buffer that's
    still mid-start.

    Exists because the normal cleanup paths (the reaper's idle-heartbeat
    check, stop_buffer, stop_all) all work by iterating *currently
    Redis-tracked* buffers -- confirmed live that this leaves a real gap:
    a channel directory whose Redis state key is simply gone (expired
    past _BUFFER_STATE_TTL with nothing left to refresh it -- e.g. a
    client killed hard enough that it never sent stop_buffer, and no
    heartbeat arrived again before the TTL ran out -- or a state write
    that never happened at all, e.g. a crash between _start_ffmpeg()
    creating the directory and _set_buffer_state() persisting it) is
    invisible to every one of those, since none of them ever look at
    what's actually sitting in storage_path independent of what Redis
    currently says. This closes that gap by reconciling the filesystem
    against Redis directly, the one place these leaks are actually
    visible from.

    A directory's own mtime changes whenever ffmpeg writes a new segment
    file into it (a new directory entry), so "how long since this
    directory's mtime" is a real idle-since signal for an actively
    written buffer, not just a creation timestamp -- and for a
    freshly-mkdir'd but not-yet-written one, mtime is the creation time
    itself, so min_age_seconds also covers _start_ffmpeg's own narrow
    directory-created-before-state-persisted window.
    """
    root = Path(storage_path)
    if not root.is_dir():
        return []
    now = time.time()
    orphans = []
    for entry in root.iterdir():
        if not entry.is_dir():
            continue
        if _get_buffer_state(entry.name) is not None:
            continue  # tracked -- not an orphan
        try:
            age = now - entry.stat().st_mtime
        except OSError:
            continue
        if age < min_age_seconds:
            continue  # too recent to be sure it isn't just starting up
        orphans.append(entry)
    return orphans


def _scrub_orphaned_dirs(storage_path: str, min_age_seconds: int, logger) -> list:
    removed = []
    for entry in _find_orphaned_channel_dirs(storage_path, min_age_seconds):
        try:
            shutil.rmtree(entry)
            removed.append(entry.name)
        except OSError:
            logger.exception("timeshift_buffer: couldn't scrub orphaned directory %s", entry)
    if removed:
        logger.info(
            "timeshift_buffer: scrubbed %d orphaned buffer director%s: %s",
            len(removed),
            "y" if len(removed) == 1 else "ies",
            ", ".join(removed),
        )
    return removed


_manifest_cache = {}
_manifest_cache_lock = threading.Lock()


def _get_live_manifest(state: dict, logger) -> dict:
    """Builds a byte-addressable manifest of the buffer's currently-listed
    (live.m3u8) segments -- filename, byte size, duration, and cumulative
    byte/time offsets -- so a client can treat the rolling live buffer as
    one growing, seekable byte stream (Range-reading individual segment
    files directly, see _BufferRequestHandler's Range support) instead of
    going through inputstream.ffmpegdirect's HLS-seek machinery, which
    pvr.dispatcharr-unofficial's docs/TIMESHIFT.md documents as confirmed broken for
    this kind of buffer.

    Always reflects the current state of live.m3u8 -- never stale -- but
    doesn't necessarily redo the work of getting there: `_manifest_cache`
    (module-global, per-worker-process, keyed by channel_uuid) skips
    re-parsing the playlist and re-stat()-ing every visible segment when
    nothing has actually changed on disk since this same process last read
    it (checked via the playlist file's own mtime/size -- confirmed cheap
    and sufficient, no need for content hashing), and even when it has
    changed, only stat()s segments genuinely new since last time -- a
    sequence number is never reused for the life of *one buffer instance*
    (HLS media sequence is monotonic, even though -segment_wrap does
    recycle filenames), so a cache hit by sequence is guaranteed to be the
    exact same bytes, the same invariant pvr.dispatcharr-unofficial's own
    RefreshLiveManifest() already relies on client-side -- with two
    deliberate exceptions, both confirmed live to matter, not just
    theoretical:

    - The newest (last-listed) segment on any given call is always
      re-stat()'d even if it matches a cached entry, since a size sampled
      the very first moment a segment becomes visible could race a
      not-yet-fully-flushed write, and unlike every other cached entry,
      that risk can't yet have been disproven by a later segment having
      since appeared after it (see the inline comment where this is
      checked).
    - The *whole cache entry* for a channel_uuid is discarded outright,
      not partially trusted, if `state["access_token"]` (a fresh random
      value per genuine new buffer instance, see _start_buffer's own
      comment) doesn't match what the cache was built from -- "a sequence
      number is never reused" is only true *within* one continuously-
      running buffer instance; a channel stopped and later restarted
      (e.g. a channel switch away and back) gets a brand-new ffmpeg
      process whose live.m3u8 restarts numbering from scratch, reusing
      the exact same (sequence, filename) pairs for completely different,
      unrelated file content. An earlier version of this same check
      compared the buffer's pid instead -- confirmed live that isn't
      good enough: pids get recycled by the OS, and under heavy
      start_buffer/stop_buffer churn (many hours of repeated test
      cycles), a stale cache entry tagged with a pid the OS has since
      reassigned to a genuinely new instance passed the check it should
      have failed. See the inline comment where `buffer_token` is read
      for the live incidents (a channel switched away from and back,
      then a cold open with heavy same-day buffer churn, both "Packet
      corrupt" within seconds) that found and then fully closed this.

    Matters because
    this is called far more often than the buffer could possibly have
    grown -- the addon's own catch-up-to-tail loop and throttled length
    checks call this repeatedly while waiting, not just once per new
    segment -- and at this plugin's default settings (buffer_minutes=60,
    segment_seconds=2) the visible window is 1800 segments, meaning a
    naive rebuild-from-scratch call was up to 1800 stat() syscalls just to
    answer "did anything change". Per-worker rather than Redis-backed:
    avoids adding a second, differently-shaped piece of Redis-persisted
    state alongside the buffer's own lifecycle state, and a cold worker
    (one that's never seen this channel_uuid, or a round-robin request
    landing on a different worker than last time) just falls back to a
    full stat() sweep once, exactly like this function always did before
    -- never worse than the old behavior, only better when it helps.

    The rolling window means "byte offset 0" in THIS response corresponds
    to whatever's currently oldest -- a later call's "byte offset 0" will
    be different content once the window has advanced. A client that wants
    a stable address space across repeated calls (pvr.dispatcharr-unofficial does,
    to avoid its own position bookkeeping going stale mid-playback) can't
    just concatenate offsets naively; each segment also carries an absolute
    `sequence` number (HLS's own #EXT-X-MEDIA-SEQUENCE plus its position in
    the list), which is stable for the life of the buffer regardless of how
    the visible window slides, and is what a client should key its own
    merged/cumulative table on instead of list position. (Byte/time offsets
    themselves are always recomputed from the resolved segment list on
    every call, cache hit or not -- cheap, in-memory-only arithmetic, but
    not themselves cacheable, precisely because they're window-relative.)"""
    channel_uuid = state["channel_uuid"]
    try:
        channel_dir = _channel_dir(state["storage_path"], channel_uuid)
    except ValueError as exc:
        # Matches this function's own established contract (raise
        # RuntimeError for "can't produce a manifest" conditions, already
        # handled by _get_live_manifest_action()'s caller) rather than
        # letting a corrupted/pre-fix Redis entry's ValueError propagate
        # as an unrelated exception type.
        raise RuntimeError(f"invalid channel_uuid in buffer state: {channel_uuid!r}") from exc
    live_playlist_path = channel_dir / "live.m3u8"
    if not live_playlist_path.is_file():
        # Two very different situations produce the identical symptom here
        # -- no playlist yet -- and a caller retrying blindly on either one
        # can't tell them apart: a buffer that's still cold-starting (ffmpeg
        # running, just hasn't finished its first segment_time interval
        # yet) versus one that will *never* produce a playlist because
        # ffmpeg already exited (confirmed live: an upstream provider's own
        # concurrent-stream limit, already fully used by other channels,
        # makes Dispatcharr's live proxy refuse the connection ffmpeg is
        # reading from -- ffmpeg has no -reconnect flag set here, so it
        # just exits rather than retrying forever). Checking whether the
        # tracked pid is still alive distinguishes them cheaply, so a
        # caller (pvr.dispatcharr-unofficial's own OpenLiveTimeshiftStream() cold
        # -start retry loop) can fail fast on the second case instead of
        # retrying for its full ~15s budget against something that will
        # never succeed.
        if not _is_process_alive(state.get("pid")):
            log_tail = ""
            try:
                log_path = channel_dir / "ffmpeg.log"
                lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
                log_tail = " | ".join(lines[-5:])
            except OSError:
                pass
            raise BufferFailedError(
                "ffmpeg exited before producing any segments -- it will not "
                "recover on its own (a provider-side concurrent-stream limit "
                "is the most common cause)" + (f"; last ffmpeg.log lines: {log_tail}" if log_tail else "")
            )
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet")

    try:
        playlist_stat = live_playlist_path.stat()
    except OSError:
        # Translates a low-level race (playlist vanished between the
        # existence check above and this stat()) into the same
        # caller-facing message as that check -- the original OSError
        # adds nothing a caller needs, so deliberately not chained.
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet") from None

    # Ties every cache entry to the specific ffmpeg process (buffer
    # *instance*) it was built from -- confirmed live this matters, not
    # just theoretical: a channel stopped and later restarted (e.g. a
    # channel switch away and back) gets a brand-new ffmpeg process whose
    # live.m3u8 restarts numbering from scratch, so its early segments
    # have the exact same (sequence, filename) pairs as the *previous*
    # buffer instance's -- despite being completely different files with
    # different real content. _delete_buffer_state() clears this same
    # worker process's own cache on a clean stop, but that alone isn't a
    # sufficient guarantee across a multi-worker deployment: the stop_buffer
    # call that tears down the old instance can land on a *different*
    # worker than the one that cached get_live_manifest data for it, in
    # which case that worker's own stale cache never gets cleared at all,
    # and would otherwise be trusted again the moment it next handles a
    # request for the *new* instance of the same channel_uuid -- reusing a
    # completely wrong size for a same-named, same-sequenced, but
    # genuinely different segment (confirmed live: a channel switched away
    # from and back produced exactly this, "Packet corrupt" within
    # seconds of the fresh buffer starting, caught by pvr.dispatcharr-unofficial's
    # own Content-Range cross-check -- see docs/TIMESHIFT.md's "1.0.7
    # follow-up #2").
    #
    # Identified by `access_token`, NOT `pid` -- confirmed live this
    # distinction matters: an earlier version of this fix compared pid,
    # which reproducibly still failed under heavy start_buffer/stop_buffer
    # churn (many hours of repeated test cycles against one channel),
    # because OS pids get recycled -- a stale cache entry tagged with a
    # pid the OS has since reassigned to a genuinely new ffmpeg process
    # passes an equality check it has no business passing. `access_token`
    # (see _start_buffer's own comment -- secrets.token_urlsafe(24),
    # freshly generated for every genuine new buffer instance, never
    # reused for a *different* instance no matter how much churn happens)
    # has none of that risk. A mismatch here is treated exactly like a
    # cold cache, full stop, no partial trust of anything in it.
    buffer_token = state.get("access_token")

    with _manifest_cache_lock:
        cached = _manifest_cache.get(channel_uuid)
        # A missing/empty buffer_token (state predates the access_token
        # feature, or something else went wrong establishing identity)
        # can't be trusted to match anything, itself included -- fails
        # closed to a full cold rebuild rather than risk two different
        # "unknown" instances comparing equal.
        if cached is not None and (not buffer_token or cached.get("instance_token") != buffer_token):
            cached = None
        if (
            cached is not None
            and cached["playlist_mtime_ns"] == playlist_stat.st_mtime_ns
            and cached["playlist_size"] == playlist_stat.st_size
        ):
            # Nothing on disk has changed since our own last read of this
            # exact playlist file -- reuse it outright, no re-parse, no
            # re-stat, not even a re-read of the (small but non-zero) text
            # file. dict insertion order is what supplies list order here
            # (guaranteed since Python 3.7), matching how by_sequence was
            # built below on the call that populated this cache entry.
            media_sequence = cached["media_sequence"]
            ordered = [(seq,) + entry for seq, entry in cached["by_sequence"].items()]
        else:
            lines = live_playlist_path.read_text(encoding="utf-8", errors="replace").splitlines()

            media_sequence = 0
            for line in lines:
                if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
                    with contextlib.suppress(ValueError):
                        media_sequence = int(line[len("#EXT-X-MEDIA-SEQUENCE:") :].strip())
                    break

            # First pass: pure text parsing, no filesystem access yet --
            # just the (sequence, filename, duration_ms) triples in
            # playlist order. Kept separate from size resolution below
            # so that step can tell which entry is the newest one.
            parsed = []
            list_index = 0  # position within the m3u8's own segment list, before any drops
            i = 0
            while i < len(lines):
                line = lines[i]
                if line.startswith("#EXTINF:") and i + 1 < len(lines) and not lines[i + 1].startswith("#"):
                    seg_name = lines[i + 1].strip()
                    sequence = media_sequence + list_index
                    list_index += 1
                    try:
                        # OverflowError caught alongside ValueError, not
                        # just for symmetry: float("inf") parses fine, but
                        # round(inf) raises OverflowError, not ValueError
                        # -- found via the same audit that caught the
                        # analogous gap in recording_edl's _parse_edl (see
                        # that plugin's own docs/RECORDING_EDL.md entry).
                        # A single malformed #EXTINF: line shouldn't fail
                        # the *entire* manifest fetch over one segment's
                        # duration -- ffmpeg is the only realistic writer
                        # of this file and isn't expected to ever emit
                        # "inf", but a parser reading generated content
                        # shouldn't assume that.
                        duration_ms = int(round(float(line[len("#EXTINF:") :].rstrip(",")) * 1000))
                    except (ValueError, OverflowError):
                        duration_ms = 0
                    parsed.append((sequence, seg_name, duration_ms))
                    i += 2
                else:
                    i += 1

            old_by_sequence = cached["by_sequence"] if cached else {}
            new_by_sequence = {}
            ordered = []
            newest_index = len(parsed) - 1
            for idx, (sequence, seg_name, duration_ms) in enumerate(parsed):
                # A sequence number is never reused for the life of a
                # buffer, so a hit here (same sequence, same filename) is
                # guaranteed to be the exact same bytes -- see this
                # function's own docstring -- with one deliberate
                # exception: the newest (last-listed) segment is never
                # trusted from cache, even on a match. ffmpeg only adds a
                # segment to the playlist once it's done writing it, but
                # confirming that "done" is visible to a stat() from a
                # separate process, on every filesystem, the *instant* the
                # entry first appears, isn't something this function
                # should assume -- a size sampled on that very first call
                # could plausibly race a not-yet-fully-flushed write.
                # Before this cache existed, that risk was harmless: the
                # *next* manifest call (of which pvr.dispatcharr-unofficial's own
                # cold-start retry loop issues several before ever reading
                # a byte) would simply re-stat and self-correct. Caching
                # turned a harmless, self-healing transient into a size
                # that, once wrong, stayed wrong for the rest of that
                # segment's time in the window -- exactly the shape of a
                # real, reproducible corrupt-playback report on a freshly
                # opened live-timeshift stream, which starts right at the
                # live edge (see pvr.dispatcharr-unofficial's own
                # kLiveEdgeMarginSegments) where the newest segment is
                # most likely to still be this fresh. Re-verifying just
                # the one newest entry costs at most one extra stat() per
                # call with something new -- in the common case (exactly
                # one new segment since last call) it costs nothing extra
                # at all, since that segment wasn't cached yet anyway.
                reusable = None if idx == newest_index else old_by_sequence.get(sequence)
                if reusable is not None and reusable[0] == seg_name:
                    size = reusable[1]
                else:
                    try:
                        size = (channel_dir / seg_name).stat().st_size
                    except OSError:
                        # Recycled (by the live buffer's own
                        # -segment_wrap) between the playlist listing it
                        # and this stat -- drop it rather than fail the
                        # whole manifest over one segment (sequence
                        # numbers for surviving entries are unaffected,
                        # since they were assigned from list position
                        # above, not from what survives here).
                        continue

                entry = (seg_name, size, duration_ms)
                new_by_sequence[sequence] = entry
                ordered.append((sequence,) + entry)

            _manifest_cache[channel_uuid] = {
                "instance_token": buffer_token,
                "playlist_mtime_ns": playlist_stat.st_mtime_ns,
                "playlist_size": playlist_stat.st_size,
                "media_sequence": media_sequence,
                "by_sequence": new_by_sequence,
            }

    if not ordered:
        raise RuntimeError("no segments currently available -- the buffer may be too new")

    # Byte/time offsets are always recomputed here, cache hit or not --
    # cheap, in-memory-only arithmetic, but not themselves cacheable, since
    # they're window-relative (see this function's own docstring).
    segments = []
    cumulative_bytes = 0
    cumulative_ms = 0
    for sequence, seg_name, size, duration_ms in ordered:
        segments.append(
            {
                "filename": seg_name,
                "sequence": sequence,
                "byte_offset": cumulative_bytes,
                "byte_size": size,
                "time_offset_ms": cumulative_ms,
                "duration_ms": duration_ms,
            }
        )
        cumulative_bytes += size
        cumulative_ms += duration_ms

    return {
        "media_sequence": media_sequence,
        "segments": segments,
        "total_bytes": cumulative_bytes,
        "total_duration_ms": cumulative_ms,
    }


def _prune_stale_viewers(state, idle_timeout, now=None):
    """Drops any viewer_id whose own last-seen heartbeat is older than
    idle_timeout, in place on state["viewers"]/state["viewer_heartbeats"].
    Returns True if anything was actually pruned.

    Needed because last_heartbeat (refreshed by ANY successful file fetch,
    see _touch_heartbeat) is buffer-wide, not per-viewer: if one viewer
    crashes hard enough to never call stop_buffer, its viewer_id otherwise
    stays in state["viewers"] forever, kept "alive" by other viewers'
    ordinary segment fetches. That phantom entry then blocks stop_buffer's
    reference count from ever reaching zero once the real remaining
    viewers actually do stop -- the buffer (and the provider slot it
    holds) never gets torn down. A viewer with no recorded heartbeat yet
    (older plugin-version state from before this field existed, or a
    start_buffer call that raced this exact instant) is treated as fresh
    as of `now`, not as already stale, so an in-progress upgrade doesn't
    mass-prune viewers that just haven't had a chance to report in yet.
    """
    now = now if now is not None else time.time()
    viewers = state.get("viewers", [])
    if not viewers:
        return False
    heartbeats = state.get("viewer_heartbeats", {})
    fresh = [v for v in viewers if now - heartbeats.get(v, now) <= idle_timeout]
    if len(fresh) == len(viewers):
        return False
    state["viewers"] = fresh
    state["viewer_heartbeats"] = {v: heartbeats[v] for v in fresh if v in heartbeats}
    return True


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
                settings_dict = settings_getter()
                idle_timeout = int(settings_dict.get("idle_timeout_seconds", 30))
                now = time.time()
                for state in _iter_buffer_states():
                    if _prune_stale_viewers(state, idle_timeout, now):
                        logger.info(
                            "timeshift_buffer: pruned stale viewer(s) for channel %s (no heartbeat for %ds)",
                            state["channel_uuid"],
                            idle_timeout,
                        )
                        _set_buffer_state(state["channel_uuid"], state)
                    if now - state.get("last_heartbeat", 0) > idle_timeout:
                        logger.info(
                            "timeshift_buffer: reaping idle buffer for channel %s (no heartbeat for %ds)",
                            state["channel_uuid"],
                            int(now - state.get("last_heartbeat", 0)),
                        )
                        _teardown_buffer(state, logger)

                # Reconciles storage_path against Redis directly, catching
                # the class of leak the loop above structurally can't (see
                # _find_orphaned_channel_dirs' own comment) -- makes
                # scrub_orphaned_buffers a manual-cleanup convenience
                # rather than the only way this ever gets fixed. Same
                # min-age floor reasoning as that action's own default:
                # at least 5 minutes regardless of a shorter
                # idle_timeout_seconds, since there's no tracked state
                # here to double-check against before deleting.
                storage_path = settings_dict.get("storage_path", "/data/timeshift")
                _scrub_orphaned_dirs(storage_path, max(idle_timeout, 300), logger)
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
    version = "0.6.2"
    description = (
        "Server-side rolling live-TV buffer per channel, so clients can "
        "pause/rewind live playback without a local on-device buffer."
    )
    author = "BruiserBrody17"
    help_url = (
        "https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/tree/master/dispatcharr-plugin/timeshift_buffer"
    )

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
            "id": "about",
            "label": "About",
            "type": "info",
            "description": (
                "Started/stopped per channel by a client (e.g. "
                "pvr.dispatcharr-unofficial's live-timeshift setting) via the plugin "
                "run/ API, not usually by hand. The buttons below are for "
                "manual testing and emergency cleanup."
            ),
        },
        {
            "id": "storage_path",
            "label": "Buffer storage path",
            "type": "string",
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
            "id": "buffer_minutes",
            "label": "Buffer length (minutes)",
            "type": "number",
            "default": 60,
            "help_text": (
                "How far back a viewer can rewind. Drives both "
                "segment_list_size (what the playlist advertises) and "
                "segment_wrap (when old segment files get reused)."
            ),
        },
        {
            "id": "segment_seconds",
            "label": "Segment length (seconds)",
            "type": "number",
            "default": 2,
            "help_text": (
                "ffmpeg -segment_time. A client only sees new content once a "
                "segment closes, so shorter segments mean less stalling/"
                "rebuffering during ordinary playback, at the cost of more, "
                "smaller files on disk and more requests to this plugin's own "
                "file server. Confirmed live at the default (2s) with 4 "
                "channels buffering concurrently -- steady, error-free segment "
                "production throughout, see the addon's own docs/TIMESHIFT.md "
                "for the full account."
            ),
        },
        {
            "id": "idle_timeout_seconds",
            "label": "Idle timeout (seconds)",
            "type": "number",
            "default": 30,
            "help_text": (
                "Stops a channel's buffer if no heartbeat arrives for this "
                "long -- only a backstop for a client that disappears without "
                "cleanly closing (a crash, a network drop), not a normal "
                "Stop, which tears the buffer down immediately regardless. "
                "Kept short so an abandoned buffer doesn't occupy one of a "
                "provider's concurrent-stream slots for long. See "
                "pvr.dispatcharr-unofficial's docs/TIMESHIFT.md for details."
            ),
        },
        {
            "id": "max_concurrent_buffers",
            "label": "Max concurrent channel buffers",
            "type": "number",
            "default": 4,
            "help_text": "Safety cap -- each active buffer is a real ffmpeg process plus continuous disk writes.",
        },
        {
            "id": "internal_base_url",
            "label": "Internal base URL",
            "type": "string",
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
            "id": "http_port",
            "label": "Buffer server port",
            "type": "number",
            "default": 9192,
            "help_text": (
                "Port this plugin's own file server listens on (playlists "
                "and segments are served directly by the plugin, not "
                "through Dispatcharr's normal web port -- confirmed live "
                "that Dispatcharr's /media/ static route is unreachable in "
                "this deployment mode, see plugin.py's module docstring). "
                "Every request needs a per-buffer access token, issued only "
                "via the authenticated start_buffer action -- reachable "
                "doesn't mean readable without one. Still must be mapped "
                "through your container config the same way 9191 already "
                "is, or clients outside the container can't reach it."
            ),
        },
        {
            "id": "test_channel_uuid",
            "label": "Test channel UUID",
            "type": "string",
            "default": "",
            "help_text": (
                "Only used by the manual-test buttons below (plugin action "
                "buttons can't take click-time input) -- paste a channel's "
                "UUID here, save, then use Start/Stop Test Buffer. The real "
                "integration (a client calling run/ over the REST API) "
                "passes channel_uuid directly and ignores this field."
            ),
        },
    ]

    actions = [
        {
            "id": "start_buffer",
            "label": "Start Buffer (manual test)",
            "description": "Starts a rolling buffer for a channel. Params: channel_uuid (required).",
            "button_label": "Start Test Buffer",
        },
        {
            "id": "stop_buffer",
            "label": "Stop Buffer",
            "description": "Stops a channel's buffer and removes its segment files. Params: channel_uuid (required).",
            "button_label": "Stop Test Buffer",
            "confirm": {
                "required": True,
                "title": "Stop buffer?",
                "message": "This ends the rolling buffer for the given channel and deletes its segment files.",
            },
        },
        {
            "id": "heartbeat",
            "label": "Heartbeat",
            "description": (
                "Refreshes a channel's idle timeout. Params: channel_uuid (required), viewer_id "
                "(optional). The buffer-wide timeout is refreshed by any file fetch regardless -- "
                "pass viewer_id to also refresh that specific viewer's own last-seen time, which is "
                "what lets stop_buffer tell a still-watching viewer apart from one that crashed "
                "without ever calling stop_buffer (see plugin.py's _prune_stale_viewers). A client "
                "with viewer_id lifecycle (start_buffer/stop_buffer) should call this on an interval "
                "well under idle_timeout_seconds."
            ),
        },
        {
            "id": "get_live_manifest",
            "label": "Get Live Manifest (manual test)",
            "description": (
                "Returns a byte-addressable manifest (segment filenames, byte sizes, durations, "
                "cumulative offsets) of the buffer's currently-listed segments (params: channel_uuid, "
                "required -- start_buffer must already be running). Used by pvr.dispatcharr-unofficial to treat "
                "the rolling live buffer as one growing, seekable byte stream via Range reads against "
                "individual segments, instead of routing through inputstream.ffmpegdirect's HLS-seek "
                "path (confirmed broken for this kind of buffer, see docs/TIMESHIFT.md)."
            ),
            "button_label": "Get Test Manifest",
        },
        {
            "id": "list_buffers",
            "label": "List Active Buffers",
            "description": (
                "Shows every currently-running buffer and its age, as a "
                "one-line summary in the result notification (Dispatcharr's "
                "own Plugins page has no other way to display an action's "
                "result data)."
            ),
            "button_label": "Refresh List",
        },
        {
            "id": "stop_all",
            "label": "Stop All Buffers",
            "description": "Emergency cleanup: stops every active buffer and removes all segment files.",
            "button_label": "Stop Everything",
            "button_variant": "filled",
            "button_color": "red",
            "confirm": {
                "required": True,
                "title": "Stop all buffers?",
                "message": "This ends every active rolling buffer right now, for every channel and "
                "every viewer currently using one.",
            },
        },
        {
            "id": "scrub_orphaned_buffers",
            "label": "Scrub Orphaned Buffer Directories",
            "description": (
                "Removes leftover directories under storage_path that Redis no longer has any "
                "record of (a client killed hard enough that it never sent stop_buffer, and no "
                "heartbeat arrived again before the tracked state's own TTL expired, is the usual "
                "cause) -- the reaper above already does this automatically on every tick, so this "
                "is mainly for cleaning up right now rather than waiting for the next one. Only "
                "touches directories untouched for several minutes; anything that could still be "
                "an actively-starting buffer is left alone."
            ),
            "button_label": "Scrub Now",
            "confirm": {
                "required": True,
                "title": "Scrub orphaned directories?",
                "message": "Permanently deletes any buffer directory under storage_path with no "
                "matching tracked state and no recent activity. Does not touch anything currently "
                "active.",
            },
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
        if action == "get_live_manifest":
            return self._get_live_manifest_action(params, settings_dict, logger)
        if action == "list_buffers":
            return self._list_buffers()
        if action == "stop_all":
            return self._stop_all(logger)
        if action == "scrub_orphaned_buffers":
            return self._scrub_orphaned_buffers(settings_dict, logger)

        return {"status": "error", "message": f"Unknown action: {action}"}

    def stop(self, context: dict):
        """Called when the plugin is disabled, deleted, or reloaded."""
        logger = context.get("logger")
        if _reaper_stop_event is not None:
            _reaper_stop_event.set()
        self._stop_all(logger)
        # Also scrub anything already-orphaned at the moment of teardown --
        # _stop_all() above only touches what's still Redis-tracked, so
        # without this a disable/delete would leave existing orphans behind
        # rather than actually cleaning storage_path out.
        self._scrub_orphaned_buffers(context.get("settings", {}), logger)
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
        raw = params.get("channel_uuid") or settings_dict.get("test_channel_uuid")
        if not raw:
            return None
        # Rejects anything that isn't a real UUID before it ever reaches
        # _channel_dir() -- every action below treats a None return here
        # identically to a missing channel_uuid (their existing "channel_uuid
        # is required" check), so a caller-supplied value like
        # "../recordings" is refused with the same ordinary error response
        # rather than being used to build a filesystem path. Dispatcharr's
        # own channel uuid field is a real UUID, so a non-UUID-shaped value
        # has no legitimate reason to reach here at all.
        try:
            uuid.UUID(str(raw))
        except (ValueError, AttributeError, TypeError):
            return None
        return raw

    def _start_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (pass it as a param, or paste one into "
                "the test_channel_uuid setting for manual testing)",
            }

        # Registers this caller as one of the buffer's viewers (a plain
        # list, not a set -- state is round-tripped through Redis as JSON,
        # which has no native set type). Optional and best-effort: a caller
        # that doesn't pass one (an older addon version, or a manual click
        # of this plugin's own "Start Test Buffer" button, which calls
        # run() with empty params) just doesn't participate in reference
        # counting -- see _stop_buffer()'s own comment for exactly what
        # that degrades to.
        viewer_id = params.get("viewer_id")

        existing = _get_buffer_state(channel_uuid)
        # Confirmed live this check matters, not just theoretical: a
        # buffer whose ffmpeg already died (see _get_live_manifest()'s
        # own comment -- e.g. a provider-side concurrent-stream limit
        # refusing the connection) otherwise stayed "existing" forever.
        # Every future start_buffer for the same channel would keep
        # reattaching to it (refreshing last_heartbeat below), which
        # both kept reporting false success to callers and kept the
        # idle-timeout reaper from ever reaping a buffer that will
        # never produce anything -- a permanently zombied channel until
        # someone noticed and called stop_buffer by hand. Treat a dead
        # process exactly like "no buffer exists" instead: clean up its
        # stale state and fall through to a genuinely fresh start.
        if existing and not _is_process_alive(existing.get("pid")):
            logger.warning(
                "timeshift_buffer: start_buffer found a dead buffer for %s (pid %s no longer running) -- "
                "cleaning up and starting fresh instead of reattaching",
                channel_uuid,
                existing.get("pid"),
            )
            _remove_channel_files(existing, logger)
            _delete_buffer_state(channel_uuid)
            existing = None

        if existing:
            existing["last_heartbeat"] = time.time()
            if viewer_id:
                viewers = existing.setdefault("viewers", [])
                if viewer_id not in viewers:
                    viewers.append(viewer_id)
                existing.setdefault("viewer_heartbeats", {})[viewer_id] = time.time()
            # Retrofits a token onto state left behind by a plugin version
            # older than the access-token requirement (see
            # _check_access_token) -- makes this self-healing across an
            # upgrade instead of leaving a pre-existing buffer permanently
            # unreachable (nobody could ever produce a token matching
            # "none stored").
            if "access_token" not in existing:
                existing["access_token"] = secrets.token_urlsafe(24)
            _set_buffer_state(channel_uuid, existing)
            return {
                "status": "ok",
                "message": f"Reattached to already-running buffer ({len(existing.get('viewers', []))} viewer(s))",
                "http_port": existing["http_port"],
                "playlist_route": existing["playlist_route"],
                "already_running": True,
                "access_token": existing["access_token"],
            }

        max_concurrent = int(settings_dict.get("max_concurrent_buffers", 4))
        if len(_list_buffer_keys()) >= max_concurrent:
            return {
                "status": "error",
                "message": f"Already at max_concurrent_buffers ({max_concurrent})",
            }

        try:
            state = _start_ffmpeg(channel_uuid, params, settings_dict, logger)
        except FileNotFoundError:
            return {"status": "error", "message": "ffmpeg not found in this container"}
        except Exception as exc:
            logger.exception("timeshift_buffer: failed to start buffer for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        state["viewers"] = [viewer_id] if viewer_id else []
        state["viewer_heartbeats"] = {viewer_id: time.time()} if viewer_id else {}
        # Required by every request this buffer's own file server serves
        # from here on -- see _check_access_token's own comment for why.
        # token_urlsafe() output is already safe to place directly in a
        # URL query string (no escaping needed).
        state["access_token"] = secrets.token_urlsafe(24)
        _set_buffer_state(channel_uuid, state)
        return {
            "status": "ok",
            "message": f"Started a new buffer for channel {channel_uuid}",
            "http_port": state["http_port"],
            "playlist_route": state["playlist_route"],
            "already_running": False,
            "access_token": state["access_token"],
        }

    def _stop_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "ok", "message": "no buffer was running"}

        # Reference-counted stop, not unconditional -- confirmed live this
        # matters: an earlier version of this addon called stop_buffer
        # unconditionally on every Close(), which killed a second viewer's
        # buffer the moment a first viewer also stopped watching (see
        # docs/TIMESHIFT.md's "Concurrent viewers" section in the addon
        # repo). A caller identifies itself via viewer_id (registered by
        # start_buffer above); this only removes *that* viewer from the
        # buffer's own tracked list; the underlying ffmpeg process is only
        # actually stopped once the list is empty. A caller with no
        # viewer_id (an older addon version, or this plugin's own manual
        # "Stop Test Buffer" button) can't be tracked at all, so it always
        # falls through to the unconditional stop below -- the same
        # behavior this action has always had for such callers, not a
        # regression, since there was never a way to reference-count them.
        viewers = state.get("viewers", [])
        viewer_id = params.get("viewer_id")
        if viewer_id:
            if viewer_id in viewers:
                viewers.remove(viewer_id)
                state.get("viewer_heartbeats", {}).pop(viewer_id, None)
            # Drops any OTHER viewer_id that's gone stale (crashed without
            # ever calling stop_buffer) before deciding whether the buffer
            # is genuinely still in use -- see _prune_stale_viewers' own
            # comment. Without this, a single leftover phantom viewer_id
            # would keep this buffer (and the provider slot it holds)
            # alive forever after the last real viewer cleanly stops.
            idle_timeout = int(settings_dict.get("idle_timeout_seconds", 30))
            _prune_stale_viewers(state, idle_timeout)
            viewers = state.get("viewers", [])
            if viewers:
                state["viewers"] = viewers
                _set_buffer_state(channel_uuid, state)
                return {
                    "status": "ok",
                    "message": "viewer removed; buffer still active for other viewers",
                    "remaining_viewers": len(viewers),
                }

        _teardown_buffer(state, logger)
        return {"status": "ok", "message": "Buffer stopped"}

    def _heartbeat(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "error", "message": "no buffer running for this channel"}

        now = time.time()
        state["last_heartbeat"] = now
        # An explicit per-viewer heartbeat (viewer_id passed alongside
        # channel_uuid) is what lets _prune_stale_viewers tell a genuinely
        # still-watching viewer apart from a crashed one -- see that
        # function's own comment. Only recorded for a viewer_id this
        # buffer already knows about (start_buffer registers it first).
        viewer_id = params.get("viewer_id")
        if viewer_id and viewer_id in state.get("viewers", []):
            state.setdefault("viewer_heartbeats", {})[viewer_id] = now
        _set_buffer_state(channel_uuid, state)
        return {"status": "ok", "message": f"Heartbeat refreshed for channel {channel_uuid}"}

    def _get_live_manifest_action(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "error", "message": "no buffer running for this channel -- call start_buffer first"}

        try:
            manifest = _get_live_manifest(state, logger)
        except BufferFailedError as exc:
            # Caught ahead of the plain RuntimeError branch below (it's a
            # subclass) -- `fatal: true` is what lets a caller (this
            # addon's own OpenLiveTimeshiftStream() cold-start retry loop)
            # stop retrying immediately instead of waiting out its full
            # budget against a buffer that will never produce a segment.
            # Also self-heals here rather than waiting for the next
            # start_buffer call to notice (see that method's own comment):
            # nothing else will proactively clean this up otherwise, since
            # a dead buffer with no further fetches never goes idle either.
            _teardown_buffer(state, logger)
            return {"status": "error", "fatal": True, "message": str(exc)}
        except RuntimeError as exc:
            return {"status": "error", "message": str(exc)}
        except Exception as exc:
            logger.exception("timeshift_buffer: manifest build failed for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        # Asking for the manifest is itself a sign this buffer is actively
        # being watched.
        state["last_heartbeat"] = time.time()
        _set_buffer_state(channel_uuid, state)

        return {
            "status": "ok",
            "message": (
                f"{len(manifest['segments'])} segment(s), {manifest['total_bytes']} bytes, "
                f"{manifest['total_duration_ms'] / 1000:.1f}s buffered"
            ),
            "http_port": state["http_port"],
            "channel_uuid": channel_uuid,
            "segment_route_prefix": f"/{channel_uuid}/",
            **manifest,
        }

    def _list_buffers(self):
        buffers = []
        now = time.time()
        for state in _iter_buffer_states():
            buffers.append(
                {
                    "channel_uuid": state["channel_uuid"],
                    "age_seconds": int(now - state.get("started_at", now)),
                    "idle_seconds": int(now - state.get("last_heartbeat", now)),
                    "http_port": state.get("http_port"),
                    "playlist_route": state.get("playlist_route"),
                    # Reference-counted viewers (see _start_buffer()/_stop_buffer()'s
                    # own comments) -- diagnostic only, not itself load-bearing for
                    # cleanup: a caller with no viewer_id is simply never counted
                    # here even while it's genuinely watching.
                    "viewers": len(state.get("viewers", [])),
                }
            )

        # A human-readable summary, not just the raw buffers list above --
        # Dispatcharr's own Plugins page shows an action's "message" field
        # directly in its result toast (confirmed by reading its frontend,
        # PluginCard.jsx's handlePluginRun()), which is the *only* place any
        # of this action's result is ever actually visible in that UI. The
        # raw "buffers" array above is still returned for a caller that
        # wants the real data (this addon's own diagnostics, or a direct
        # API call), just not something that UI can render on its own.
        if not buffers:
            message = "No active buffers"
        else:
            parts = [f"{b['channel_uuid'][:8]} ({b['viewers']} viewer(s), {b['age_seconds']}s old)" for b in buffers]
            message = f"{len(buffers)} active buffer(s): " + ", ".join(parts)

        return {"status": "ok", "message": message, "buffers": buffers}

    def _stop_all(self, logger):
        stopped = []
        for state in _iter_buffer_states():
            _teardown_buffer(state, logger)
            stopped.append(state["channel_uuid"])
        message = "No buffers were running" if not stopped else f"Stopped {len(stopped)} buffer(s)"
        return {"status": "ok", "message": message, "stopped": stopped}

    def _scrub_orphaned_buffers(self, settings_dict, logger):
        storage_path = settings_dict.get("storage_path", "/data/timeshift")
        idle_timeout = int(settings_dict.get("idle_timeout_seconds", 30))
        # Same floor as the reaper's own automatic pass -- see
        # _find_orphaned_channel_dirs' comment on why an orphan (no
        # tracked state to double-check against) gets more margin than
        # ordinary heartbeat-based reaping.
        removed = _scrub_orphaned_dirs(storage_path, max(idle_timeout, 300), logger)
        message = (
            "No orphaned directories found"
            if not removed
            else f"Removed {len(removed)} orphaned director{'y' if len(removed) == 1 else 'ies'}"
        )
        return {"status": "ok", "message": message, "removed": removed}
