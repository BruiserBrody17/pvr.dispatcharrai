*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Catch-up ("timeshift") playback

Dispatcharr has a real catch-up/archive feature, exposed via
`GetEPGTagStreamProperties()`/`IsEPGTagPlayable()` on the guide's past
programmes (Kodi's normal "play from EPG" mechanism, same category as
recordings -- **not** `OpenLiveStream()`/`CloseLiveStream()`, no
`SetHandlesInputStream` capability needed). Important to understand what this
actually is, since it's easy to conflate with TVHeadend-style timeshifting:

- **It's per-channel and per-programme, not a continuous rolling buffer.**
  A channel supports it only if `Channel::catchupEnabled` (`is_catchup` in
  the API) is true and `catchupDays` (`catchup_days`) is nonzero -- both
  driven by whether the *upstream IPTV provider* offers catch-up/archive for
  that specific channel (the Xtream Codes `tv_archive` flag), not something
  Dispatcharr generates itself for every channel.
- **You pick a specific past (or currently-airing) EPG entry from the guide
  and it plays from that programme's start**, with normal seek/rewind
  *within that one programme* (the catch-up endpoint supports HTTP Range,
  confirmed with a real `206` on a ranged request). It is not "press
  rewind while watching live and seamlessly scroll back" the way TVHeadend's
  HTSP-based timeshift buffer works -- Dispatcharr has no equivalent concept
  of a generic rolling per-channel live buffer, and this addon's live
  playback (`GetChannelStreamProperties()`) is still plain URL passthrough
  with no addon-managed stream lifecycle.
- `IsEPGTagPlayable()` only reports true once the programme has actually
  started (`GetStartTime() <= now`) and is still within the channel's
  `catchupDays` retention window -- there's no way to query the provider's
  *actual* current archive depth per programme, so this is a best-effort
  window check, not a guarantee the archive still has that exact programme.

## Seeking reliability during catch-up playback

Reported as unreliable in practice. Isolated the cause by testing the raw
HTTP mechanics directly against a live instance: the catch-up endpoint's
`Accept-Ranges`/`Content-Range` handling is correct and fast at every offset
tried (six different large offsets across a 2.5GB archive file, all `206`,
all under 2s, verified the returned bytes actually differ between offsets).
So the unreliability isn't Dispatcharr's HTTP layer -- it's Kodi's own
generic MPEG-TS-over-HTTP seeking (plain `CCurlFile` + FFmpeg demuxer),
which estimates byte-position-from-time via internal PCR/bitrate sampling, a
well-known source of imprecise/flaky seeking for this content type in
general, independent of the server.

**Tried and reverted:** routing catch-up playback through the separate
`inputstream.ffmpegdirect` addon's `stream_mode: timeshift`, on the theory
that its seek handling would be more robust than Kodi's generic MPEG-TS
seeking. Confirmed live against a real install that this doesn't just fail
to help -- it breaks seeking entirely (no seeking at all, worse than the
original flakiness). Root-caused by reading ffmpegdirect's actual source
(`github.com/xbmc/inputstream.ffmpegdirect`, branch `Piers`):

- `stream_mode: timeshift` (`src/stream/TimeshiftBuffer.cpp`) seeks only
  within a local, segmented recording that ffmpegdirect itself
  progressively downloads from what it assumes is a *live*, continuously
  arriving source. Our catch-up URL is a single, already-complete archived
  file, not a live source -- there is nothing for it to progressively
  record, so its seek model doesn't apply.
- `stream_mode: catchup` (`src/stream/FFmpegCatchupStream.cpp`,
  `SeekCatchupStream()`) seeks by reconstructing a *new* URL for the exact
  wall-clock time being sought to, via a `catchup_url_format_string`. This
  requires the backend to support starting playback at an arbitrary
  in-programme timestamp. Dispatcharr's catch-up API does not support
  this: its `start` parameter only selects *which* archived programme to
  fetch, never a time offset within it -- in-programme position is meant
  to be handled entirely via HTTP Range on the byte stream.

Neither of ffmpegdirect's specialized modes matches Dispatcharr's actual
catch-up URL shape (one complete, Range-seekable file per programme).

Also tried and reverted alongside it: `PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE`
(a plain Kodi-core flag, unrelated to ffmpegdirect) to make the OSD feel
more like live TV. Per Kodi's own PVR client header
(`xbmc/pvr/addons/PVRClient.h`), setting it makes Kodi call back into
`GetChannelStreamProperties()` -- the *live-channel* path -- instead of
just using the catch-up URL returned from `GetEPGTagStreamProperties()`.
That's the live-channel code path, not built for a static archived file,
and made seeking worse, not better. Confirmed by removing it: seeking
returned to the original (flaky-but-present) behavior.

So the addon now plays the catch-up URL directly via
`PVR_STREAM_PROPERTY_STREAMURL` -- exactly the same three properties as
the original catch-up implementation, before either of these two attempts
at improving seek reliability. Seeking precision on raw MPEG-TS via Kodi's
built-in PCR/bitrate-based estimation remains inherently approximate --
this is the default behaviour when the setting below is off.

**A third attempt, revisited later per a direct user request to look at
this again, found a genuinely different (and, this time, working) code
path.** Both previous attempts pointed `PVR_STREAM_PROPERTY_INPUTSTREAM`
at `inputstream.ffmpegdirect` with an explicit `stream_mode` ("timeshift"
or "catchup") -- what hadn't been tried was leaving `stream_mode` unset
entirely. Confirmed via ffmpegdirect's own source
(`src/StreamManager.cpp`'s `Open()`): with no `stream_mode` set, it
instantiates the plain `FFmpegStream` class instead of
`FFmpegCatchupStream`/`TimeshiftStream` -- the same base class this
addon's in-progress-recording playback already routes through -- whose
`SeekTime()` calls libavformat's own `av_seek_frame()` against the mpegts
demuxer directly, rather than the generic byte-estimation seek Kodi-core
falls back to with no inputstream addon involved at all. Gated behind a
new setting, `enable_catchup_ffmpegdirect_seek` (off by default,
Live TV/EPG category) -- requires `inputstream.ffmpegdirect` to actually
be installed, same caveat as the two settings that already depend on it.

Verified live against a real instance, both seek directions, several
times, using a real `SportsCenter` catch-up recording: confirmed via
`kodi.log` that this path is genuinely active (`OpenStream() - Num Props:
1`, only `is_realtime_stream=false` set, no `stream_mode` at all;
`ffmpegdirect::FFmpegStream::OpenWithCURL`; a real
`Duration: 01:03:59.44` known upfront from the `proxy/catchup/...`
session URL). Seeks landed precisely -- within roughly 10-15 seconds of
the requested target (an 18:20 request landing at 18:09, a 5:00 request
landing at 5:15) -- and playback resumed and continued normally
afterward each time, a real improvement over the default path's
byte-estimation imprecision.

**One caveat recorded rather than glossed over: a seek can occasionally
take much longer than that ~10-20s typical case to actually land.** One
attempt sat completely unmoved -- confirmed via Kodi's own on-screen OSD
position readout, not just JSON-RPC (which could plausibly just be
reporting stale), holding at the identical pre-seek timestamp for 85+
seconds straight -- before testing moved on without conclusively
resolving whether it would have eventually landed given more time. Not
reproduced on demand despite deliberately retrying the same seek
distances and directions afterward, every other attempt (including
several seeks issued back-to-back in the same session) landing within the
normal ~10-20s window. So: a real, if apparently intermittent, latency
characteristic of this path -- not a hard, reliably-reproducible failure,
but also not nothing. Worth keeping an eye on with more real-world use
rather than either dismissing it or blocking the feature on fully
explaining it.

**A companion session's macOS testing found a credible explanation for
this latency, and a fix was tried for it -- and made things worse, not
better, in direct patient testing.** A real macOS `kodi.log` showed the
mechanism behind the occasional slowness: leaving `open_mode` unset
(above) lands on `OpenMode::CURL`, which routes ffmpegdirect's I/O
through Kodi-core's own `CCurlFile`/`CFileCache` rather than an
independent connection, and libavformat's mpegts demuxer's normal
PCR-probe seek algorithm (~15-30 probe-and-adjust reads, expected for a
format with no real index) was shown paying `CFileCache`'s own "cache
completely reset for seek to position X" cost on every single probe.
Forcing `open_mode` to `"ffmpeg"` instead (matching the in-progress-
recording HLS path, for the same reasoning: bypass Kodi-core's cache
layer by having FFmpeg's own native `http://` protocol handler own the
I/O) was tried live on Windows as the fix -- and a forward seek that
would typically land within the normal ~10-20s window under `CURL` mode
instead sat completely unmoved for **nearly 5 minutes (280s)** before
finally landing 68 seconds off target, confirmed via patient polling
specifically designed to avoid the mistake made on the first attempt at
this same test: an initial, shorter 60-second wait had already concluded
this looked "stuck," which in hindsight wasn't long enough to
distinguish "slow" from "actually stuck" for this path -- seeks here can
need patience on the order of minutes, not seconds, before concluding
anything either way, a lesson that applies to any future investigation
of this same latency, not just this one fix attempt. Reverted; `open_mode`
stays unset. The *diagnosis* of why `CURL` mode is occasionally slow is
still credible -- forcing `"ffmpeg"` mode just isn't the right fix for it,
and shouldn't be retried without knowing this.

