# Timeshift Buffer (Dispatcharr plugin)

Server-side rolling live-TV buffer per channel, held by Dispatcharr itself
-- an alternative to `pvr.dispatcharrai`'s local (`inputstream.ffmpegdirect`
`stream_mode: timeshift`) buffering, which lives on the Kodi device instead.

**Status: the full live-vs-snapshot design is now confirmed live,
end-to-end, against a real Dispatcharr instance and a real
`pvr.dispatcharrai` build.** `start_buffer`'s live playlist plays cleanly
but genuinely can't seek (`canseek: false`, a `Player.Seek` call failing
outright, confirmed live) -- not a bug, the identical root cause
`pvr.dispatcharrai`'s own docs already document for in-progress-recording
"Play live": Kodi's PVR layer requires a known, *finite* duration to permit
seeking at all, and the live playlist is deliberately `Duration: N/A` --
that's what lets it keep tailing new content. `snapshot_buffer` freezes the
buffer into something with a real, finite duration, and **confirmed live
it plays back correctly as a real seekable file**. The Kodi addon-side
integration (context-menu "Instant replay from buffer" arming a snapshot
open on the next Play) is also now built and confirmed live end-to-end --
see `pvr.dispatcharrai`'s own `docs/API_NOTES.md`.

**Known, confirmed-unfixable-for-now limitation: seeking within a
snapshot does not work.** Four separate live tests -- different
directions, different positions, all comfortably inside the snapshot's
real duration -- failed identically: `inputstream.ffmpegdirect` logs
`SeekTime - unknown position after seek`, then the demuxer immediately
hits EOF and playback sticks. Root-caused (not just observed) by reading
`inputstream.ffmpegdirect`'s own source: this project's server-side
stream properties never set `stream_mode`, so ffmpegdirect falls back to
a generic seek path that this addon's catch-up and local-timeshift
features both avoid by setting `stream_mode: catchup` / `timeshift`
instead. A real fix attempt -- removing `_start_ffmpeg`'s
`-reset_timestamps 1`, on the theory that per-segment PTS resets were
defeating the seek's target-PTS search -- was built, deployed, and
re-tested live: **identical failure**, so that theory is ruled out and
the flag was left removed (harmless, more standard HLS practice) without
resolving the actual cause. Decision: treat "Instant replay from buffer"
as exactly that -- restart from a fixed point, not scrubbable -- rather
than keep guessing at fixes. See `pvr.dispatcharrai`'s `docs/API_NOTES.md`
for the full trace if you want to pick this back up.

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
   copies -- a real, finite, seekable window into what's been buffered so
   far. Copies rather than just re-listing the live files in a different
   shape because the live buffer's own `-segment_wrap` keeps recycling
   those original files in the background for as long as it keeps running,
   which would risk a segment getting overwritten while a client watching
   the "frozen" snapshot still had it queued up. The live buffer itself
   keeps recording in the background after a snapshot is taken -- only the
   snapshot's own copied files are frozen. **Confirmed live**: playback of
   a snapshot works correctly as a real, seekable file (`canseek: true`
   is reported), but actually seeking within it does not work -- see the
   status note above for the confirmed failure mode and the ruled-out fix
   attempt.

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

## Testing manually

Use the "Start Test Buffer" / "Take Test Snapshot" / "List Active Buffers"
/ "Stop Test Buffer" buttons on the Plugins page (paste a channel's UUID
into the `test_channel_uuid` setting first and save -- action buttons can't
take click-time input) to confirm segments and a playlist actually appear
under `storage_path`, and that
`http://<dispatcharr-host>:<http_port><playlist_route>` (the pieces
`start_buffer`/`snapshot_buffer`/`list_buffers` return) is fetchable.
"Stop All Buffers" is there for cleanup if something's stuck.

## What's still not done

The addon-side integration described above (live buffer playback, the
"Instant replay from buffer" context-menu action, snapshot playback) is
built and confirmed live. Seeking within a snapshot is a known, currently
unresolved limitation -- confirmed broken (not just imprecise) across
multiple live tests and one real fix attempt, both ruled out -- see the
status note at the top of this file. Treated as accepted for now rather
than actively worked on; picking it back up would need real debugging
inside the Dispatcharr container (verbose ffmpeg/curl logging, or
instrumenting `inputstream.ffmpegdirect` itself), not another guess.
