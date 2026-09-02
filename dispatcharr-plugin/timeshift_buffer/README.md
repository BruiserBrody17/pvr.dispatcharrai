# Timeshift Buffer (Dispatcharr plugin)

Server-side rolling live-TV buffer per channel, held by Dispatcharr itself
-- an alternative to `pvr.dispatcharrai`'s local (`inputstream.ffmpegdirect`
`stream_mode: timeshift`) buffering, which lives on the Kodi device instead.

**Status: confirmed live, end-to-end, including real seeking.**
`start_buffer`'s live playlist itself still can't be seeked directly
(`canseek: false` if you open it as a plain HLS URL -- Kodi's PVR layer
requires a known, finite duration, and the live playlist is deliberately
`Duration: N/A` so it can keep tailing new content). Two earlier
approaches to work around that -- a `snapshot_buffer` freeze-and-seek
(the "Instant replay from buffer" workflow) and a `-reset_timestamps`
fix attempt for direct live seeking -- were both built, tested live, and
found genuinely broken (`inputstream.ffmpegdirect`'s generic HLS seek
path failing 100% of the time; full trace in `pvr.dispatcharrai`'s
`docs/TIMESHIFT.md` if you want the history).

**The actual fix was architectural, not another seek-path patch:**
`pvr.dispatcharrai` no longer opens this buffer through
`inputstream.ffmpegdirect`/a `STREAMURL` at all. It exposes the buffer via
Kodi's own `OpenLiveStream`/`ReadLiveStream`/`SeekLiveStream` PVR API
instead, so Kodi's *native* internal demuxer handles MPEG-TS parsing and
seek refinement -- the same mechanism that's always worked reliably for
this addon's completed-recording playback. This plugin's role in that:
`get_live_manifest` (below) and Range support in the file server, so the
addon can treat the rolling buffer as one growing, byte-seekable stream
instead of an HLS playlist. **Confirmed live**: real pause/rewind/
fast-forward/live-follow from plain Play on a real channel, including a
95-second rewind spanning several manifest refreshes. `snapshot_buffer`
is left in this plugin as a standalone action (still works, still a
legitimate capability), but `pvr.dispatcharrai` itself no longer uses it
-- plain Play now gets everything that workflow offered and more.

## How it works

1. `start_buffer` (`{"channel_uuid": "..."}`) launches `ffmpeg` reading
   from Dispatcharr's own live proxy (`/proxy/ts/stream/<uuid>` -- the same
   URL any viewer uses, so it shares Dispatcharr's existing
   single-upstream-connection-per-channel behavior rather than opening a
   second one), writing fixed-length `.ts` segments and a sliding-window
   `.m3u8` playlist via ffmpeg's own `-f segment` muxer
   (`-segment_wrap`/`-segment_list_size` do the rolling-window/old-segment-
   recycling work natively -- no separate Python trimming loop needed for
   that). **Confirmed live.**
2. Segment files live under `storage_path` (default `/data/timeshift`),
   **not** Django's `MEDIA_ROOT` -- confirmed against Dispatcharr's own
   `docker-compose.yml` that `MEDIA_ROOT` isn't under the one volume
   (`./data:/data`) actually bind-mounted, so writing there directly
   wouldn't survive a container recreate and couldn't be redirected to real
   storage.
3. **Serving**: the first design symlinked `MEDIA_ROOT/timeshift ->
   storage_path`, relying on Django's already-registered `static(MEDIA_URL,
   ...)` route. Confirmed live this doesn't work -- fetching a file under
   `/media/timeshift/...` returned Dispatcharr's own React app shell
   instead. Reading the *complete* `dispatcharr/urls.py` (not just a grep)
   explained why: the catch-all SPA route
   (`path("<path:unused_path>", TemplateView...)`) is concatenated *before*
   the appended `static()` patterns, and Django tries patterns in list
   order, so the catch-all always wins for any `/media/...` request. This
   looks like a real routing bug/footgun in Dispatcharr itself, not
   something fixable from a plugin. Since plugins also can't register their
   own URL routes (confirmed via `apps/plugins/loader.py` -- no such hook
   exists), the plugin instead runs its own minimal HTTP server
   (`http.server.ThreadingHTTPServer`, stdlib only) bound to `http_port`
   (default `9192`), serving `storage_path` directly. **This port needs to
   be exposed through your container config**, the same way `9191` already
   is -- the one real infrastructure requirement beyond installing the
   plugin. **Confirmed live**: a real channel opened, played, correct
   video/audio streams, no errors.
4. **Idle-timeout liveness comes from the HTTP server itself**, not a
   client explicitly calling `heartbeat`: every successful file fetch
   (playlist or segment) refreshes `last_heartbeat`. This matters because a
   Kodi PVR addon using plain `STREAMURL` passthrough for live channels
   gets no callback at all for "the user stopped watching" -- but
   `inputstream.ffmpegdirect` re-fetches a live `.m3u8` on an interval for
   as long as playback continues and simply stops once it doesn't, so the
   request stream to this server already *is* the liveness signal. A
   background reaper thread (leader-elected across worker processes via
   Redis) stops and cleans up any buffer whose `last_heartbeat` goes stale
   past `idle_timeout_seconds` -- covers a client crashing or losing
   network before it could otherwise signal it's done.
5. `snapshot_buffer` (`{"channel_uuid": "..."}`, requires `start_buffer`
   already running for that channel) **copies** the buffer's
   currently-listed segment files into a separate, non-recycled `snapshot/`
   subdirectory and writes an `ENDLIST`-terminated playlist referencing the
   copies -- a real, finite window into what's been buffered so far. Copies
   rather than just re-listing the live files in a different shape because
   the live buffer's own `-segment_wrap` keeps recycling those original
   files in the background for as long as it keeps running, which would
   risk a segment getting overwritten while a client watching the "frozen"
   snapshot still had it queued up. The live buffer itself keeps recording
   in the background after a snapshot is taken -- only the snapshot's own
   copied files are frozen. **Confirmed live**: playback of a snapshot
   works correctly as a real, seekable file (`canseek: true` is reported).
   Not used by `pvr.dispatcharrai` itself anymore (see the status note
   above), kept as a standalone action.
6. `get_live_manifest` (`{"channel_uuid": "..."}`, requires `start_buffer`
   already running) is what `pvr.dispatcharrai` actually uses for real
   live seeking: returns the buffer's currently-listed segments (filename,
   byte size, duration) plus a `media_sequence`/per-segment `sequence`
   derived from HLS's own `#EXT-X-MEDIA-SEQUENCE`. That sequence number is
   the point -- the rolling window means a fresh fetch's own byte/time
   offsets shift to newer content over time (segment 0 today isn't segment
   0 an hour from now), so a client that wants a *stable* address space
   across repeated calls needs to merge by sequence, not by re-deriving
   offsets fresh each time. Read live from `live.m3u8` + `os.path.getsize()`
   on every call (not cached), same recycling-race handling as
   `_create_snapshot`.
7. The file server's `do_GET` supports HTTP Range requests (`Range:
   bytes=X-Y`, standard 206/`Content-Range` handling) against any file
   under `storage_path`, not just whole-file GETs -- what actually lets a
   client read a growing buffer as a byte-seekable stream: `get_live_manifest`
   says which segment covers a given byte range, and a Range GET against
   that one segment file returns exactly the bytes needed. **Confirmed
   both locally (a standalone unit-style test of the handler) and live**
   (206 responses with correct `Content-Range` against a real running
   buffer).

## Installing

1. Copy this directory to `data/plugins/timeshift_buffer/` on the host (or
   `/app/data/plugins/timeshift_buffer/` inside the container), matching
   however you already reach the `data/` directory Dispatcharr's compose
   file bind-mounts. (A `.zip` of just `plugin.json`/`plugin.py`/`README.md`
   uploaded via the Plugins page's Import button also works -- **the
   registry key Dispatcharr uses is the folder name inside the zip**, so
   make sure it extracts to a folder literally named `timeshift_buffer`,
   not something generic; a mismatch here surfaces as a plain 404 "Plugin
   not found" on every call, confirmed live.)
2. In Dispatcharr's UI, open the Plugins page, click refresh, enable
   "Timeshift Buffer" (accept the trust-warning modal -- this plugin runs
   arbitrary server-side code, same as any other).
3. **Set `storage_path` to somewhere on real, persistent storage** before
   actually using it -- if you're on Unraid and already redirect
   `/data/recordings` to a share via a container Path config, add a similar
   mapping for whatever path you set here (default `/data/timeshift`). Left
   pointed at the container's own unmapped filesystem, continuous rolling
   writes will fill up whatever's backing that (often a small/fast appdata
   or cache volume) fast.
4. **Map `http_port` (default `9192`) through your container config**, the
   same way `9191` already is -- without this, the plugin's own file server
   is only reachable from inside the container.
5. Check `internal_base_url` matches how this plugin can actually reach
   Dispatcharr's own web service from inside the container -- unverified
   default is `http://127.0.0.1:9191`.
6. **The account calling `start_buffer`/etc. must be a Dispatcharr admin
   account** (`user_level >= 10`) -- confirmed against
   `apps/accounts/permissions.py` that the plugin run endpoint requires
   `IsAdmin` for POST, not just any authenticated user. `pvr.dispatcharrai`
   surfaces this as a clear log line if it's wrong, rather than a bare 403.
7. Nothing to configure for correct Stats-screen attribution -- `start_buffer`
   now takes optional `username`/`client_ip` params (`pvr.dispatcharrai`
   already passes both: the Dispatcharr account it's configured with, and
   the local IP it reaches Dispatcharr through) and uses them to make the
   buffer's ffmpeg connection show the real user and device instead of
   "Anonymous"/`127.0.0.1`. See `_stream_attribution_headers()`'s own
   docstring in `plugin.py` for the two independent Dispatcharr-core
   mechanisms this relies on (`stream_ts()`'s DRF/JWT auth, and
   `get_client_ip()`'s trust of `X-Forwarded-For` from loopback peers). A
   different client calling `start_buffer` directly (not through
   `pvr.dispatcharrai`) can pass its own values for the same effect, or
   omit them to keep the previous anonymous/`127.0.0.1` behavior.

## Testing manually

Use the "Start Test Buffer" / "Take Test Snapshot" / "Get Test Manifest" /
"List Active Buffers" / "Stop Test Buffer" buttons on the Plugins page
(paste a channel's UUID into the `test_channel_uuid` setting first and
save -- action buttons can't take click-time input) to confirm segments
and a playlist actually appear under `storage_path`, and that
`http://<dispatcharr-host>:<http_port><playlist_route>` (the pieces
`start_buffer`/`snapshot_buffer`/`list_buffers` return) is fetchable.
"Stop All Buffers" is there for cleanup if something's stuck.

**A real operational gotcha hit while developing this, worth knowing
before you go looking for a bug that isn't one:** redeploying this plugin
(re-importing the zip with changes, even via Dispatcharr's own "overwrite"
flow) does not reliably make every already-running worker process pick up
the new code. `apps/plugins/loader.py` creates a fresh Python module and
swaps it into `sys.modules` on reload, but doesn't stop whatever the *old*
module's code already started -- this plugin's own background HTTP file
server (a thread bound to a port) is exactly that kind of already-started
state, so it can keep answering requests with stale code indefinitely
after a redeploy, even across a plain plugin disable/enable toggle. What
actually forced every worker to pick up new code reliably: Dispatcharr's
own dedicated reload endpoint (`POST /api/plugins/plugins/reload/`, no
plugin key needed -- reloads all plugins), *not* the per-plugin enable/
disable toggle or a redeploy-with-overwrite alone, both of which were
tried first and didn't fix it. If a code change to this plugin doesn't
seem to be taking effect, reach for that endpoint (or restart Dispatcharr
outright) before assuming the change itself is wrong.

## What's still not done

The addon-side integration -- live buffer playback, real seeking via
`get_live_manifest` + Range reads, `snapshot_buffer` as a standalone
capability -- is built and confirmed live, including pause/rewind/
fast-forward/live-follow on a real channel. Nothing outstanding is
currently tracked for this plugin.
