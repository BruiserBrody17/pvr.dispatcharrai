*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Live TV pause/rewind ("timeshift")

Requested as "timeshifting with the live TV buffer held on the Dispatcharr
side, similar to how tvheadend does timeshifting with Kodi." Originally
concluded Dispatcharr has no equivalent to TVHeadend's server-side rolling
live buffer, since there's nothing in its *core* API for one -- correct as
far as it goes, but **superseded below**: Dispatcharr does have a real,
documented server-side Python plugin system (`Plugins.md`/`Plugin_repo.md`
at its repo root), which turned out to be enough to build one after all.

Two implementations now exist, picked by the `live_timeshift_mode` setting
(off by default; was a plain `enable_live_timeshift` boolean before the
server-side mode existed -- a deliberate breaking settings change, not
preserved as a migration, since this addon was still effectively
single-user at the time):

**Local** (`live_timeshift_mode = 1`): `GetChannelStreamProperties()`
routes live channel playback through `inputstream.ffmpegdirect`'s
`stream_mode: timeshift`. Confirmed via ffmpegdirect's own README this is
exactly what that mode is for: adding pause/rewind to a plain,
continuously-arriving live stream with no backend cooperation required at
all, by recording it to a local on-disk buffer as it plays. The buffer
lives on the Kodi device's own storage (size/path/retention controlled by
ffmpegdirect's own addon settings), not on the Dispatcharr server, so it
doesn't persist across a Kodi restart and isn't shared between devices.
Requires `inputstream.ffmpegdirect` to be installed; if it isn't and this
mode is selected, live channel playback fails outright (not just
timeshift).

**Server-side** (`live_timeshift_mode = 2`): a genuine, TVHeadend-like
rolling buffer, held on the Dispatcharr server, with real pause/rewind/
fast-forward -- via a companion Dispatcharr plugin this addon ships
alongside itself (`dispatcharr-plugin/timeshift_buffer/` in this repo),
**not** built into Dispatcharr itself and not installed through Kodi. See
that directory's own `README.md`/`plugin.py` for the plugin's design and
the several live-tested dead ends that led to its current shape (Django's
`MEDIA_ROOT` static route turned out to be unreachable due to a
routing-order bug in Dispatcharr's own `urls.py`; the plugin now runs its
own minimal file server instead).

On this addon's side, `GetChannelStreamProperties()` for this mode leaves
`STREAMURL` unset entirely -- **not** the `inputstream.ffmpegdirect`
passthrough this paragraph originally described (see the investigation
below for why that approach's seeking turned out to be unfixable). Kodi
instead calls this addon's own `OpenLiveStream()`/`ReadLiveStream()`/
`SeekLiveStream()` (`PVRCapabilities::SetHandlesInputStream`), which call
`StartTimeshiftBuffer()` to ensure the plugin's buffer is running, then
serve it as a growing, byte-seekable stream via the plugin's
`get_live_manifest` action and Range-read segment files -- see "The
actual fix" near the end of this file for the full mechanism and its live
confirmation.

Two things confirmed against Dispatcharr's actual source before writing
the addon-side call, not assumed from `Plugins.md` alone:
- **The REST wrapper's response shape**: `apps/plugins/api_views.py`'s
  `PluginRunAPIView` always wraps whatever the plugin's own `run()`
  returned inside a top-level `"result"` key, alongside its own
  `"success"`/`"error"` -- so a plugin-level *logical* failure (e.g. the
  plugin's own `max_concurrent_buffers` cap) comes back as a normal HTTP
  200 with `"result": {"status": "error", ...}`, not as a non-2xx status.
  `StartTimeshiftBuffer()` has to check both layers.
- **The permission requirement**: `permission_classes_by_method["POST"] =
  [IsAdmin]` in `apps/accounts/permissions.py`, confirmed by tracing the
  actual mixin the view uses -- the plugin run endpoint requires an admin
  account (`user_level >= 10`), not just any authenticated user. This
  addon's existing JWT auth mechanism works fine as-is (permissions
  resolve against the live DB user, not token claims), but **the
  Dispatcharr account this addon is configured with must be an admin
  account**, or server-side timeshift fails with a 403 that
  `GetChannelStreamProperties()` surfaces as `PVR_ERROR_SERVER_ERROR` with
  a log line naming both possible causes (plugin not installed/enabled, or
  non-admin account) rather than a bare, unexplained failure.

`StartTimeshiftBuffer()` builds the final playlist URL from this addon's
own configured Dispatcharr host plus the port/path the plugin reports back
for its own file server, deliberately always as `http://` regardless of
the `use_https` setting -- the plugin's minimal file server has no TLS of
its own and isn't assumed to sit behind whatever reverse-proxy/TLS
termination the main API port might. If your Dispatcharr deployment splits
those (a reverse proxy that only forwards the main API port at a given
hostname, with the plugin's own port needing separate exposure), that's a
known, documented limitation, not something solved for -- see the plugin's
own `README.md`.

**Verified live end-to-end -- playback works, but pause/rewind itself
doesn't, for a confirmed, architectural reason.** First live attempt hit
a 404 (`"Plugin not found"`): the registry key is the plugin's *folder
name*, and the zip built for an earlier manual install had extracted to a
generic `plugin/` folder rather than `timeshift_buffer/` -- fixed by
reinstalling under the correct folder name, not an addon-side bug. After
that, a real channel (ESPN, via `Player.Open`) opened cleanly through the
whole chain: `StartTimeshiftBuffer()` reached the plugin, got back a real
port/route, `inputstream.ffmpegdirect` opened
`http://<host>:9192/<uuid>/live.m3u8` successfully (`Input #0, hls`, `start:
1.412000`, correct 1920x1080 h264 + eac3 5.1 streams, clean player start,
no errors).

But `Player.GetProperties` reported `canseek: false`, and a `Player.Seek`
call failed outright (`-32100 Failed to execute method`), not just a
UI-level no-op -- confirmed real, not a stale report. This is the *same*
root cause already documented above for in-progress-recording "Play
live": Kodi's own PVR layer gates `canseek` on a known, *finite* total
duration, independent of whatever `INPUTSTREAM_SUPPORTS_SEEK` the
inputstream addon itself advertises -- and a perpetually-growing live
playlist (`Duration: N/A`, confirmed in the same log) can never provide
one by definition, no matter how this addon or the plugin behave. Setting
`is_realtime_stream=false` (which is what actually unlocks seek support
for the in-progress-recording and catch-up cases elsewhere in this addon)
doesn't help here, because those cases eventually *do* get a real,
bounded duration (an `ENDLIST`-terminated playlist, or a catch-up
programme's known length) -- a rolling live buffer that never stops
growing structurally never can.

**Resolved, and confirmed live: the plugin now offers the same "Play
live" vs. "Play from start" trade-off already used for in-progress
recordings.** A new `snapshot_buffer` plugin action (params:
`channel_uuid`, requires `start_buffer` already running for that channel)
copies the buffer's currently-listed segment files into a separate,
non-recycled `snapshot/` subdirectory and writes an `ENDLIST`-terminated
playlist referencing the copies -- a real, finite, seekable window into
what's been buffered so far, while the live buffer itself keeps recording
in the background untouched. Copies rather than just re-listing the same
live files in a different shape, specifically because the live buffer's
own `-segment_wrap` keeps recycling those original files for as long as
it keeps running, which would risk a segment getting overwritten while a
client was still watching the "frozen" snapshot -- the same category of
"don't rely on something about to move under you" mistake this project
already got bitten by once before with `LocalPlaylistServer`'s gradual-cap
fix.

Confirmed live: a snapshot plays back correctly as a real, seekable file
(unlike the live buffer, which fails a `Player.Seek` call outright) --
**but seeking within it is confirmed broken, not just imprecise.** Four
separate live tests -- backward to an early point, backward near the
tail, forward mid-file, all at different real buffer sizes -- produced
the identical failure every single time, 4/4, both before and after a
real fix attempt (below):

```
demuxer seek to: <target>
AddOnLog: inputstream.ffmpegdirect: ffmpegdirect::FFmpegStream::SeekTime - unknown position after seek
demuxer seek to: <target>, success
CVideoPlayer::Process - eof reading from demuxer
```

`ffmpegdirect` logs that it doesn't know where the seek landed, then
Kodi's demuxer immediately reports end-of-file and playback sticks
(`Player.GetProperties` reports `speed: 0` indefinitely -- confirmed not
a slow resume by re-checking seconds later, still 0). Every target tried
was well inside the snapshot's real total duration (computed from the
`.m3u8`'s own `#EXTINF` sum), so this isn't an out-of-range seek either.

Root-caused by reading `inputstream.ffmpegdirect`'s own source
(`FFmpegStream.cpp`/`StreamManager.cpp`, `xbmc/inputstream.ffmpegdirect`
on GitHub): this addon's server-side stream properties never set
`inputstream.ffmpegdirect.stream_mode`, so ffmpegdirect falls back to its
generic `FFmpegStream` class and a bare `av_seek_frame()` -- the code
path above. This addon's *catch-up* feature avoids this entirely by
setting `stream_mode: catchup` (a specialized `FFmpegCatchupStream`
class with its own seek logic), and *local* live-timeshift mode avoids it
by setting `stream_mode: timeshift` (`TimeshiftStream`, its own local
buffer and seek) -- the snapshot path is the one place in this addon that
lands on the generic, broken one.

**Fix attempted and confirmed NOT to work:** the working theory was that
`_start_ffmpeg`'s `-reset_timestamps 1` (each segment's PTS restarting
near zero) defeats `av_seek_frame`'s global-target-PTS search, since no
single segment's local PTS space would contain the computed global
target. Removed it, redeployed, stopped the stale buffer, built a fresh
one, and re-tested live: **identical failure**, same log signature, same
`speed: 0` stuck state, on a target comfortably inside the new snapshot's
duration. This rules out `-reset_timestamps` as the cause; the flag was
left removed anyway (continuous timestamps across segments is closer to
how real-world HLS packaging works, and it's not implicated as harmful),
but the actual root cause of the `av_seek_frame` failure is still
unresolved.

**The paragraph above (and the "Instant replay from buffer" menu-hook
design that followed it) is SUPERSEDED -- kept for the history, not as
current behavior.** At the time, giving up on direct-live-buffer seeking
and routing pause/rewind through a menu-hook-armed, one-shot finite
snapshot (mirroring the in-progress-recording "Play live"/"Play from
start" trade-off) seemed like the only path forward, since every seek
attempt against `inputstream.ffmpegdirect`'s generic HLS path -- live
buffer or snapshot alike -- failed identically. That menu hook
(`kMenuHookInstantReplay`, `CallChannelMenuHook()`,
`m_pendingSnapshotChannelUid`) was built, and its own arm/consume
mechanics were confirmed working live (the notification fired, the
snapshot URL opened correctly, `canseek: true` was reported) -- the
plumbing was never the problem, only the seek underneath it. It's since
been removed from this addon entirely, not left in as a fallback: see
below for why it's no longer needed.

## The actual fix: this addon demuxes the buffer itself, not ffmpegdirect

The seek failures above all shared one root cause: they went through
`inputstream.ffmpegdirect`'s generic `FFmpegStream::SeekTime()` (a bare
`av_seek_frame()` against an HLS-parsed `AVFormatContext`), because
`GetChannelStreamProperties()` handed Kodi a `STREAMURL` for `ffmpegdirect`
to open, whatever shape that URL's playlist took. The fix wasn't a better
playlist shape -- it was routing around that seek path entirely.

Kodi's PVR client API has a mode where the addon itself owns live-channel
I/O: `PVRCapabilities::SetHandlesInputStream(true)`, plus
`OpenLiveStream()`/`CloseLiveStream()`/`ReadLiveStream()`/
`SeekLiveStream()`/`LengthLiveStream()`. Confirmed by reading Kodi-core's
own `PVRPlaybackState.cpp`: `StartPlayback()` only calls
`item->SetDynPath(url)` when `GetChannelStreamProperties()`'s `STREAMURL`
is non-empty -- leave it unset for a given channel and Kodi falls through
to these addon callbacks instead, exactly like this addon's own
already-working completed-recording playback
(`OpenRecordedStream()`/`ReadRecordedStream()`/`SeekRecordedStream()`)
already does. That's the load-bearing precedent: recordings never had a
seek problem in this addon, because they never went through
`inputstream.ffmpegdirect` at all -- Kodi's own internal demuxer
(`CDVDDemuxFFmpeg`) does the MPEG-TS parsing and seek refinement directly
against a plain byte-seekable source. Server-side live timeshift now uses
the identical mechanism.

What that needed from the plugin (`dispatcharr-plugin/timeshift_buffer/`):
Range-request support in its file server (previously whole-file-only),
and a new `get_live_manifest` action exposing the buffer's currently
-listed segments with byte sizes/durations and a stable,
`#EXT-X-MEDIA-SEQUENCE`-derived `sequence` number per segment. That
sequence number matters because the buffer's rolling window means a fresh
manifest fetch's own byte/time offsets aren't stable -- "byte 0" points at
different content an hour later as old segments roll off. `DispatcharrClient`
(`OpenLiveTimeshiftStream()`/`ReadLiveTimeshiftStream()`/
`SeekLiveTimeshiftStream()`/`RefreshLiveManifest()`) merges repeated
fetches by that sequence number into one append-only, fixed-origin address
space instead of trusting each fetch's own relative offsets -- the part of
this design most likely to have a subtle bug, since it's the one piece
with no direct precedent elsewhere in this addon.

`GetStreamTimes()` reports a `ptsEnd` that grows on every call (kodi-dev-kit's
own `PVRStreamTimes` doc comment: *"For Live TV, ... must point to end of
the timeshift buffer"*) -- confirmed via `inputstream.ffmpegdirect`'s own
`TimeshiftStream` class (what Local mode uses) that Kodi genuinely supports
a growing-duration, still-seekable real-time stream; this addon's server-side
mode now does the equivalent at the PVR-client level instead of the
inputstream level.

**Confirmed live, end-to-end, on a real channel:** plain Play opens
correctly with zero properties beyond `isrealtimestream=true` (no
`STREAMURL`, no `inputstream.ffmpegdirect` in the picture at all, per
`GetChannelStreamProperties()`'s own debug log) and real audio/video
decodes immediately. `canseek: true` from the start. Pause → Resume:
clean. A -10s and a +20s seek both landed within about a second of target
(`CDVDDemuxFFmpeg::SeekTime - seek ended up on time ...` -- Kodi's native
demuxer, not ffmpegdirect) and playback kept running afterward, unlike
every prior attempt. 4x fast-forward worked (with expected transient
decoder warnings from landing mid-GOP, not a functional break -- the same
class of noise fast-forwarding produces on any byte-seeked, non-frame
-indexed content). A -95s rewind spanning several manifest refreshes --
directly exercising the sequence-based merge logic, not just a single
fetch -- landed correctly and kept playing. Clean stop and clean reopen
afterward.

The "Instant replay from buffer" menu hook is retired as a result: plain
Play now gets everything it offered (and real scrubbing, which it never
could) with no extra step, the same way Local mode always has. The
plugin's `snapshot_buffer` action remains in the plugin as a standalone
capability -- see its own README -- but this addon no longer calls it.

