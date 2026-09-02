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
rolling buffer, held on the Dispatcharr server -- via a companion
Dispatcharr plugin this addon ships alongside itself
(`dispatcharr-plugin/timeshift_buffer/` in this repo), **not** built into
Dispatcharr itself and not installed through Kodi. See that directory's
own `README.md`/`plugin.py` for the plugin's design and the several
live-tested dead ends that led to its current shape (Django's `MEDIA_ROOT`
static route turned out to be unreachable due to a routing-order bug in
Dispatcharr's own `urls.py`; the plugin now runs its own minimal file
server instead). On this addon's side, `GetChannelStreamProperties()`
calls the plugin's `start_buffer` action over Dispatcharr's plugin REST
API (`POST /api/plugins/plugins/timeshift_buffer/run/`,
`DispatcharrClient::StartTimeshiftBuffer()`) and points `STREAMURL` at the
returned playlist through `inputstream.ffmpegdirect` with no `stream_mode`
set -- the same plain-growing-HLS-playlist recipe already proven for
in-progress-recording "Play live" (`FetchInProgressPlaylistSnapshot()`),
since architecturally it's the identical shape: a rolling, non-`ENDLIST`
playlist a client tails and can pause/seek within once paused.

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

**Decision: server-side timeshift does not attempt real seek/scrubbing.**
Chasing this further would need real debugging inside the Dispatcharr
container (verbose ffmpeg/curl logging, or instrumenting ffmpegdirect
itself) rather than another guess-and-redeploy cycle, and isn't worth
blocking the feature on. "Instant replay from buffer" still has real,
honest value as-is: it restarts playback from a fixed point in whatever
has been buffered (a real "watch that again" gesture), it just can't be
scrubbed within once playing -- attempting a `Player.Seek` during
snapshot playback will currently hang playback the way described above.
Local mode (`live_timeshift_mode: Local`) remains the only path in this
addon with real pause/rewind on live TV, via ffmpegdirect's own
`TimeshiftStream`.

**Built on the Kodi addon side, confirmed live end-to-end.** A PVR addon
using plain `STREAMURL` passthrough for live channels (as this one does)
gets no callback at all when the user presses pause/rewind mid-playback
-- that's handled entirely by the player/inputstream addon, with no hook
back into the addon -- so a truly seamless "press rewind while watching
live" gesture isn't achievable from here regardless of what the plugin
can do. Implemented as a `PVR_MENUHOOK_CHANNEL` context-menu entry,
"Instant replay from buffer" (only registered when `live_timeshift_mode`
is server-side, mirroring the same only-appears-when-relevant convention
as the in-progress-recording "Play live" hook), backed by
`m_pendingSnapshotChannelUid` -- the same one-shot arm/consume pattern as
`m_pendingLiveModeRecordingId`, with one real difference:
`CallChannelMenuHook()` calls `StartTimeshiftBuffer()` itself
immediately, before arming anything, rather than waiting for the next
Play. That's deliberate, not an oversight -- a snapshot can only ever
contain what's already been buffered, so if arming just set a flag and
waited, "instant replay" on a channel nobody had been server-side-
buffering yet would replay essentially nothing. Starting the buffer at
arm time means whatever elapses between selecting the menu item and
actually pressing Play becomes real, replayable content. Plain Play
without arming still calls `StartTimeshiftBuffer()` too (unchanged, the
live-tailing path); the armed case calls `SnapshotTimeshiftBuffer()`
instead on the next `GetChannelStreamProperties()` for that same channel.

Live test confirmed the whole chain: selecting "Instant replay from
buffer" fired `StartTimeshiftBuffer()` and the "armed" notification;
waiting ~45s then pressing Play opened
`.../<uuid>/snapshot/snapshot.m3u8` (not `live.m3u8`), and
`Player.GetProperties` reported `canseek: true` -- confirming the
snapshot approach genuinely resolves the live buffer's `canseek: false`
limitation. Plain (unarmed) Play was also retested and opens cleanly.
That retest also caught a real startup race: the addon could report
success and hand back a `STREAMURL` before ffmpeg (on the plugin side)
had written its first segment, so `inputstream.ffmpegdirect`'s very
first open attempt failed with `Error, could not open file` even though
the URL became reachable moments later. Fixed with
`DispatcharrClient::WaitForTimeshiftPlaylistReady()`, a poll-until-200
helper (up to 20 attempts, 500ms request timeout, 250ms between
attempts) called right after `StartTimeshiftBuffer()`/
`SnapshotTimeshiftBuffer()` succeed and before the URL is handed to
Kodi; confirmed fixed live (subsequent plain Play attempts opened
cleanly on the first try). Only open item is the seek failure documented
above, which is a real limitation of this feature (see "Decision" above),
not specific to the menu-hook/arm-consume wiring itself.

