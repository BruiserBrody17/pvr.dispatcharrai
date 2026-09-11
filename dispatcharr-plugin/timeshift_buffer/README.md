# Timeshift Buffer (Dispatcharr plugin)

Server-side rolling live-TV buffer per channel, held by Dispatcharr
itself. `pvr.dispatcharr` uses it to give live TV real pause, rewind,
fast-forward, and live-follow -- no extra step, just plain Play.

The seeking architecture (why this exists, what didn't work first, how
Kodi's native demuxer ends up handling seeking directly against this
plugin's own byte-range file server) is documented in
[docs/TIMESHIFT.md](../../docs/TIMESHIFT.md) in the main repo.

## How it works, briefly

`start_buffer` launches `ffmpeg` per channel against Dispatcharr's own
live proxy, writing rolling `.ts` segments and a playlist under
`storage_path`, served by this plugin's own minimal HTTP server (bound
to `http_port`) rather than Django's `MEDIA_ROOT` route (unreachable
here due to a routing-order issue in Dispatcharr's own `urls.py`).
`get_live_manifest` returns the current segment list with a stable
sequence number; the file server answers HTTP Range requests against
them, letting `pvr.dispatcharr` treat the buffer as one growing,
byte-seekable stream. Viewers are reference-counted -- the ffmpeg
process stops as soon as the last one deregisters -- with a background
reaper (leader-elected across worker processes) as the backstop for
viewers that vanish without deregistering (crash, network drop,
force-quit), reaping anything idle past `idle_timeout_seconds`.

Multiple devices watching the same channel share this one buffer
process -- Dispatcharr opens a single upstream connection per channel
regardless of viewer count. That sharing doesn't extend to rewind
depth: `pvr.dispatcharr` trims what it exposes locally to a small
near-live-edge window on every fresh channel open, so a device can only
rewind into what it's personally watched since opening the channel,
never another device's earlier viewing. See
[docs/TIMESHIFT.md](../../docs/TIMESHIFT.md)'s "Concurrent viewers"
section for why.

## Installing

1. Download `timeshift_buffer.zip` from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharr/releases)'s
   Assets and upload it via Dispatcharr's Plugins page **Import** button
   (**the folder name inside the zip must match `timeshift_buffer`
   exactly**, or every call 404s with "Plugin not found" -- already
   correct in the release zip). Alternatively, copy this directory to
   `data/plugins/timeshift_buffer/` on the host
   (`/app/data/plugins/timeshift_buffer/` inside the container), wherever
   your compose file bind-mounts `data/` -- useful if you're working from
   a repo checkout rather than a release.
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
