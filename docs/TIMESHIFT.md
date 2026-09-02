*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Live TV pause/rewind ("timeshift")

Requested as "timeshifting with the live TV buffer held on the Dispatcharr
side, similar to how tvheadend does timeshifting with Kodi." Originally
concluded Dispatcharr has no equivalent to TVHeadend's server-side rolling
live buffer, since there's nothing in its *core* API for one -- correct as
far as it goes, but **superseded below**: Dispatcharr does have a real,
documented server-side Python plugin system (`Plugins.md`/`Plugin_repo.md`
at its repo root), which turned out to be enough to build one after all.

Two implementations existed side by side for a while, picked by the
`live_timeshift_mode` setting (off by default; was a plain
`enable_live_timeshift` boolean before the server-side mode existed -- a
deliberate breaking settings change, not preserved as a migration, since
this addon was still effectively single-user at the time). **Local was
later removed** once server-side proved stable through real use -- one
less thing to choose between, and one less dependency on a separate addon
for live channel playback at all. `live_timeshift_mode`'s value `1` (what
local used to mean) was deliberately left unreused at the time rather
than renumbering server-side down to `1`. **`live_timeshift_mode` itself
was later removed too, once server-side had proven stable through further
real use -- server-side timeshift is now unconditional, not a setting at
all, and `GetChannelStreamProperties()`/`OpenLiveStream()` no longer
branch on anything.** A real consequence, not just an implementation
detail: this addon now requires the companion plugin installed and
enabled (and the configured account to be a Dispatcharr admin) for live
TV playback to work *at all*, not just for pause/rewind -- there's no
plain-STREAMURL fallback left to fall back to. The two descriptions below
are kept as-written for the history of why server-side won, not as a
currently-available choice.

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

## Two follow-up bugs found via real use, and their actual root causes

The initial confirmation above was a single, freshly-started session per
channel. Real day-to-day use surfaced two bugs that scenario didn't cover:
a higher-bitrate channel (ESPN 1080p) hitching every few seconds, and
reopening a channel after Stop resuming from the old stale position
instead of live. Both traced back to the same place --
`OpenLiveTimeshiftStream()` -- and both are fixed by the same change.

**First hypothesis, tested and ruled out:** the hitching looked like a
throughput problem, and there was a real (separate) one to fix --
`CDVDDemuxFFmpeg::CreateDemuxer()` defaults its AVIO read buffer to a
hardcoded 4096 bytes unless the PVR client implements
`GetStreamReadChunkSize()`, which this addon didn't. Every ffmpeg demux
read was therefore one full HTTP round trip to the timeshift plugin's file
server per 4KB, confirmed in Kodi's own source
(`DVDDemuxFFmpeg.cpp:356-360`, `InputStreamPVRBase.cpp`'s `GetBlockSize()`).
Implemented it (256KB now, applies to both live and recording playback,
both going through the same `CInputStreamPVRBase`-backed path) -- a real
improvement, kept, but confirmed live it did **not** fix the hitching: the
exact same stall pattern persisted afterward, unchanged.

**Actual root cause:** `OpenLiveTimeshiftStream()` left `position` at its
default-constructed `0` -- the start of whatever's still known in the
buffer's fixed-origin address space, not "now". For a channel whose buffer
had just been started (or one whose buffer happened to be small), `0` is
also very close to `totalBytes` -- i.e., playback was starting essentially
at the live edge either way, with ~zero cushion. ffmpeg's segmenter only
exposes a segment once it's fully closed (`segment_seconds`, 6s by
default), so sitting right at the tail means there is *nothing* to read
until the next segment closes -- confirmed by the stall period tracking
`segment_seconds` almost exactly (a repeating "stream stalled" -> buffering
-> resume cycle roughly every 4-5 seconds in the actual test log). MLB
Network's buffer, still running from earlier testing, had simply
accumulated more backlog by the time it was opened -- explaining the
apparent channel-to-channel difference without any real bitrate
dependency. The same zero-margin `position` is also exactly why reopening
resumed from the old position instead of live: `0` never moved, so every
open replayed from the same spot.

The fix (`OpenLiveTimeshiftStream()`): after the initial manifest fetch,
set `position` to the byte offset of the segment 3 segments behind the
current tail (or the true tail if fewer than that many segments exist yet,
e.g. right after a cold `StartTimeshiftBuffer()`) -- a real cushion of
already-available data for the demuxer's read-ahead to draw on between
segment arrivals, while staying clearly "live" to the viewer, comparable
to the inherent latency any real live-TV/DVR service already has.

**Confirmed live**, ESPN (1080p): 60+ seconds of continuous playback, zero
"stream stalled" events (was multiple per minute before), a -30s seek
landed cleanly and playback continued. MLB Network: played ~40s, Stop,
waited a few seconds, reopened -- resumed near the current live edge
(matching elapsed real time), not the original stale start position.

## Fixed: seeking after a Stop/reopen didn't land on target

Real use surfaced a third issue the above testing didn't catch: seek
*within* one continuous session is fully accurate (confirmed repeatedly,
including the -95s multi-refresh rewind above), but after a Stop and
reopen of the *same* channel, a subsequent seek -- of any size -- didn't
land where requested. It was first suspected to be an addon-side bug
(tried preserving `m_liveTimeshiftStream`'s segment history across the
Close/reopen so "rewind" could reach further back than the plugin's
current rolling-manifest window) -- confirmed live this doesn't fix it,
and doesn't even change the observed behavior in practice, since the
plugin's own rolling window is usually still large enough on its own to
cover the whole session anyway.

Root-caused by tracing actual byte positions through temporary logging
added to `SeekLiveTimeshiftStream()`/`ReadLiveTimeshiftStream()`: Kodi's
`CDVDDemuxFFmpeg` creates a brand-new demuxer instance on every
`OpenLiveStream()`, with its own `m_startTime` PTS anchor established
fresh from whatever's first read *this session* -- it has no seek index
for anything it hasn't itself read yet, regardless of what this addon's
own byte-address-space nominally contains further back. A -90s seek
request was observed landing at literal byte 0 (`SeekLiveTimeshiftStream
(position=0, whence=0)`, confirmed via the log) rather than anywhere near
the intended target -- Kodi/ffmpeg's own generic fallback for a backward
seek it can't otherwise resolve, not a value this addon computed or
clamped to. Kodi's own `SeekTime()` (`DVDDemuxFFmpeg.cpp`) only waits for
*any* valid PTS after a seek, not one matching the requested target, so
playback didn't stall -- it just resumed forward from wherever the
fallback landed, exactly matching what was observed: "no matter what
seeking I did, it always started playing the feed from the beginning of
the initial playback."

`CInputStreamPVRBase` doesn't implement Kodi's `IPosTime` interface
(confirmed by reading its header) -- the one hook that would let an input
stream handle time-based seeks directly and bypass ffmpeg's generic
byte-domain guessing entirely -- so this couldn't be fixed by intercepting
the seek itself. **The actual fix: `OpenLiveTimeshiftStream()` now stops
and restarts the channel's server-side buffer on every Open()** (a new
`StopTimeshiftBuffer()`, calling the plugin's `stop_buffer` action, before
`StartTimeshiftBuffer()`), instead of reattaching to whatever's already
been running since a previous session. Every Play now gets a genuinely
fresh ffmpeg process and segment sequence, so "byte 0" of this addon's
address space and "where this session's demuxer started reading" are the
same point again -- restoring exactly the alignment that already made
seeking work within one continuous session, now for a freshly reopened one
too.

**Confirmed live**: reopened ESPN (1080p), let ~25s accumulate, then three
seeks in the same reopened session -- `-15s` landed at 12.9s, a follow-up
`+10s` landed at 95.0s (consistent with real elapsed time between them),
and a deliberately-oversized `-1000s` landed at 0.4s, correctly clamped to
the true start of the fresh buffer rather than some unrelated fallback
point. The debug log showed a genuine multi-step binary search (probing
byte 0, then near the tail, then narrowing between them) converging on
each target, instead of the single-probe fallback-to-0 seen before this
fix. No hitching on a cold buffer either (0 stalls across 45s on ESPN,
30s on MLB Network) -- the margin-based near-live starting position
(above) still applies on top of this, so a fresh buffer still gets a few
seconds' cushion before playback starts rather than reading right at its
own bleeding edge.

**Trade-off**: stopping and restarting the buffer on every Open() means a
second Kodi client (or profile) watching the same channel concurrently
would have its buffer torn out from under it mid-playback -- the plugin's
`start_buffer` is otherwise idempotent specifically so multiple viewers
can share one upstream connection per channel (see the plugin's own
README). This trades that sharing away for correct per-session seeking,
which is the right trade for a single-viewer setup but worth knowing if
this addon is ever used from more than one Kodi client against the same
Dispatcharr account at once.

## Follow-up bug from the fresh-buffer fix: repeated "live playlist not found"

The fresh-buffer-per-Open fix above introduced its own regression, caught
via real use: a Stop followed by Play sometimes failed outright
(`failed to open server-side timeshift stream ...: live playlist not
found -- the buffer may not have produced any segments yet`), and once it
happened once, it kept happening on every retry until the stream was
stopped directly in Dispatcharr (not just re-tried from Kodi).

Root cause, confirmed via direct testing against the plugin's own actions
(`start_buffer`/`stop_buffer`/`get_live_manifest`) and Dispatcharr's own
`/proxy/ts/status/<uuid>`: when `start_buffer` reattached to an
already-running buffer (the old behavior), it returned instantly with
content already available. Now that every Open() forces a genuinely fresh
ffmpeg process, that process needs a real few seconds to connect to
Dispatcharr's live proxy and produce a full first segment
(`segment_seconds`, 6s by default) before there's a playlist to report at
all -- `OpenLiveTimeshiftStream()` was failing on the very first check
instead of allowing for that. Worse, retrying Play right after such a
failure made it *worse*, not better: each retry's own `StopTimeshiftBuffer()`
call killed the previous attempt's buffer moments before it would have
finished starting, repeating indefinitely -- a self-perpetuating failure
loop that only broke once a manual stop in Dispatcharr (not immediately
followed by a Kodi-triggered restart) let a buffer finally start
undisturbed.

Fixed by giving `OpenLiveTimeshiftStream()` a real cold-start grace
period: it now retries `RefreshLiveManifest()` for up to 15 seconds (30
attempts, 500ms apart) instead of failing on the first check. **Confirmed
live**: three consecutive Stop -> wait 2s -> Play cycles on ESPN (1080p),
all three succeeded cleanly with zero "live playlist not found" errors
(previously this failed every time); a genuinely cold first Open also
succeeded within the retry window with no hitching afterward (0 stalls).

## Seek latency: the catch-up-to-tail loop was the real cost, not the network

Seeking was functionally correct after the fixes above, but slow --
measured live at 4-6+ seconds for a single seek, most of it in one or two
long pauses rather than spread evenly. Diagnosed with real timing
instrumentation (temporarily logging curl's own `CURLINFO_TOTAL_TIME` per
request, and the catch-up-to-tail loop's own duration) rather than
guessing: individual HTTP requests to the plugin's file server were
consistently fast (never exceeded the 50ms logging threshold, confirming
this wasn't network latency or a throughput problem), and ffmpeg's own
multi-step binary search, once it had data to work with, converged in well
under 200ms. The actual cost was almost entirely in
`ReadLiveTimeshiftStream()`'s own catch-up-to-tail loop.

Two compounding problems, both found via the same instrumentation:

1. **The loop's budget (8 attempts * 250ms = 2s) was far shorter than the
   real gap between segments** (`segment_seconds`, 6s by default). It gave
   up almost every time, Kodi immediately retried the read, landed right
   back in the same loop, and repeated -- turning what should be one ~6s
   wait into 2-4 full "gave up" cycles (12-16+ seconds), confirmed via the
   loop's own logged attempt counts and elapsed time. This was firing
   constantly even during *ordinary* near-live playback, not just seeking
   -- usually absorbed by Kodi's own read-ahead cache without a visible
   stall, but real wasted time regardless, and it directly padded out any
   seek whose own internal probing landed at/near the live edge (routine
   for a seek originating near "now"). Fixed by sizing the budget off the
   *last known segment's own actual duration* (with margin) instead of a
   fixed guess, so one wait reliably covers one real gap regardless of how
   `segment_seconds` is configured.

2. **ffmpeg's own generic mpegts seek does a real multi-step probe**
   (confirmed via `SeekLiveTimeshiftStream` tracing -- several distinct
   byte positions probed in quick succession while it estimates), and one
   of those probes routinely overshoots right up to the current tail.
   Blocking that probe for a full segment interval was, on its own, the
   single largest contributor to seek latency measured live (a 4.2s wait
   out of one seek's ~4.4s total). Fixed by giving a read landing at the
   tail a fast, near-instant "not there" (a single attempt, no sleep)
   instead of the full wait when it's likely part of active seek probing
   -- ffmpeg can usually just try an earlier candidate rather than getting
   this exact byte. "Likely probing" isn't just a time window after the
   last seek (confirmed live that alone caused a *different* regression:
   normal decode reads landing at the tail right after a seek *completes*
   also fell inside the window and wrongly got the fast, wrong answer,
   visibly pausing playback) -- a read landing at the *same* position where
   a fast probe already gave up escalates to the full budget instead,
   since that's no longer a fresh candidate, it's a genuine stuck wait.

**Confirmed live**: a forward seek landing near the live edge -- the exact
scenario that previously took 4-6+ seconds -- now completes in 33-140ms,
landing within about a second of the requested target (segment-boundary
snapping, not a precision issue). A five-seek sequence (mixed forward and
backward, small and large) left playback healthy afterward with zero
stalls and zero "unknown position" errors.

## Not a bug: JSON-RPC's Player.GetProperties "time"/"totaltime" is EPG-relative, not buffer-relative

Testing this addon via JSON-RPC (as all of the above was), `Player.
GetProperties`'s `time`/`totaltime`/`percentage` properties for a channel
never match `GetStreamTimes()`'s own small, buffer-relative range -- they
instead look like "how far into the current EPG programme are we" (e.g.
showing ~38 minutes into a channel whose timeshift buffer has only existed
for under a minute, with `totaltime` matching the EPG programme's own
scheduled duration, e.g. a flat 1 or 2 hours). This is real, reproducible,
and initially looked like a serious bug -- an absolute `Player.Seek`
computed from that displayed value lands at/near the live edge every time,
regardless of target, while a small buffer-relative target lands
correctly. Confirmed by reading Kodi-core, not guessed: `PlayerOperations.cpp`
(`GetPropertyValue`, the `"time"`/`"totaltime"`/`"percentage"` branches)
hardcodes `epg->Progress()`/`epg->GetDuration()`/`epg->ProgressPercentage()`
for *any* `IsPVRChannel()` item, unconditionally -- this is a Kodi-core,
JSON-RPC-API-level design choice that applies to every PVR addon, not
something this addon (or any addon) controls or can opt out of.

**The real, in-GUI mechanism is unaffected and correct.** Kodi-core has a
separate, dedicated set of info labels for genuinely timeshift-capable PVR
streams (`PVR.TimeshiftSeekbar`, `PVR.TimeshiftProgress`, and friends,
`GUIDialogSeekBar.cpp`/`PVRGUITimesInfo.cpp`), and traced its data flow
end to end: `CPVRGUITimesInfo::UpdateTimeshiftData()` reads
`CServiceBroker::GetDataCacheCore().GetPlayTimes()`, which `CVideoPlayer`
populates directly from `state.time`/`state.timeMin`/`state.timeMax` --
which, per `VideoPlayer.cpp`'s `UpdatePlayState()`, come from
`m_pInputStream->GetITimes()`, i.e. `CInputStreamPVRBase::GetTimes()`,
i.e. this addon's own `GetStreamTimes()`. The *generic*, skin-standard
`Player.Time`/`Player.Duration` info labels (what virtually every skin's
actual OSD and seek bar are built on -- distinct from the JSON-RPC
`Player.GetProperties` properties above, a separate code path) come from
that same `state.time`/`state.timeMax`. **Confirmed live**, not just from
source: with a ~55s-old buffer, `Player.Time`/`Player.Duration` via
`XBMC.GetInfoLabels` correctly showed `00:40`/`00:55` -- small and
buffer-relative, nothing like the ~38-minute EPG figure `Player.
GetProperties` shows for the same moment.

**Practical takeaway for testing this addon (or any timeshift-capable PVR
addon) via JSON-RPC**: don't compute an absolute `Player.Seek {"time":
...}` target from `Player.GetProperties`'s `time`/`totaltime` on a PVR
channel -- it's EPG-relative by Kodi-core design, not stream-relative, and
feeding it back into an absolute seek silently targets the wrong domain
entirely. Relative seeks (`Player.Seek {"seconds": N}`, used throughout
all the testing above) are unaffected -- confirmed via `VideoPlayer.cpp`'s
`SeekTimeRelative()`, which computes its target from the player's own
internal clock, not from the EPG-derived display value. For precise
absolute-position testing, read `Player.Time`/`Player.Duration` (or the
`PVR.Timeshift*` labels) via `XBMC.GetInfoLabels` instead.

## Buffer teardown was slow to notice a Stop

Real use surfaced one more gap: `CloseLiveTimeshiftStream()` (called on a
plain Stop) only ever reset this addon's own local state -- it never told
the plugin to actually stop the server-side buffer. That only happened at
the *start* of the next `OpenLiveTimeshiftStream()` (see the fresh-buffer
fix above), so between a Stop and the next Play (or never, if the user
didn't come back to that channel), the buffer's ffmpeg process and
Dispatcharr's own upstream client registration for it just kept running
until the plugin's own idle-timeout reaper eventually noticed -- confirmed
against `idle_timeout_seconds`'s default of 120s in plugin.py, matching
what was observed live (Dispatcharr's own `/proxy/ts/status/<uuid>` still
showing `state: active` for about two minutes after Stop).

Fixed by having `CloseLiveTimeshiftStream()` also call `StopTimeshiftBuffer()`
(a new addon-side wrapper around the plugin's existing `stop_buffer`
action), on a detached background thread so the network round trip (plus
the plugin's own up-to-5s SIGTERM-then-SIGKILL grace period for the ffmpeg
process) doesn't block Kodi's calling thread just to tear this down. Safe
against a near-immediate reopen's own (synchronous) `StopTimeshiftBuffer()`
call for the same channel racing this one -- `stop_buffer` is idempotent
and its file cleanup already tolerates "already gone" (confirmed in
plugin.py's `_remove_channel_files`), so no plugin-side change was needed.

**Confirmed live**: Stop, then polled Dispatcharr's own
`/proxy/ts/status/<uuid>` every few seconds -- showed `Channel ... not
found` (cleared) within about 12 seconds of Stop, down from ~120s.
Immediately reopening the same channel afterward still worked cleanly (no
errors, no stalls), confirming the two `StopTimeshiftBuffer()` call sites
(this one and `OpenLiveTimeshiftStream()`'s) don't interfere with each
other in practice.

## Buffers showed as "Anonymous" from "127.0.0.1" in Dispatcharr's Stats

Because a server-side timeshift buffer's ffmpeg process reads from
Dispatcharr's own live proxy from *inside* Dispatcharr's own container --
not from the viewer's actual device -- its connection carried no
credentials and no meaningful IP, so Dispatcharr's Stats screen showed
every buffer as an anonymous client at `127.0.0.1`. Fixed entirely on the
plugin side (see `dispatcharr-plugin/timeshift_buffer/plugin.py`'s
`_stream_attribution_headers()` and its own detailed docstring for the two
separate Dispatcharr-core mechanisms involved -- DRF/JWT auth for the
user, and `get_client_ip()`'s `X-Real-IP` trust for the IP, the second of
which took a real correction: an initial `X-Forwarded-For` attempt looked
right but didn't work, because Dispatcharr's own XFF handling specifically
skips any hop that's itself a private-range address, silently discarding a
home-LAN client's own IP as if it were just another internal proxy).

The addon's only role here is supplying the two values `start_buffer` uses:
`OpenLiveTimeshiftStream()`/`StartTimeshiftBuffer()` pass `username` (the
Dispatcharr account this addon is already configured with) and `client_ip`
(cached from `CURLINFO_LOCAL_IP` on `Request()`'s own connection -- the
local interface this machine actually reaches Dispatcharr through, not a
separate platform-specific "what's my IP" lookup) as `start_buffer`
params. No new addon setting and nothing new to configure -- it's derived
from what was already there.

**Confirmed live**: `/proxy/ts/status/<uuid>`'s client entry went from
`user_id: "0"`, `ip_address: "127.0.0.1"` to the real account's id and the
actual LAN IP of the machine running Kodi, both through a direct plugin
action call and through actual Kodi playback.

