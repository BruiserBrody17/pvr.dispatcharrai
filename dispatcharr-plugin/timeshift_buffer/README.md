# Timeshift Buffer (Dispatcharr plugin)

Server-side rolling live-TV buffer per channel, held by Dispatcharr
itself. `pvr.dispatcharrai` uses it to give live TV real pause, rewind,
fast-forward, and live-follow -- no extra step, just plain Play.

The seeking architecture (why this exists, what didn't work first, how
Kodi's native demuxer ends up handling seeking directly against this
plugin's own byte-range file server) is documented in
[docs/TIMESHIFT.md](../../docs/TIMESHIFT.md) in the main repo.

## How it works, briefly

`start_buffer` launches `ffmpeg` per channel against Dispatcharr's own
live proxy (the same URL any viewer uses), writing rolling `.ts` segments
and a playlist under `storage_path`. Segments are served by this plugin's
own minimal HTTP server (bound to `http_port`) rather than through
Django's `MEDIA_ROOT` route, which a routing order issue in Dispatcharr's
own `urls.py` makes unreachable for this purpose. `get_live_manifest`
returns the buffer's current segments (with a stable sequence number, so
a client can address the rolling window correctly across refreshes); the
file server answers HTTP Range requests against them, which is what lets
`pvr.dispatcharrai` treat the buffer as one growing, byte-seekable
stream. A background reaper (leader-elected across worker processes)
stops and cleans up any buffer that's gone idle past `idle_timeout_seconds`.

Multiple devices watching the same channel share this one buffer
process -- Dispatcharr opens a single upstream connection to your
provider per channel regardless of how many devices are watching, not
one per viewer. That sharing doesn't extend to rewind depth, though:
`pvr.dispatcharrai` deliberately trims what it exposes locally to a
small near-live-edge window on every fresh channel open, so a device
can only rewind into what it's personally been watching since it opened
the channel, never another device's earlier viewing or time from before
it joined. See [docs/TIMESHIFT.md](../../docs/TIMESHIFT.md)'s
"Concurrent viewers" section for why -- Kodi's own demuxer can't
reliably seek backward into buffer content it hasn't personally read
through this session, confirmed live via a seek landing on the MPEG-TS
PTS wraparound point instead of anywhere near its target.

## Installing

1. Download `timeshift_buffer.zip` from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharrai/releases)'s
   Assets, and upload it via Dispatcharr's Plugins page **Import** button
   -- its top-level folder is already named exactly `timeshift_buffer`,
   which Dispatcharr requires for it to load (**the folder name inside
   the zip must match exactly**, or every call 404s with "Plugin not
   found"). Alternatively, copy this directory directly to
   `data/plugins/timeshift_buffer/` on the host (or
   `/app/data/plugins/timeshift_buffer/` inside the container), matching
   however you already reach the `data/` directory Dispatcharr's compose
   file bind-mounts -- useful if you're working from a repo checkout
   rather than a release.
2. In Dispatcharr's UI, open the Plugins page, click refresh, enable
   "Timeshift Buffer" (accept the trust-warning modal -- this plugin runs
   arbitrary server-side code, same as any other).
3. **Set `storage_path` to real, persistent storage** before using it --
   left on the container's own unmapped filesystem, continuous rolling
   writes will fill up whatever's backing that (often a small cache
   volume) fast.
4. **Map `http_port` (default `9192`) through your container config**,
   the same way `9191` already is -- without this, the plugin's file
   server is only reachable from inside the container.
5. Check `internal_base_url` matches how this plugin reaches Dispatcharr's
   own web service from inside the container (default
   `http://127.0.0.1:9191`).
6. The account calling these actions must be a Dispatcharr **admin**
   account (`user_level >= 10`).

## Testing manually

Paste a channel's UUID into the `test_channel_uuid` setting and save
(action buttons can't take click-time input), then use "Start Test
Buffer" / "Get Test Manifest" / "List Active Buffers" / "Stop Test
Buffer" on the Plugins page to confirm segments and a playlist appear
under `storage_path`, and that
`http://<dispatcharr-host>:<http_port><playlist_route>` is fetchable.
"Stop All Buffers" is there for cleanup if something's stuck.

## If a code change to this plugin doesn't seem to take effect

Redeploying (even via Dispatcharr's own "overwrite" import flow) doesn't
reliably make every already-running worker pick up new code -- this
plugin's own background HTTP server is a thread that keeps running under
the old code until something actually restarts it. Use Dispatcharr's own
`POST /api/plugins/plugins/reload/` (reloads all plugins, no plugin key
needed) or restart Dispatcharr outright; a plain per-plugin enable/disable
toggle isn't enough. See `docs/TIMESHIFT.md` for the full story.
