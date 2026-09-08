*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Live TV pause/rewind ("timeshift")

Requested as "timeshifting with the live TV buffer held on the Dispatcharr
side, similar to how tvheadend does timeshifting with Kodi." Originally
concluded Dispatcharr has no equivalent to TVHeadend's server-side rolling
live buffer, since there's nothing in its *core* API for one -- correct as
far as it goes, but **superseded below**: Dispatcharr does have a real,
documented server-side Python plugin system (`Plugins.md`/`Plugin_repo.md`
at its repo root), which turned out to be enough to build one after all.

Three implementations have existed at various points, picked by the
`live_timeshift_mode` setting (was a plain `enable_live_timeshift`
boolean before the server-side mode existed -- a deliberate breaking
settings change, not preserved as a migration, since this addon was still
effectively single-user at the time). **Local was removed** once
server-side proved stable through real use -- one less thing to choose
between, and one less dependency on a separate addon for live channel
playback at all. `live_timeshift_mode`'s value `1` (what local used to
mean) was deliberately left unreused rather than renumbering server-side
down to `1`. **`live_timeshift_mode` itself was then removed too, once
server-side had proven stable through further real use** -- server-side
timeshift became unconditional for a while, with no setting at all.

**That turned out to be a real problem for anyone who doesn't want (or
can't get) an admin-level Dispatcharr account**: the companion plugin's
`run/` API requires one (`IsAdmin`, see below), so with no setting at
all, live TV playback failed outright for a non-admin account -- not
just pause/rewind, playback itself. `live_timeshift_mode` was
**reintroduced** as a result, defaulting to `2` (server-side, matching
what every install already had with no setting present, so existing
users see no change) with `0` (Off) as an explicit opt-out -- a plain
live stream via `STREAMURL`, no companion plugin, no admin account, no
pause/rewind. Value `1` (local) stayed retired at the time, not
reintroduced alongside it -- **superseded below**: it was brought back
ahead of 1.0 once the admin requirement above turned out to be
unloosenable.

**Default flipped from server-side (`2`) to Off (`0`) ahead of 1.0.** The
`2` default above was deliberately chosen at the time to match every
existing install's behavior from the unconditional-server-side era, so
reintroducing the setting wouldn't itself change anything for users who
already had it working. But `OpenLiveStream()` hard-fails every live
channel (returns `false`, no fallback) when the account isn't admin or
the plugin isn't installed -- exactly the state of a brand-new install
before anyone's done that extra setup, which is the common case now that
this project has no installed base yet to preserve continuity for. Off
now ships as the default: live TV works immediately with zero extra
setup, and server-side pause/rewind is an explicit opt-in once the admin
account and plugin are in place. Purely a shipped-default change --
`settings.xml`'s `<default>` only applies to a fresh profile, so anyone
who already has this addon installed keeps whatever value they already
have on disk regardless.

**Local (`live_timeshift_mode = 1`) reintroduced.** Investigating whether
the server-side admin requirement above could be loosened (it can't --
confirmed against Dispatcharr's current source, see the "permission
requirement" section below: it's a blanket restriction on every plugin's
`run/` API, not something this addon or its companion plugin can work
around) surfaced that Local fills the exact gap that leaves: real
pause/rewind for an account that isn't (or can't be) a Dispatcharr admin,
with zero Dispatcharr-side cooperation at all. It was only ever removed
for complexity reduction once server-side proved stable, not because it
was broken -- the code below (restored essentially unchanged from before
its removal, see git history) sets `inputstream.ffmpegdirect`'s
`stream_mode: timeshift`, landing on ffmpegdirect's own dedicated
`TimeshiftStream` class rather than the generic one that the snapshot
workaround further below hit a confirmed, unfixable seek bug in.

**Confirmed live, not just by inference from the code path being
different**: opened a real channel (CNN) under Local, `TimeshiftStream::
Start`/`DoReadWrite` in `kodi.log` confirmed ffmpegdirect actually
instantiated the dedicated class, `canseek: true`. A backward seek (30s)
logged `demuxer seek to: 5755.827300` / `..., success` with
ffmpegdirect's own `TimeshiftBuffer::Seek`/`TimeshiftSegment::Seek`
locating the exact segment and packet index, then a forward seek (30s)
back toward live succeeded the same way -- playback resumed cleanly both
times (`speed: 1`, no stuck `speed: 0`, no repeated identical failure the
way the snapshot path's `av_seek_frame` bug produced 4/4). Real pause/
rewind, working, no admin account, no server-side plugin.

**`inputstream.ffmpegdirect` declared as an optional addon.xml dependency,
not a required one.** Before this, a user could pick Local without
realizing the separate addon isn't installed, and only find out when
every live channel silently fails. Considered making it a *required*
`<import>` instead (Kodi would then auto-install it alongside this addon)
but traced Kodi's actual install path first
(`CAddonInstallJob::CheckDependencies()`/`Install()` in
`xbmc/addons/AddonInstaller.cpp`, confirmed against Kodi's current
source, not assumed): a required dependency that isn't available from any
of the user's enabled repositories fails the *entire* addon install, not
just the one feature that needs it -- confirmed this applies to a
zip-sideloaded install (`InstallFromZip()`) exactly the same as a
repository install, since both funnel through the same
`CAddonInstallJob`. That's a real risk for every installer, not just
Local-mode users, for a payoff that only helps Local-mode users, so
`optional="true"` instead: per the same source, Kodi's installer
completely ignores a missing optional dependency (doesn't install it,
doesn't block anything), so this is purely a documentation/discoverability
improvement -- Kodi now lists the relationship in this addon's own
metadata (confirmed live via `Addons.GetAddonDetails`: `"dependencies"`
includes `{"addonid": "inputstream.ffmpegdirect", ..., "optional": true}`,
`"broken": false`) -- not a behavior change. The help text's existing
warning ("must be installed or live channel playback fails outright")
remains the actual mechanism protecting a user from this mistake.

**Off** (`live_timeshift_mode = 0`): `GetChannelStreamProperties()` sets
`STREAMURL` directly to Dispatcharr's own live proxy URL
(`GetLiveStreamUrl()`), the same URL the plugin's own buffer reads from.
Kodi's generic `CCurlFile` opens it directly -- no inputstream addon, no
addon-side stream callback, no companion plugin, no elevated account.
Confirmed live: `canseek: false` (by design -- a plain stream has no
buffer to seek within), stable playback with real elapsed time
progressing and zero decode errors.

**A real bug, found by a user right after Off was reintroduced: changing
`live_timeshift_mode` via Kodi's settings GUI had no effect on an
already-running instance until Kodi was fully restarted.** Root cause,
confirmed directly in source: `PVRDispatcharr`'s constructor read every
setting exactly once (`kodi::addon::GetSettingInt(...)` etc.) into a
plain member, and nothing in this addon ever overrode Kodi's
settings-changed notification -- every consumer
(`GetChannelStreamProperties()`, `OpenLiveStream()`, ...) kept reading
that now-stale cached value indefinitely. Isolated cleanly (not just
inferred): started fresh with Off on disk, confirmed the Off-mode
property signature via a real `Play`; without restarting, edited
`settings.xml` to server-side (isolating "does the addon ever re-read
this" from whatever mechanism the GUI itself uses to notify the addon);
played the same channel again on the same still-running instance and got
the *identical* Off-mode signature -- proving the instance never re-read
it. **Silently defeated the entire point of the fix that just
reintroduced Off**: a user switching to it specifically because their
account isn't admin-level would still get an immediate playback failure
on the very first attempt after changing the setting, since the addon
kept trying the now-stale server-side/admin-only path until they figured
out to restart Kodi.

Fixed by implementing `kodi::addon::CAddonBase::SetSetting()` (in
`addon.cpp`'s `CAddonDispatcharr`, which Kodi calls once per changed
setting whenever the user edits addon settings via the GUI, without
restarting Kodi -- there's no per-instance equivalent wired into the PVR
C++ API, only this addon-base-level one, so `CAddonDispatcharr` tracks a
pointer to the `PVRDispatcharr` instance it created and forwards to a new
`OnAddonSettingChanged()` there). `live_timeshift_mode` and every other
setting this addon can safely apply without reconnecting
(`channel_refresh_hours`, `epg_refresh_hours`,
`enable_catchup_ffmpegdirect_seek`, `recording_refresh_minutes`,
`recurring_rule_utc_offset_minutes`, `debug_logging`) now take effect
immediately, no restart needed -- each was already read from more than
one thread (Kodi's own PVR-calling threads plus this addon's background
refresh threads), so each became `std::atomic` rather than plain,
matching this project's own established data-race-fixing standard rather
than introducing a new unsynchronized-write path deliberately. The
Dispatcharr connection settings (`host`/`port`/`use_https`/`username`/
`password`/`verify_ssl`/`timeout`/`api_key`, baked into
`DispatcharrClient`'s `Config` at construction) and
`enable_realtime_updates` (would need dynamically starting/stopping a
background thread outside its normal constructor/destructor lifecycle)
are deliberately left restart-only -- `SetSetting()` returns
`ADDON_STATUS_NEED_RESTART` for those specifically, rather than silently
doing nothing.

Verified the fix doesn't regress anything (both timeshift modes still
confirmed working correctly against a real build with every setting
above converted to atomic), and that `OnAddonSettingChanged()` correctly
stays silent at normal startup (doesn't fire spuriously just from Kodi
loading the addon's current settings) via a temporary diagnostic log
line, removed before landing. Neither this addon's own dev environment
nor the session that found the original bug had GUI automation available
(both API/log-only access) -- a disable/re-enable of the addon via
JSON-RPC was tried as a possible substitute trigger and ruled out
(confirmed via `kodi.log` timestamps that it fully destroys and recreates
the PVR client instance, equivalent to a restart, not a test of the
live-update path at all).

**Confirmed end-to-end afterward by the same user who found the original
bug, through Kodi's real settings dialog**: flipped `live_timeshift_mode`
both directions (Off -> Server-side and back) and played a channel each
time with no Kodi restart in between -- the new mode was picked up
immediately both ways, the exact scenario the original bug report
described, now clean. (A follow-up attempt to also drive that same
dialog via JSON-RPC synthetic input, purely to have an automated
supplement to the direct user confirmation, hit its own unrelated wall --
the AddonSettings dialog's content rendered blank under screencapture and
didn't respond to `Input.Down`/`Input.Select`, while the rest of Kodi's
UI navigated normally in the same session and the user's own real mouse/
keyboard interaction with that exact dialog worked cleanly -- read as a
JSON-RPC-synthetic-input limitation specific to that one dialog, not an
addon or fix problem, and not chased further given the direct
confirmation already in hand.)

**Follow-up bug in the live-apply mechanism itself, found via real
CoreELEC testing: any settings save at all silently restarted the PVR
client, defeating live-apply for every setting, not just the connection
ones.** Root cause: Kodi has a documented quirk where a settings-dialog
save's terminal `SetSetting()` call can arrive mislabeled with the name
of the *last* setting defined in `settings.xml` (`api_key`, in this
addon) even when nothing about that setting actually changed. Since
`api_key` is one of the settings `OnAddonSettingChanged()` treats as
always needing a restart, this spurious re-notification triggered a
restart on *every* save -- confirmed live, reproducibly: saving
`debug_logging` alone, isolated, with nothing else touched, restarted the
instance every time. Fixed by caching the addon's own last-known value
for each restart-triggering setting (`m_lastAppliedConfig` in
`PVRDispatcharr.h`) and only actually requesting a restart when the
incoming value genuinely differs from that cache -- a spurious
re-notification carrying an unchanged value becomes a no-op instead.
Confirmed live afterward: the identical isolated `debug_logging` toggle,
repeated, produced no restart at all.

**Local** (`live_timeshift_mode = 1`; removed once server-side proved
stable, then reintroduced -- see above): `GetChannelStreamProperties()`
routes live channel playback through `inputstream.ffmpegdirect`'s
`stream_mode: timeshift`. Confirmed
via ffmpegdirect's own README this is exactly what that mode is for:
adding pause/rewind to a plain, continuously-arriving live stream with no
backend cooperation required at all, by recording it to a local on-disk
buffer as it plays. The buffer lives on the Kodi device's own storage
(size/path/retention controlled by ffmpegdirect's own addon settings),
not on the Dispatcharr server, so it doesn't persist across a Kodi
restart and isn't shared between devices. Requires
`inputstream.ffmpegdirect` to be installed; if it isn't and this mode is
selected, live channel playback fails outright (not just timeshift).

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

  **Confirmed this is a blanket restriction, not specific to this
  plugin.** `apps/plugins/api_views.py`'s `PluginRunAPIView` resolves
  permissions via a `PluginAuthMixin` reading a hardcoded
  `permission_classes_by_method` table in `apps/accounts/permissions.py`
  -- `POST` (i.e. every `run/` action, for every plugin) maps to
  `[IsAdmin]` unconditionally, checked before the request ever reaches a
  plugin's own `run()`, so no plugin can loosen this for its own actions.
  This is a real gap compared to how Dispatcharr gates its own native
  recording playback (`RecordingViewSet`'s `file`/`hls` actions): those
  only require `dvr_access` of `view` or `manage` (`apps/channels/
  dvr_access.py`) -- and `view` is the *default* for any standard,
  non-admin account. Dispatcharr's permission model clearly already
  supports this finer-grained tier (`IsAdminOrDVRManager`/`IsDVRViewer`
  exist and gate the recording endpoints), it's just never been wired up
  to the plugin `run/` endpoint. Loosening this would need an upstream
  Dispatcharr change (e.g. letting a plugin declare a permission class per
  action); nothing on this addon's or plugin's side can work around it.

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

**Trade-off, later found to be worse in practice than described here, and
superseded -- see "Concurrent viewers" further down**: stopping and
restarting the buffer on every Open() means a second Kodi client (or
profile) watching the same channel concurrently would have its buffer torn
out from under it mid-playback -- the plugin's `start_buffer` is otherwise
idempotent specifically so multiple viewers can share one upstream
connection per channel (see the plugin's own README). This trades that
sharing away for correct per-session seeking, which is the right trade for
a single-viewer setup but worth knowing if this addon is ever used from
more than one Kodi client against the same Dispatcharr account at once.

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

**This section's own fix is itself superseded -- see "Concurrent viewers"
further down**: the `StopTimeshiftBuffer()` call this section added to
`CloseLiveTimeshiftStream()` turned out to have the same concurrent-viewer
problem as the seeking fix's own stop-before-start, and has since been
removed; kept here for the history. Real use surfaced one more gap: `CloseLiveTimeshiftStream()` (called on a
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

## Real hardware (CoreELEC/ODROID N2+) surfaced four more real bugs

Everything above was developed and confirmed against Windows/macOS. A
dedicated live-TV stress-testing pass on a real N2+ found four further
issues -- all confirmed live on that device, not theorized -- because
real embedded hardware and a real home network surface timing edge cases
a dev machine's faster CPU and LAN rarely hit.

**A multi-worker-process port race in the plugin's own HTTP server.**
Dispatcharr's plugin `run()` calls can land on any of its several WSGI
worker processes, but `_ensure_http_server_running()`'s own
`_http_server`/`_http_server_thread` tracking was a plain module-level
global -- invisible across processes, exactly the reason buffer *state*
already lives in Redis instead (see this file's `plugin.py` for that
comment). Confirmed live: a real instance repeatedly logged `couldn't
bind http server on port 9192: Address already in use` from workers other
than whichever one happened to bind first, and when that first worker
later died or got recycled (routine for a WSGI server), port 9192 went
briefly unserved until another worker won the race to rebind -- during
that gap, this addon's HTTP reads against the buffer's file server failed
outright, producing a real live-playback stall. Fixed with `SO_REUSEPORT`
on the plugin's listening socket: every worker process binds its own
socket on the same port, the kernel load-balances incoming connections
across all of them, and no single worker dying creates a gap -- safe
specifically because every worker's listener serves identical content
(the same shared `storage_path` files on disk).

**The catch-up-to-tail retry margin (1.5x the last segment's duration)
ran thinner than intended.** Confirmed live: ordinary, non-error catch-up
cycles routinely used 60-95% of that budget just to catch up under normal
jitter, not just during a real outage -- leaving too little real margin
before a read genuinely gave up and reported a stall to Kodi. Widened to
3x.

**Seeking to the live edge left zero buffer margin, causing a predictable
stutter almost every time.** `SeekLiveTimeshiftStream()`'s `SEEK_END`
clamp landed exactly at the known tail (`totalBytes`, off by under 1KB in
one traced case) -- confirmed live that this meant playback immediately
re-caught-up to the tail within a couple of seconds of real playback and
had to wait through a full segment-production cycle a second time, long
enough to trigger Kodi's own stall/rebuffer right after what looked like
a completed seek. Fixed by backing the clamp off by roughly one segment's
worth of bytes (`m_liveTimeshiftStream.segments.back().byteSize`) instead
of landing exactly on `totalBytes` -- the same "live edge minus a little"
margin real-world HLS/DASH players keep for this exact reason. Confirmed
live: the same seek-to-live sequence that previously stuttered noticeably
completed with no unreasonable delay and clean playback afterward.

**A genuine crash, traced to a single noisy sample sizing a
safety-critical budget.** The catch-up loop's retry budget was computed
from the *single last segment's own duration* -- fine under the original
6-second default, but after tuning `segment_seconds` down to 2 (see
below) a real instance produced one segment just 151ms long (ffmpeg's
segment cutter targets `segment_seconds` but cuts at the next keyframe
at/after it, so real durations vary run to run). That collapsed the
budget to 2 attempts / 0.5s -- nowhere near enough margin -- and the read
gave up for real, repeatedly, until ffmpeg's own demuxer read the
resulting silence as genuine end-of-stream and closed playback outright
(`VideoPlayer: eof, waiting for queues to empty`, then Kodi kicked back
to the main menu). Fixed by averaging the last 5 segments' durations
instead of trusting the single most recent one, with a 1.5s floor
regardless (covers a fresh buffer's still-warming-up first few segments
too). Confirmed live afterward: the same rewind-then-seek-to-live
sequence that previously crashed played cleanly, with sane, stable
segment-duration estimates in the log instead of one-off outliers.

## Concurrent viewers: the stop-on-Open/stop-on-Close fix above was itself a real bug

The "seeking after a Stop/reopen" fix a few sections up traded away
concurrent-viewer support deliberately (see its own "Trade-off" paragraph)
-- but real multi-device use surfaced that the actual consequence was worse
than "the first viewer loses pause/rewind": **a second viewer opening the
same channel killed the first viewer's playback outright, and the first
viewer's own eventual Stop then killed the second viewer's replacement
buffer too**, leaving both viewers broken in sequence rather than one.
Confirmed live: watching ESPN (1080p) on a Mac, then opening the same
channel on a second, separate device (a Rocky Linux laptop) -- the Mac's
playback stopped, and the second device's own playback stalled
indefinitely a few seconds later. Dispatcharr's own `list_buffers` plugin
action showed zero active buffers afterward (the second device's own fresh
buffer had also been torn down), and the second device's `kodi.log` showed
a 20+ second gap with zero addon debug output between `VideoPlayer::OpenFile`
and an eventual `stream stalled`/`CloseFile` -- consistent with the addon
blocking inside a network call (most likely `CURLOPT_TIMEOUT`,
`m_config.timeoutSeconds`, default 30s) after its own buffer was killed
out from under it by the first viewer's Close.

Root cause: `OpenLiveTimeshiftStream()` unconditionally called
`StopTimeshiftBuffer()` before every `StartTimeshiftBuffer()` (the seeking
fix above), and `CloseLiveTimeshiftStream()` unconditionally called
`StopTimeshiftBuffer()` too (the "buffer teardown was slow to notice a
Stop" fix above) -- neither call site had any way to know whether another
viewer was still using the same channel's buffer, so each one's
"just tearing down my own stream" was actually "tearing down *the*
buffer, unconditionally," for however many viewers happened to be using
it.

**First fix attempted, and confirmed live NOT to work on its own:** the
working theory was that the failure the original fix prevented wasn't
really "a fresh demuxer instance can never seek into content it hasn't
personally read this session," but rather a *stale, discontinuous* buffer
(content from a much earlier, long-since-restarted or partially
plugin-side-evicted encoder run, with its own incompatible PTS timeline) --
and that simply no longer stopping the buffer at all (reattaching via
`StartTimeshiftBuffer()`'s existing idempotent `start_buffer`, which already
reports back an already-running buffer rather than restarting one) would
keep the whole addressed byte range one continuous, valid encoder run and
fix concurrency for free. **Tested live and confirmed wrong**: opened ESPN
(1080p) on Windows, played ~20s, Stop, waited ~22s, reopened the same
channel -- the reopen reattached to the still-running buffer instantly (no
cold-start wait; the manifest already had ~90MB/several minutes of history
from before this reopen, direct evidence the buffer had genuinely never
been restarted), then a plain `Player.Seek {"seconds": -20}` reproduced the
*exact same failure signature* the original fix was written to prevent:

```
SeekLiveTimeshiftStream(position=0, whence=0) from current=125098280, totalBytes=126204964 -> newPos=0
SeekLiveTimeshiftStream(position=564, whence=0) from current=524288, totalBytes=126204964 -> newPos=564
CDVDDemuxFFmpeg::SeekTime - seek ended up on time 95376117
```

`95376117` ms is ~26.49 hours -- unmistakably the MPEG-TS 33-bit PTS
wraparound point (2^33 / 90kHz ≈ 26.51h), not anywhere near a plain -20s
target. Decoding didn't stall (`speed: 1`, `Player.Duration` kept growing
normally throughout), but the reported playback position was garbage --
the demuxer had landed near byte 0 of this addon's own *entire* local
address space (all ~126MB/several minutes this reopen's cold-start fetch
had pulled in from the still-running buffer), not anywhere near -20s from
where playback actually was. **This confirms the original diagnosis was
right after all**: Kodi's own demuxer genuinely cannot reliably seek
backward into buffer content *this* demuxer instance hasn't itself read
forward through this session, regardless of whether that content belongs
to one continuous, never-restarted encoder run -- continuity alone doesn't
fix it. (It also directly answers, live, the "can a second viewer join a
running buffer and rewind into its pre-join history" question this was
investigated alongside: no, not with Kodi's current PVR API and generic
ffmpeg MPEG-TS seek -- the exact same mechanism that breaks a *single*
viewer's own reopen would break a second viewer's join identically.)

**The actual fix**: keep not stopping the server-side buffer (still
correctly fixes the concurrency bug -- see below), but after the
cold-start manifest fetch populates this addon's own local address space
from *everything* the buffer currently holds, `OpenLiveTimeshiftStream()`
now discards all but the trailing `kLiveEdgeMarginSegments` (3) segments of
it and rebases the byte/time offsets of what's kept so the oldest
surviving segment becomes local byte 0 -- i.e., this addon's own exposed
address space is trimmed back down to the same small, near-live-edge
window a genuinely fresh (just-restarted) buffer would have had, matching
exactly what made seeking reliable before, without ever telling the plugin
to stop or restart anything server-side. As playback continues past that
point, new segments accumulate locally via the normal sequence-based merge
in `RefreshLiveManifest()`, unaffected -- within-session backward seeking
into everything read *since* this Open() keeps working exactly as it always
has; only the pre-existing history from before this particular Open() is
no longer exposed.

`OpenLiveTimeshiftStream()` also no longer calls `StopTimeshiftBuffer()`
before `StartTimeshiftBuffer()` -- `start_buffer` is idempotent (reattach
if already running, start fresh if not), matching the design it was always
meant to support ("idempotent specifically to let multiple viewers share
one upstream connection per channel," per its own original comment) --
this addon's own stop-on-Open call was what had been overriding it.

**`CloseLiveTimeshiftStream()` no longer calling `StopTimeshiftBuffer()` at
all, relying purely on the plugin's heartbeat-driven idle-timeout reaper
for all cleanup, is itself superseded -- see "Provider concurrent-stream
limits" further down** for a real bug that design caused and the viewer
reference-counting fix that replaced it. Kept here for the history: joining
an already-running buffer and rewinding into history from before that join
is confirmed **not achievable** with Kodi's current PVR API (no `IPosTime`
hook on `CInputStreamPVRBase`, confirmed earlier in this file) and generic
ffmpeg MPEG-TS byte-domain seeking -- the trim above is what makes a
second viewer's join safe *for the first viewer*, not a way to grant the
second viewer any extra rewind range.

**Confirmed live on Windows after the trim fix**: the same Stop -> wait
~22s -> reopen -> `Player.Seek {"seconds": -20}` sequence above, repeated
against the trimmed design, this time landed on `CDVDDemuxFFmpeg::SeekTime
- seek ended up on time 100` (100ms -- a real, sane, small value) instead of
the ~26.5h wraparound garbage, with `Player.Time`/`Player.Duration` (via
`XBMC.GetInfoLabels`) correctly showing a small buffer-relative `00:02`/
`00:30` right after the seek, `canseek: true`, `speed: 1` throughout, and
`Player.Time` continuing to progress normally (00:02 -> 00:27 over the next
10 real seconds) with no stall or EOF in the log afterward.

**Confirmed live across two real, separate devices** (Windows and a Rocky
Linux laptop, the same two-device setup that originally surfaced this bug),
exercising the actual scenario rather than a same-instance stand-in:
Windows opened ESPN (1080p) and played cleanly (`speed: 1`, `Player.Time`
progressing normally); with Windows still playing, Rocky opened the *same*
channel -- Rocky started playing cleanly, and Windows's own `kodi.log`
showed no `ClosePVRStream`, no stall, and `Player.Time` continuing to
advance in real time throughout (previously, this exact step killed the
first viewer outright). Windows was then stopped while Rocky kept
playing -- Rocky's own playback continued unaffected (`speed: 1`,
`Player.Time` still advancing, no stall/EOF in its log) and a further
`-20s` seek on Rocky landed correctly with no PTS-wraparound garbage and
playback continuing afterward (previously, this exact step killed the
second viewer's replacement buffer). Both halves of the original cascading
failure are confirmed fixed.

(Getting to this point on the Rocky Linux side also surfaced two purely
environmental issues, unrelated to this addon's code: a stale relaunch
script had grabbed display environment variables from `gnome-shell`'s own
process, which doesn't carry `WAYLAND_DISPLAY` -- fixed by sourcing it from
a real Wayland client process instead (`pipewire`, in this case) -- and
Kodi's local PVR cache database (`TV46.db`) had become unable to open
(`SQLITE_CANTOPEN`), which blocked `PVR.GetChannels` entirely until the
file was renamed aside and Kodi rebuilt it fresh on next launch. Neither
is a bug in this addon; noted here only because they blocked testing.)

**Not a bug, and expected: Dispatcharr's own Stats page only shows one
active client for a channel with several concurrent Kodi viewers.**
Confirmed real, via a genuine three-way test (ESPN 1080p played
simultaneously on Windows, macOS, and Rocky Linux, all with clean
playback) -- Dispatcharr's Stats page showed only the first (Windows)
device as active; macOS and Rocky Linux never appeared. Root cause is the
same shared-buffer design this whole section is about: a later viewer's
`start_buffer` call finds a buffer already running and just refreshes its
heartbeat (`plugin.py`'s `_start_buffer()`, the `existing` early-return
branch) -- it never re-invokes `_start_ffmpeg()`, which is the only place
`_stream_attribution_headers()` (the `username`/`client_ip` passed to
Dispatcharr's own live proxy) is actually applied. So there is only ever
one real upstream connection to Dispatcharr per channel no matter how many
Kodi viewers are sharing it, permanently attributed to whichever viewer's
Open() happened to create the buffer -- Dispatcharr's Stats page reflects
that one real connection, not the plugin's own local HTTP server's
separate, unrelated set of viewers reading segment files from it. Not
something to fix: re-attributing on every reattach would need the plugin
to track and periodically refresh a *set* of attributions per buffer
rather than one fixed pair, adding real complexity for a page that's
diagnostic/informational only -- nothing about playback, timeshift
correctness, or this addon's own behavior depends on Dispatcharr's Stats
page reflecting every concurrent viewer.

## Provider concurrent-stream limits: relying on the idle-timeout reaper alone was a real bug

The idle-timeout-reaper-only cleanup design a few sections up (no explicit
stop on Close, buffer torn down only once its heartbeat goes stale) traded
away *fast* cleanup deliberately (see its own "Trade-off" paragraph) --
but real use surfaced that this was a genuine bug for anyone whose
upstream provider caps concurrent streams, not just a slower version of
the same behavior. **Confirmed real**: with a provider limited to 3
concurrent streams, 2 already in use by in-progress recordings, and the
3rd by a live channel being watched -- stopping that live channel, then
immediately switching to a *different* channel, played nothing at all.
Dispatcharr's own status page kept showing the *original* channel's
buffer as the active 3rd stream (accurately -- it genuinely hadn't been
torn down yet, unlike the earlier, unrelated "Stats page shows a stale
client" question above, which really was just attribution/UI, not a
still-running buffer), so there was no free slot left for the new
channel to start with, and it failed outright. Waiting out the idle
timeout would eventually have freed it, but "de facto only one live
channel switch every 2 minutes" isn't a real fix for a provider-limited
account.

**The fix**: the plugin now reference-counts viewers per buffer instead of
relying solely on the heartbeat idle-timeout. `StartTimeshiftBuffer()` now
sends a `viewer_id` -- a random per-`Open()`-session token generated by
`OpenLiveTimeshiftStream()` (`LiveTimeshiftStreamState::viewerId`), unique
enough to not collide between concurrent viewers of the same channel, not
meant to be anything more -- which the plugin's `start_buffer` action adds
to a `viewers` list on the buffer's own state (a plain JSON list, not a
set, since state round-trips through Redis as JSON). `StopTimeshiftBuffer()`
is reintroduced (removed briefly by the previous fix once nothing called
it) and now called from `CloseLiveTimeshiftStream()` again, on every
Close(), passing that same `viewer_id` -- but its plugin-side `stop_buffer`
action no longer stops anything unconditionally: it removes just that one
`viewer_id` from the list, and only proceeds to actually stop the
underlying ffmpeg process (and delete the buffer state) once the list is
empty. A caller with no `viewer_id` at all (an older, un-upgraded addon
build, or a manual click of the plugin's own "Stop Test Buffer" button,
which calls `run()` with empty params) can't be tracked, so it always
falls through to the unconditional stop -- the same behavior `stop_buffer`
has always had for such callers, not a regression, since there was never
a way to reference-count them. The heartbeat idle-timeout reaper is
unchanged and still runs as a backstop, for a viewer that disappears
without cleanly calling Close at all (a crash, a network drop, force-quit).

This directly restores what the very first version of this addon's
concurrent-viewer handling got right (fast, ~12s cleanup) while keeping
what the trim-based seeking fix and the earlier reaper-only design each
got right in turn (no viewer's buffer torn out from under it by another
viewer's Open() or Close()) -- the three together are the complete,
correct design, not competing alternatives.

**Confirmed live**, reproducing the actual two-device scenario end to end,
not just the mechanism in isolation: Windows opened ESPN (1080p);
`list_buffers` showed `"viewers": 1`. Rocky Linux opened the same channel
while Windows kept playing; `list_buffers` showed `"viewers": 2`, both
devices still playing cleanly. Windows stopped -- Rocky's own playback
was completely unaffected (`speed: 1`, no stall) and `list_buffers` showed
`"viewers": 1`, buffer still running, *not* torn down. Rocky then stopped
too -- `list_buffers` returned `"buffers": []` within about 5-13 seconds
(two separate single-viewer stop/reap timings measured this way during
testing), nowhere near the old 120s idle-timeout wait. A follow-up Windows
reopen of the same channel afterward still played cleanly, confirming
nothing about ordinary single-viewer playback regressed.

## The reference-counted stop above still had a race -- switching channels could fail outright

The reference-counting fix just above was itself detached (a background
thread, matching how `CloseLiveTimeshiftStream()` had called
`StopTimeshiftBuffer()` before reference counting ever existed) -- real use
surfaced that this reintroduced a *different* provider-concurrent-stream-limit
failure, not fixed by reference counting at all. **Confirmed live**: with a
provider limited to 3 concurrent streams, 2 already used by in-progress
recordings, and the 3rd by NHL Network being watched live -- switching
directly to MLB Network (no explicit Stop in between, just picking a
different channel, which is exactly what "watch a live channel, then start
watching a different one" is from Kodi's own PVR API's perspective: a
`CloseLiveStream()`/`ClosePVRStream()` for the old channel immediately
followed by `OpenLiveStream()`/`OpenPVRStream()` for the new one) played
nothing at all and Kodi returned to the main menu outright. NHL Network's
own stream *did* visibly go down in Dispatcharr's status a few seconds
later -- correctly torn down by the reference-counting fix -- but only
*after* MLB Network had already failed to start, not before.

Root cause: this addon's own `CloseLiveTimeshiftStream()` handed the actual
`StopTimeshiftBuffer()` network call off to a detached background thread
and returned immediately, so Kodi's own next call --
`OpenLiveTimeshiftStream()` for MLB Network, made essentially back to back
with the Close() that just returned -- reached the plugin's `start_buffer`
and tried to open a *4th* upstream connection to the provider while NHL
Network's connection (2 recordings + NHL Network = the provider's real
limit of 3) hadn't actually been torn down yet. The provider naturally
refused it, `start_buffer` failed, and `OpenLiveTimeshiftStream()` had no
retry budget for *that* kind of failure (its existing cold-start retry
loop only covers `RefreshLiveManifest()` after a *successful*
`StartTimeshiftBuffer()`, for the ordinary "buffer just started, give
ffmpeg a moment to produce a first segment" case -- a hard failure to even
start the buffer at all was never something it retried).

**The fix**: `CloseLiveTimeshiftStream()`'s `StopTimeshiftBuffer()` call is
no longer detached -- it's a plain, synchronous call again, blocking
Kodi's own calling thread until it returns. This is safe to do (not a
regression to the very first, unconditional-stop design's own blocking
call) specifically *because* of the reference-counting fix above: when
other viewers remain, the call returns almost immediately (just a Redis
write to deregister one viewer id, no ffmpeg process to wait on); it only
takes real time when this genuinely was the last viewer, which is exactly
the case where the next operation (a different channel needing a free
provider slot) actually depends on the teardown having completed. The
plugin's own `_stop_ffmpeg()` (`plugin.py`) already blocks until the
ffmpeg process is confirmed dead (SIGTERM, poll every 200ms, escalate to
SIGKILL after a 5s deadline) before its HTTP response returns, so a
synchronous call here means Kodi's own sequential
`ClosePVRStream()`-then-`OpenPVRStream()` calling convention is what
actually guarantees the old channel's provider slot is free before the
new channel's own `Open()` ever asks for one -- no additional
synchronization needed on this addon's side beyond just not detaching the
call. Kodi's calling thread already blocks synchronously on comparable
network I/O elsewhere in this same class (every live-timeshift read/seek/
manifest call already does), so this isn't a new category of blocking for
it, just this one call site catching up to that existing pattern.

**Confirmed live**: the exact failing sequence above, repeated against the
synchronous fix -- opened NHL Network, confirmed one registered viewer via
`list_buffers`, then switched directly to MLB Network. `kodi.log` showed
`ClosePVRStream` for NHL Network's own stream path, then `OpenPVRStream`
for MLB Network's **2.3 seconds later** on the same log thread -- direct
evidence `CloseLiveTimeshiftStream()` was genuinely blocking on the
teardown, not returning immediately. MLB Network started and played
cleanly (`speed: 1`), and `list_buffers` immediately afterward showed
exactly one buffer -- MLB Network's own, with one registered viewer --
NHL Network's buffer gone entirely, no leftover state. (This particular
run didn't have 2 real recordings competing for provider slots alongside
it, so it directly confirms the race itself is closed -- the ordering and
timing that caused the original failure -- rather than re-proving the
provider-limit scenario end to end a second time; the mechanism is the
same either way.)

**Tuning note, not a bug: `segment_seconds` trades burst size for file
count.** The plugin's buffer is built from *closed* HLS-style segments --
ffmpeg only exposes a segment once fully written, so content arrives in
bursts sized by `segment_seconds`, not a smooth trickle, no matter how
generous the client's own retry margin is. Reducing it shrinks both the
burst size and the worst-case wait per cycle, measurably reducing stall
frequency/severity in live testing -- at the cost of more, smaller
segment files on disk and more frequent requests, exactly the trade-off
the setting's own help text documents. A deeper fix -- serving the
*currently-being-written* segment for partial reads instead of waiting
for it to close, eliminating the burstiness at the root -- would need a
real redesign of the plugin's manifest/addressing model (which currently
assumes a segment is stable once listed) and hasn't been attempted.

**Default changed from 6s to 2s**, after the value had already been
running live at 2s on a real instance for some time (left over from the
CoreELEC/ODROID testing pass above) with no issues surfacing, and after a
dedicated multi-buffer smoke test at the new default: 4 channels started
buffering concurrently via direct plugin API calls (`max_concurrent_buffers`'
own default), polled repeatedly over ~35s. 3 of the 4 produced steady,
error-free segment growth throughout (~3 new segments every 5s, matching
`segment_seconds=2` exactly) -- the 4th failed, but for an unrelated
reason confirmed via Dispatcharr's own `/proxy/ts/status/<uuid>`
(`Channel ... not found`, meaning that specific alternate-source channel
never even registered an upstream client at all -- a real provider-side
connection failure, not something segment length affects). The crash risk
that made 2s dangerous in the first place (the catch-up-retry-budget
collapse from a single short segment, described above) is already fixed
independently by that same section's segment-duration-averaging change,
which is what made flipping the default safe to consider at all.

## A provider's own concurrent-stream limit took ~15s to fail instead of failing fast

The 4-channel smoke test above surfaced a real, separate gap while
diagnosing why its 4th buffer never produced a segment: `start_buffer`
reports success as soon as it manages to *spawn* ffmpeg, not once ffmpeg
has actually confirmed a working upstream connection -- so a buffer that
will never succeed (most commonly because an upstream provider's own
concurrent-stream limit is already fully used, and Dispatcharr's live
proxy refuses the connection ffmpeg is reading from) looked identical, to
every caller, to a buffer that's simply still cold-starting. Two real
consequences followed from that, both now fixed:

1. **`OpenLiveTimeshiftStream()`'s cold-start retry loop had no way to
   tell the two apart**, so opening a channel that was genuinely (not
   racing against something about to free up -- that's the already-fixed
   synchronous-close race further up) pinned at a provider's limit still
   burned its full ~15s retry budget before failing, with a generic "not
   ready yet" message giving no indication why.
2. **A dead buffer could zombie forever.** `start_buffer`'s reattach path
   (`existing = _get_buffer_state(...)`) never checked whether the buffer
   it was reattaching to was actually still alive -- every subsequent
   `start_buffer` call against that same dead channel kept refreshing its
   `last_heartbeat`, which both kept reporting false success to callers
   *and* kept the idle-timeout reaper from ever reaping it (it never went
   idle), since nothing was actually fetching from it to notice. A channel
   that hit this once would silently fail the same way for every future
   viewer, indefinitely, until someone noticed and called `stop_buffer` by
   hand.

**The fix**: `_get_live_manifest()` now checks whether the buffer's tracked
ffmpeg process is still alive (`os.killpg(pid, 0)`, the same cross-worker
-process-safe existence check `_stop_ffmpeg()` already used) before
assuming "just cold-starting." If it's already exited, it raises a new
`BufferFailedError` (a `RuntimeError` subclass) carrying the tail of that
buffer's own `ffmpeg.log` for real diagnosis, `_get_live_manifest_action()`
catches it specifically and returns `"fatal": true` alongside the message
(and self-heals -- stops/removes/deletes the dead buffer's state right
there, rather than waiting for anything else to notice), and
`start_buffer`'s own reattach path runs the same liveness check up front,
treating a dead "existing" buffer exactly like no buffer at all instead of
reattaching to it. On the addon side, `RefreshLiveManifest()` gained an
optional `fatalOut` parameter that surfaces that flag, and
`OpenLiveTimeshiftStream()`'s cold-start loop breaks immediately when it's
set instead of continuing to retry.

**Confirmed live in two stages.** First, deterministically, against a
channel_uuid that doesn't exist in Dispatcharr at all (guaranteeing
ffmpeg fails immediately, independent of the real provider limit's
own -- confirmed separately -- inconsistent reproducibility): `start_buffer`
reported success (ffmpeg spawned), the first `get_live_manifest` call
returned `"fatal": true` with the exact real cause (`HTTP error 404 Not
Found`, `Error opening input file http://127.0.0.1:9191/proxy/ts/stream/
<uuid>`), a second call immediately afterward found the buffer already
gone (self-heal confirmed), and a fresh `start_buffer` against the same
channel started genuinely new rather than reattaching to a zombie. Second,
against the real, originally-reported condition: 3 real in-progress
recordings plus an attempt to play a live channel, with the provider's own
concurrent-stream limit already fully used by those 3 -- failed in about
5 seconds, down from the ~15s this fix was written to address.

## A plain Stop took ~5s, traced to ffmpeg's own slow SIGTERM response

Real use surfaced one more real delay, this time on the ordinary Stop
path rather than a failure path: pressing Stop on a single, ordinary live
channel (no concurrency, no provider limit involved at all) took about 5
seconds -- expected to be near-instant, since `CloseLiveTimeshiftStream()`
being synchronous (see "The reference-counted stop above still had a
race" further up) means its own duration is now directly what a user
feels pressing Stop.

Root-caused using Dispatcharr's own server-side log, not guessed:
timestamps for `live_proxy`'s own teardown (client disconnect ->
stream manager stop -> provider connection closed -> Redis keys cleaned
up) spanned under 40ms once it noticed ffmpeg's connection had dropped --
Dispatcharr's own side was never the bottleneck. What actually took the
time was the gap *before* that: `uwsgi_response_write_body_do(): Broken
pipe ... during GET /proxy/ts/stream/<uuid> (127.0.0.1)` -- Dispatcharr
mid-write to ffmpeg's own socket, discovering ffmpeg had already closed
its end -- landed almost exactly 5 seconds after Stop was pressed, the
same 5s deadline `_stop_ffmpeg()`'s own SIGTERM-then-SIGKILL escalation
used at the time. Confirmed via the plugin's own logging (its only log
line in that function is the SIGKILL warning, and the user confirmed no
such line appeared anywhere in what they could see of the log) that
ffmpeg exited cleanly on `SIGTERM` alone, well within the deadline --
just not *promptly*. No crash, no error, SIGKILL was never needed;
ffmpeg (reading a live HTTP MPEG-TS stream and writing plain copied
segment files, `-c copy`) simply isn't quick to act on a `SIGTERM` here,
for a reason not chased further (would need shell access to the
Dispatcharr container itself to actually trace ffmpeg's own signal
handling, e.g. strace or comparing SIGINT vs SIGTERM behavior -- not
available in this investigation).

**The fix, deliberately a mitigation rather than a root-cause fix**:
shortened `_stop_ffmpeg()`'s SIGTERM-to-SIGKILL escalation deadline from
5s to 2s. Safe regardless of why ffmpeg is slow to respond, specific to
this pipeline: a plain stream copy with no re-encoding has nothing
meaningful to lose from a more abrupt kill -- this plugin's own manifest
(`_get_live_manifest()`) only ever exposes segments ffmpeg has already
fully closed, so a segment truncated mid-write by SIGKILL was already
invisible to every client and gets cleaned up/overwritten normally
either way. Also added a debug-level log line on the clean-exit path
(previously silent -- only the SIGKILL warning branch logged anything at
all), specifically so a future investigation like this one has the
actual elapsed time on hand directly instead of needing to cross
-reference Dispatcharr's own live_proxy log timestamps by hand.

**Confirmed live, in two rounds** -- the first showed the fix hadn't
actually taken effect (~6.8s, barely different from before): this
plugin's own README already documents that a plain restart doesn't
reliably make already-running workers pick up new code (the background
HTTP file-server thread survives it), and this was a real instance of
exactly that gotcha, not a flaw in the fix itself. After a full
Dispatcharr restart (which does reliably reload everything), the same
Stop measured at ~2.2s (`ClosePVRStream` timing in `kodi.log`, cross
-checked against Dispatcharr's own `04:36:29,197 WARNING ... didn't exit
after SIGTERM, sending SIGKILL` line) -- confirming the shorter deadline
is what actually fired, and that this pipeline tends to need the full
escalation rather than exiting cleanly on `SIGTERM` alone.

**A further, real gap found via that same log line, deliberately left
open.** Dispatcharr's own upstream provider connection wasn't actually
closed until `04:36:35,322` -- almost 6 seconds *after* the SIGKILL that
let this addon's own `Close()` call return at `04:36:29.216`. Root cause:
`_stop_ffmpeg()` only confirms the *local* ffmpeg process is dead; it has
no visibility into whether Dispatcharr's own `live_proxy` has separately
noticed and released the upstream connection. Traced (not guessed) to a
structural property of Dispatcharr's own response generator: it only
discovers a broken pipe on its *next* write attempt, paced by the live
stream's own data arrival, not by anything this addon or plugin controls
or could poll faster than. Deliberately not fixed further: actually
closing this gap (waiting for Dispatcharr to confirm the connection is
released before `stop_buffer` returns) would mean blocking for exactly
that same Dispatcharr-side write-cadence delay -- undoing the
responsiveness fix above, not improving it. Accepted instead: the residual
window is narrow (a channel switch has to land inside those few seconds,
*and* the provider has to be at its hard limit already), and when it is
hit, the fatal-detection fix from the section above already makes it fail
fast and clearly rather than hanging or silently misbehaving -- a
reasonable place to stop, not a gap this addon can close without either
reintroducing the delay or reaching into Dispatcharr's own internals.

## `idle_timeout_seconds` default lowered from 120s to 30s

Made sense as 120s back when the idle-timeout reaper was the *only*
cleanup mechanism a buffer ever had (before viewer reference counting
existed, above) -- worth being generous about, since going idle
prematurely meant actually losing a still-wanted buffer. It no longer
carries that weight: a clean Stop now tears the buffer down directly and
fast (~2s, see the SIGTERM/SIGKILL section above), regardless of this
setting. What's left for the idle timeout to actually do is purely a
backstop for a viewer that disappears *without* cleanly closing (a
crash, a force-quit, a network drop) -- and for that job, 120s means an
abandoned buffer can sit occupying one of a provider's own
concurrent-stream slots for up to two minutes, the exact failure mode
already fixed for the clean-Stop path.

Lowered to 30s. Not expected to risk killing a real, actively-watched
session, including a paused one: `GetStreamTimes()` is polled by Kodi's
own player-state machine on a regular interval regardless of play/pause
state (this addon's own growing-duration live-timeshift design already
depends on that being true), and that call refreshes the heartbeat on
this addon's own side via its throttled `RefreshLiveManifest()` -- so a
genuinely open session keeps its own heartbeat fresh without needing
active playback specifically. Not yet confirmed live (would need an
actual crashed/killed client left running long enough to watch the
reaper reclaim it, and a genuinely long pause to confirm the heartbeat
really does stay fresh throughout) -- both worth doing before trusting
this fully, consistent with this project's own standard.

## A buffer that died mid-playback retried forever with no indication anything was wrong

Found via a comparative-architecture review against `pvr.hts`/Tvheadend
(HTSP has explicit stalled-stream detection this addon's own read loop
didn't). `RefreshLiveManifest()`'s `fatalOut` parameter -- set when the
plugin confirms its own ffmpeg has exited and this buffer will never
produce another segment, most commonly a provider's own concurrent-
stream limit rejecting the connection -- was only ever wired up in
`OpenLiveTimeshiftStream()`'s cold-start retry loop. Every steady-state
call during actual playback (`ReadLiveTimeshiftStream()`'s catch-up-to-
tail loop, `SeekLiveTimeshiftStream()`, `GetLiveTimeshiftStreamLength()`)
passed no `fatalOut` at all.

So if the buffer died *after* playback was already under way -- the same
kind of failure the cold-start path already detects and gives up on
immediately, just happening later -- `ReadLiveTimeshiftStream()`'s
catch-up loop had no way to tell that apart from an ordinary "just
waiting for the next segment" gap. It retried its full bounded budget on
every single `Read()` call, forever, always returning 0 (Kodi's "no data
yet, keep waiting" signal), with nothing worse than a debug-level "gave
up" log line. To a user, that's indistinguishable from playback frozen
indefinitely with no error, no explanation, and no way for Kodi to
notice and give up on its own.

Fixed by wiring `fatalOut` into the steady-state catch-up loop too, and
adding `LiveTimeshiftStreamState::fatal` -- once a steady-state refresh
confirms the buffer is genuinely dead, it's logged at `ADDON_LOG_ERROR`
(not just debug, since this is a real failure worth surfacing) and
`ReadLiveTimeshiftStream()`/`SeekLiveTimeshiftStream()` return `-1`
(this codebase's established hard-error convention, e.g.
`ReadRecordingStream()`'s own `curl_easy_init()`-failure path) instead of
`0`, which is what actually lets Kodi's own player recognize the stream
has genuinely ended rather than continuing to wait on it. `fatal` is
checked up front on every subsequent call too, so a confirmed-dead
buffer short-circuits immediately rather than paying for another doomed
network round trip each time.

Confirmed live (Windows, server-side timeshift mode): the *normal*,
non-fatal catch-up loop behaves identically to before this change --
same log format, same successful "caught up" cycles, no regression.
The fatal path itself wasn't independently live-triggered (would need
deliberately killing the plugin's ffmpeg process mid-playback on the
server, not done this pass) -- confidence here comes from reusing the
exact same `fatalOut` mechanism already proven correct for the
cold-start case, not a fresh, untested code path.

### A crashed viewer's own reference-count entry never got cleaned up, silently defeating fast teardown

Found via a comparative architecture review of this plugin's own
implementation (not a user report). `plugin.py`'s viewer reference
counting (`_start_buffer`/`_stop_buffer`, see the "Concurrent viewers"
section above) tracks who's watching in a plain `viewers` list, but
liveness itself -- `last_heartbeat` -- was buffer-wide, not per-viewer:
*any* successful file fetch from *any* viewer refreshed the same single
timestamp (see `_touch_heartbeat`'s own comment for why that design
exists at all -- it's what lets a client with no viewer_id lifecycle,
like plain `inputstream.ffmpegdirect` passthrough, still work as a
liveness signal).

That's fine for the buffer-wide idle reaper, but it meant a specific
viewer's own entry in the `viewers` list was never independently
checked for staleness. If one viewer (device A) crashed hard enough to
never call `stop_buffer` -- a force-quit, a network drop, a power loss,
exactly the case the idle-timeout backstop exists for -- its `viewer_id`
just sat in the list forever, kept looking "alive" by device B's own
ordinary segment fetches continuing to refresh the buffer-wide
heartbeat. When B *later* stopped cleanly, `_stop_buffer` removed B's
own id, saw A's phantom id still present, and returned "viewer removed;
buffer still active for other viewers" -- so the underlying ffmpeg
process was never actually stopped and the provider's concurrent-stream
slot stayed occupied, even though nobody was left watching. That
silently defeated the entire point of the reference-counted fast-
teardown fix (and the provider-concurrent-stream-limit fast-fail fix
that depends on slots actually freeing up promptly), for precisely the
failure mode -- an ungraceful client exit -- most likely to trigger it.

Fixed by tracking a last-seen timestamp *per viewer_id*
(`viewer_heartbeats`, a parallel dict alongside `viewers`), not just one
buffer-wide value. `_prune_stale_viewers()` drops any viewer whose own
last-seen exceeds `idle_timeout_seconds`, called from `_stop_buffer()`
right before it decides whether the buffer is still in use by anyone
else, and from the reaper loop on every tick for ongoing hygiene (so
`list_buffers`'s reported viewer counts stay accurate too, not just the
teardown decision). A viewer with no recorded heartbeat yet -- state
written by a pre-upgrade plugin version, or a `start_buffer` call that
landed the same instant -- is treated as fresh as of "now", not already
stale, so a rolling upgrade can't mass-prune viewers that simply haven't
had a chance to report in.

Per-viewer liveness needs an explicit signal, since ordinary segment/
playlist fetches carry no `viewer_id` at all (they're plain HTTP GETs --
the plugin has no way to attribute one to a specific caller). The
`heartbeat` action already existed but was previously undifferentiated
(refreshed only the buffer-wide timestamp); extended to also accept an
optional `viewer_id` and refresh that viewer's own entry. The addon side
(`pvr.dispatcharrai`) now calls it periodically -- every 10s, piggybacked
on `ReadLiveTimeshiftStream()` rather than a dedicated thread, since
that function already runs continuously for as long as playback
continues -- comfortably under the 30s default `idle_timeout_seconds`,
so a genuinely-still-watching viewer's own entry never goes stale
between heartbeats. A client that doesn't send one (an older addon
build, or any client using plain passthrough with no viewer_id at all)
just doesn't participate in per-viewer pruning, same as it never
participated in reference counting to begin with -- no regression for
that case, since the buffer-wide heartbeat mechanism is untouched.

Verified via isolated logic testing (the crashed-viewer scenario, the
ordinary both-fresh case, and the pre-upgrade-state case that must NOT
mass-prune, all confirmed to behave correctly) and confirmed live that
the addon-side change compiles and the addon loads and runs normally
(Windows, `Addons.GetAddonDetails` reports `broken: false` after
reload). The server-side half needs the updated plugin redeployed to a
live Dispatcharr instance to exercise end-to-end (a real second-viewer-
crash scenario isn't something this pass triggered against a live
buffer) -- not yet done as of this writing.

### The plugin's own file server had no access control at all

Found via the same comparative architecture review, not a user report or
a live incident. `_BufferRequestHandler` (the plugin's own minimal HTTP
server -- see the module docstring's "Since plugins can't register their
own URL routes" bullet for why it exists at all) only ever guarded
against path traversal; it had no concept of *who* was asking. Unlike
every other way into this data -- Dispatcharr's own stream endpoints
(gated by `network_access_allowed(request, "STREAMS")`), its API
(session/API-key auth), this plugin's own `run/` actions (admin-account
gated, confirmed via `apps/accounts/permissions.py`) -- this server had
none of that. The plugin's own docs ask users to expose `http_port`
through their container config "the same way 9191 already is", which in
practice often means the whole LAN and sometimes further (port-forwarded
for remote access). Anyone who could reach that port could read any
channel's currently-buffered live segments with zero Dispatcharr
credentials at all -- a real broken-access-control gap (OWASP A01), not
just a theoretical one, given how routinely this exact port gets opened
up per the plugin's own setup instructions.

Fixed by requiring a per-buffer access token (`secrets.token_urlsafe(24)`,
compared with `secrets.compare_digest` to avoid a timing side-channel) on
every request to `_BufferRequestHandler`. The token is generated once
when a buffer is first created (`_start_buffer`'s fresh-start branch) and
handed back in that same authenticated action's response -- the only way
to ever learn it is to already have Dispatcharr admin credentials, same
gate as everything else server-side. Checked in `_check_access_token`,
called from both `do_GET` and `do_HEAD` before touching the filesystem or
refreshing any heartbeat. A buffer already running under a pre-upgrade
plugin version (Redis state predates the `access_token` field) gets one
retrofitted the next time `start_buffer` reattaches to it, rather than
being permanently unreachable -- self-healing across an upgrade, the
same design principle as the crashed-viewer fix above.

Addon-side (`pvr.dispatcharrai`), the token comes back in
`StartTimeshiftBuffer()`'s own response (`CallTimeshiftPluginAction()`),
gets appended to the playlist URL there, cached on
`LiveTimeshiftStreamState::accessToken`, and reused for every later
segment range-read (`ReadLiveTimeshiftStream()`) -- no need for
`get_live_manifest`'s own response to carry it separately, since the
token doesn't change for the life of the buffer.
`secrets.token_urlsafe()`'s output is already URL-safe base64
(`[A-Za-z0-9_-]`, no padding), so it needs no escaping to sit directly in
a query string.

Confirmed live (Windows): the addon-side change compiles and the addon
loads and runs normally after reload (`Addons.GetAddonDetails` reports
`broken: false`). The full request-gets-rejected-without-a-token and
retrofitted-token-on-reattach paths need the updated plugin deployed to
a live Dispatcharr instance to exercise end-to-end -- not yet done as of
this writing.

### `get_live_manifest` rebuilt its whole response from scratch on every call

Found via the same comparative architecture review, not a live incident
-- this was a genuine inefficiency, not a correctness bug (the manifest
returned was always accurate). `_get_live_manifest()` read and parsed the
entire `live.m3u8` playlist and called `stat()` on every currently-
visible segment on every single call, with no memory of what a previous
call already found. At this plugin's own defaults (`buffer_minutes=60`,
`segment_seconds=2`), the visible window is 1800 segments -- so a call
that found nothing new still cost up to 1800 `stat()` syscalls plus a
full playlist re-parse, and this function is called far more often than
the buffer could possibly have grown: pvr.dispatcharrai's own catch-up-
to-tail loop and throttled length checks (`RefreshLiveManifest()`) call
it repeatedly while waiting, not just once per new segment. Notably, the
*client*-side counterpart of this exact function (the addon's own
`RefreshLiveManifest()`) already only merges segments newer than what it
has cached -- this function just wasn't applying the same principle to
its own internal work.

Fixed with a per-worker-process, in-memory cache (`_manifest_cache`,
keyed by `channel_uuid`), not a Redis-backed one -- deliberately, to
avoid adding a second, differently-shaped piece of Redis-persisted state
alongside the buffer's own lifecycle state for what's purely a
performance optimization, and to avoid the JSON-(de)serialization cost
of round-tripping up to 1800 entries through Redis on every call, which
could plausibly have eaten into the very savings being chased. Freshness
is checked cheaply via the playlist file's own `mtime`/`size` (one
`stat()` call, unavoidable and cheap) rather than a content hash:

- If unchanged since this same worker process's own last read: the
  entire previous response is reused outright -- no re-parse, no
  re-stat, not even a re-read of the playlist text.
- If changed: the playlist is re-parsed (cheap -- a small text file), but
  each segment is looked up in the cache **by its `sequence` number**
  before deciding whether to `stat()` it again. A sequence number is
  never reused for the life of a buffer (HLS media sequence is
  monotonic) even though `-segment_wrap` does recycle *filenames* -- so
  a cache hit by sequence is guaranteed to be the exact same bytes, the
  same invariant the addon's own client-side merge logic already relies
  on. Only genuinely new segments (normally just one, in steady state)
  pay for a `stat()` call. Byte/time offsets are still recomputed on
  every call regardless of cache hits -- cheap, in-memory-only
  arithmetic, not themselves cacheable, since they're deliberately
  window-relative (see this function's own docstring).

Being per-worker rather than cross-worker means the benefit depends on
how consistently Dispatcharr's WSGI layer routes one viewer's repeated
`get_live_manifest` calls to the same worker process -- not something
this pass confirmed either way. Worst case (a cold worker, or requests
bouncing across workers with no affinity) is identical to the old
behavior, never worse; it only helps, and how *much* it helps in
practice depends on that routing behavior. The buffer-lifecycle Redis
state itself is untouched by this change.

Verified via a functional test against the real, unmodified
`_get_live_manifest()` function (imported directly, not reimplemented)
against real files in a temp directory, with `Path.stat()` calls counted
via monkeypatching: confirmed a cold call stats every segment (3/3), an
unchanged-playlist call stats zero segments and returns a byte-identical
manifest, a call with exactly one new segment stats exactly that one
segment, and -- the trickiest case -- a call where the sliding window
drops a segment and a *new* segment recycles that dropped segment's
*filename* correctly treats it as genuinely new (stats it fresh, keyed
by its new sequence number) rather than incorrectly reusing the old
cached size for the reused filename. Not yet exercised against a live
Dispatcharr instance or measured for actual wall-clock savings.

### 1.0.5 regression: the new per-viewer heartbeat could stall playback permanently

Reported live (macOS, 1.0.5, first playback attempt after upgrading),
with real diagnostic work already done before this was even looked at:
a channel glitched on open with a burst of decode errors, then sat in
Kodi's "buffering" state for several minutes straight and never
recovered. The report itself ruled out the server side first --
fetching the three newest segments directly from the plugin's file
server while Kodi was stuck came back 200 with correct sizes and zero
MPEG-TS sync-byte misalignment across ~54,000 packets checked, and the
buffer was actively producing new segments throughout. `kodi.log` showed
`ReadLiveTimeshiftStream`/`SeekLiveTimeshiftStream` simply stopped
logging anything at all for the rest of the session -- only
`GetStreamTimes` kept polling.

Root cause: `SendTimeshiftHeartbeat()`, added this same release (see "A
crashed viewer's own reference-count entry never got cleaned up" above),
called the shared `Request()`/`EnsureAuthenticated()` path. On a cache
miss, `EnsureAuthenticated()` triggers a full synchronous token refresh
or re-login (each its own `Request()` call, each allowed up to
`m_config.timeoutSeconds` -- default 30s), and `Request()`'s own
401-retry logic can trigger *another* refresh-or-login round before
retrying the original call once more. Stacked worst case: on the order
of 150s of possible blocking from a single call, entirely plausible from
an ordinary transient network hiccup landing at the wrong moment (right
as the access token needed refreshing) -- nothing exotic or rare about
the trigger condition. Confirmed via reading `Request()`/
`EnsureAuthenticated()`/`RefreshAccessToken()`/`Login()` directly (not
independently reproduced live in this pass) that this chain is real:
`Request()` defaults to `withAuth=true, retryOnAuthFailure=1`, `m_authMutex`
is recursive (rules out same-thread self-deadlock, but does nothing about
the sheer duration of the chain), and `m_accessTokenExpiry` is set to
only 4 minutes past each login/refresh (`Login()`/`RefreshAccessToken()`'s
own comment: SimpleJWT's default token lifetime is short), so this isn't
a rare edge -- every real playback session hits at least one "cache
miss" heartbeat roughly every 4 minutes it stays open.

The reason this was invisible to this project's own reasoning when the
heartbeat was first added: the design comment at the time explicitly
called the extra round trip "bounded and infrequent enough... well
within the waits this same function already tolerates from the catch-up
-to-tail logic" -- true for an *ordinary* fast round trip, but that
assumption was never actually checked against `SendTimeshiftHeartbeat()`'s
own worst-case call chain, which was unbounded in practice. Piggybacking
the heartbeat directly onto `ReadLiveTimeshiftStream()` -- the exact
thread Kodi's own demuxer depends on for continuous reads -- turns any
unbounded call on that path into an unbounded stall of live playback
itself; a stall long enough apparently trips something in Kodi's own
player/demuxer layer that doesn't self-recover even once the slow call
eventually completes and fresh data resumes (consistent with the
`ReadLiveTimeshiftStream` logging simply stopping rather than resuming
with a delay).

Fixed by rewriting `SendTimeshiftHeartbeat()` to never call
`EnsureAuthenticated()`/`Login()`/`RefreshAccessToken()` at all: it reads
whatever access token is already cached (a mutex lock, no network call)
and skips the heartbeat outright if that's empty, using its own
dedicated `curl` handle with a short, fixed 2000ms timeout instead of
`Request()`'s shared `m_config.timeoutSeconds`/retry logic -- the same
"build a lightweight call directly instead of going through the shared
`Request()` helper" pattern this file already uses for
`WaitForTimeshiftPlaylistReady()`, for the same reason (different timeout
needs than the general-purpose helper provides). Relies on
`RefreshLiveManifest()` -- called far more often than the heartbeat's
10s interval, every read-loop iteration -- to keep the cached token
fresh via the normal path in practice; a heartbeat skipped because the
cached token happened to be stale is simply retried 10s later, never
worth blocking a live read to guarantee.

A background thread (so the read path never waits on the heartbeat call
at all, bounded or not) was considered and deliberately not used: every
`m_liveTimeshiftStream = LiveTimeshiftStreamState()` reset (three call
sites -- open, failed-open cleanup, close) would need to first join any
in-flight heartbeat thread from the outgoing state, or a still-joinable
`std::thread` destructor call embedded in that assignment calls
`std::terminate()` and crashes the whole process outright -- a real,
easy-to-get-subtly-wrong hazard for a project that has already hit real
threading bugs before (the JWT token pair/API key data races, see
`CHANGELOG.md`'s `[0.3.0]` entry). A bounded 2000ms worst case on the
existing synchronous path is a smaller, provably-safe change that
directly removes the actual root cause (the *unbounded* nested
refresh/login chain) without introducing a new lifetime hazard to get
right instead.

Not yet re-confirmed live against the exact original repro (would need
deliberately forcing a slow/failing token refresh mid-playback, not done
this pass) -- confidence here comes from the fix removing every call in
the chain that was capable of blocking longer than 2 seconds, verified
by re-reading the rewritten function against this same root-cause
analysis, plus a clean compile and a normal (non-stalled) reload in Kodi.

### 1.0.6 follow-up: a second, distinct freeze -- corrupt packets within seconds of open

Reported live (macOS, addon 1.0.6, plugin 1.0.2, confirmed both actually
running) immediately after the heartbeat fix above shipped: ESPN
(1080p) still froze, but with a different signature ruling out a
recurrence of the heartbeat bug -- stalled in ~10s with zero successful
`ReadLiveTimeshiftStream` catch-up cycles logged first (the heartbeat
incident had several successful cycles before it hung), and the first
sign of trouble was ffmpeg's own `[mpegts] Packet corrupt` at ~4.7s,
well before even one 10s heartbeat interval could have elapsed. The
report ruled out the data itself first: segments fetched directly from
the plugin's file server (via a real access token) had 100% correct TS
packet alignment across ~54,000 packets checked and correct
`Content-Length` on every fetch.

`Packet corrupt` at the mpegts-demuxer level is a byte-structure error
(a PES length mismatch or continuity-counter break), not a "haven't
found a keyframe yet" situation -- combined with the underlying files
being provably intact, this means the wrong bytes were requested, not
that corrupt bytes existed on disk. Two candidates were investigated
with source access, as the report itself suggested would settle it
faster than further black-box log analysis:

**Investigated and not the cause: the addon's cold-start read position.**
`OpenLiveTimeshiftStream()`'s trim step (kLiveEdgeMarginSegments) and
its cold-start retry loop (`RefreshLiveManifest()` retried up to 30
times, breaking on the first response with at least one segment) were
read in full. Starting mid-GOP on a freshly-opened `-c copy` buffer is
inherent to how any live TS proxy works and isn't new -- and a mid-GOP
start produces missing-reference-frame warnings at the codec level
followed by a clean resync at the next keyframe, not TS-packet-level
corruption. Ruled out as the mechanism, though not necessarily unrelated
to why decode looked as bad as it did once real corruption was already
present.

**Root cause: a wrong segment size, once locked in, permanently
misaligns every later segment's computed offset.** This addon's own
`RefreshLiveManifest()` merges each newly-seen segment's `byte_size`
into its own cumulative address space exactly once (its own comment:
"an already-known segment's size can't legitimately change"), and every
*later* segment's `byteOffset` is computed by adding onto that same
running total -- never recomputed from the plugin's own response
offsets, which are deliberately window-relative (see the top of this
file). If the plugin ever reports a `byte_size` smaller than a
segment's real size, the addon requests exactly that (smaller) range,
gets a clean, fully valid 206 response for it (no error anywhere -- it's
genuinely a truthful subset of real, correctly-encoded bytes), advances
past what it believes is the segment's end, and starts reading the
*next* segment's file from byte 0 -- silently skipping however many
real bytes were missed at the end of the previous one. That skip lands
wherever it lands relative to TS packet boundaries, which is almost
certainly mid-packet, producing exactly a `Packet corrupt` signature at
the seam -- and unlike a clean read error, it's undetectable by fetching
either file whole and checking it in isolation (exactly what the
report's own verification did, and exactly why it couldn't have caught
this).

Whether `get_live_manifest` can *actually* report a wrong size
under real conditions wasn't conclusively proven live (a deliberate
attempt to force the exact race wasn't made this pass), but a plausible
mechanism exists in the manifest cache added for the "rebuilt its whole
response from scratch" fix above: a segment's size is cached the first
time it's observed and trusted from then on. Two changes address this,
at both ends independently, rather than betting on pinning down the
exact trigger:

- **Plugin side:** `_get_live_manifest()` now always re-`stat()`s the
  newest (last-listed) segment on any call that reparses the playlist,
  even if it matches a cached entry -- never trusting a cache hit for
  the one entry that could conceivably have been observed before a
  write was fully settled. Every other cached entry remains trusted
  outright, since a *later* segment having since appeared after it is
  itself proof it's done. Verified via a functional test against the
  real function: a cache entry deliberately poisoned with a wrong size
  for a still-newest segment is correctly discarded and re-verified the
  next time that segment's entry is touched by a reparse.
- **Addon side:** `ReadLiveTimeshiftStream()` now captures the real
  segment size the plugin's file server reports on every read (the
  `Content-Range: bytes X-Y/TOTAL` header any Range GET already
  receives, previously read and discarded) and compares it against the
  size this session cached for that segment. Any disagreement -- from
  this exact mechanism or any other future cause -- is now a loud,
  immediate, diagnosable failure (`ADDON_LOG_ERROR` naming the segment,
  its sequence, and both sizes, then `fatal = true`) instead of silent,
  permanent misalignment for the rest of the session. This is the more
  load-bearing of the two changes: it doesn't depend on correctly
  guessing the plugin-side trigger, and turns any future recurrence
  (from this cause or a new one) into an actionable log line instead of
  another multi-hour investigation.

Confirmed live (Windows): both changes compile cleanly and the addon
reloads normally. The plugin-side fix is verified via a functional test
against the real, unmodified function. Neither change was verified
against the exact original failure (would need the real ESPN 1080p
stream and a way to force the underlying race, not available from this
session) -- if this recurs after both ship, the new addon-side
diagnostic should name the exact segment and size disagreement, which
would be the fastest path to a fully confirmed root cause.

**Update -- verified live against the real failure (macOS, ESPN 1080p,
a separate Claude Code instance on the user's own Mac, relayed back):**
the permanent-freeze regression is genuinely fixed. ESPN (1080p) played
continuously for 4+ minutes, including a real -30s rewind seek partway
through (`demuxer seek to: ..., success`, `speed:1`/`canseek:true`
holding throughout) -- well past the ~10s mark that reliably killed it
under 1.0.6. Confirmed safe to tag.

However: `Packet corrupt` did **not** go away -- it just stopped causing
a permanent stall. Over the same session: 134 occurrences, recurring at
a strikingly regular ~2.5s interval (close to the plugin's 2s
`segment_seconds`) continuously throughout, not clustered near open;
118 `hardware accelerator failed to decode picture` lines alongside it;
and -- the important part -- **zero** occurrences of the new
`ReadLiveTimeshiftStream: segment ... real size ... disagrees with the
manifest-reported size` diagnostic, despite `Packet corrupt` firing 134
times in the same window.

That diagnostic exists specifically to catch the mechanism this fix
targeted (a cached size disagreeing with the file server's own
`Content-Range` total). It never firing, while the symptom kept
recurring, is real evidence -- not just an absence of proof -- that
*these* `Packet corrupt` occurrences are **not** caused by that
mechanism: segment sizes agree, every single time, on every read. Two
distinct things share the same ffmpeg log signature:

1. **The permanent-freeze mechanism this fix targets**: a wrong size,
   once locked in, permanently shifts every later segment's computed
   offset -- corruption that never resolves on its own, matching the
   original report (freeze within ~10s, never recovers). Fixed, and
   the diagnostic above would catch it if it ever recurs.
2. **A separate, apparently pre-existing, self-limiting artifact**:
   `Packet corrupt` recurring roughly once per segment boundary,
   throughout an entire session, that Kodi's own demuxer evidently
   resyncs from cleanly each time without visible playback impact.
   Given the interval lines up with `segment_seconds` almost exactly,
   this smells like something about the transition between two
   independently-produced segment *files* being concatenated into one
   continuous raw byte stream and handed to a demuxer that has no
   HLS-level awareness a segment boundary occurred at all (a genuine
   architectural property of this whole feature -- see this file's own
   "The actual fix: this addon demuxes the buffer itself" section --
   not something introduced by any change in this session) -- a PCR/
   continuity-counter discontinuity at the exact splice point is the
   leading guess, not confirmed.

**Update -- refined hypothesis, still not confirmed, deliberately not
pursued further.** `_start_ffmpeg()` invokes `ffmpeg -c copy -f segment
...` ([plugin.py](../dispatcharr-plugin/timeshift_buffer/plugin.py)).
Every time the `segment` muxer starts a new output file, it opens a
**fresh `AVFormatContext`** -- a new muxer session, not a continuation
of the previous one. `-c copy` skips re-*encoding* the codec payload,
but the TS *muxing* layer (packetization, PAT/PMT insertion, and each
PID's 4-bit continuity counter) is regenerated fresh per file -- normal,
correct behavior for HLS-style segmenting, where each segment is meant
to be independently playable, not byte-concatenated with its neighbors.
This addon's whole design treats the rolling buffer as *one continuous
raw byte stream* fed to a single demuxer instance instead (a deliberate,
effective workaround for `ffmpegdirect`'s broken HLS seeking -- see "The
actual fix" section above), so a byte-level discontinuity at each seam
becomes the demuxer's problem to survive, not something this
architecture actively smooths over.

Real corroborating precedent already exists in this same file: `-reset
-timestamps 1` was deliberately removed specifically because a
*different* piece of per-segment muxer state resetting broke something
(PTS continuity for seeking -- see the "Fix attempted and confirmed NOT
to work" section higher up, and the comment directly above `_start_
ffmpeg`'s own `cmd` construction). Continuity counters are a sibling
piece of the exact same underlying phenomenon -- per-segment muxer
reinitialization -- just never addressed, since nothing forced the
question until this investigation went looking for it.

Deliberately left unconfirmed and unfixed for now, by explicit
decision rather than time running out: a real fix would mean either
finding an ffmpeg flag to suppress the reset (no such flag is known to
exist -- carrying continuity-counter state across independent muxer
sessions isn't a normal use case ffmpeg is expected to expose a knob
for) or binary-patching each segment's continuity counters at the
splice point in this addon's own hot read path (technically
straightforward -- a well-defined 4-bit field at a fixed offset in
every 188-byte packet -- but real complexity and real risk added to
live playback, for a symptom currently confirmed cosmetic: Kodi's own
demuxer resyncs cleanly every time, no visible playback impact
confirmed over a 4+ minute real session). Revisit if it ever stops
being cosmetic, or if a lower-risk way to confirm the hypothesis
directly (e.g. capturing raw bytes on both sides of a real splice)
becomes worth the effort.

**Deliberately left open, not chased further this pass** -- flagged
here rather than closed out, per the live-testing session's own
recommendation: it's the *same symptom* that started this whole
investigation, just not currently fatal, and "harmless so far" isn't
the same as "understood." Tracked in `docs/OPEN_ITEMS.md`'s Ongoing
section.

### The "cosmetic" Packet corrupt noise stopped being cosmetic once -- first observed real failure

Reported live (macOS, addon build from the `chore/add-formatting-config`
branch -- a pure reformat plus a `0.x` version renumber on top of
current `master`, no logic changes on this path either in that branch
or predating it): live TV playback (a news channel, one mid-session
channel switch) hit two `Stream stalled, start buffering` events, the
second with a real, severe audio desync (`ActiveAE - large audio sync
error` climbing to and holding at -8278ms across dozens of consecutive
log lines) alongside `[h264] co located POCs unavailable` and `mmco:
unref short failure` next to the usual `hardware accelerator failed to
decode picture`. Zero occurrences of the `ReadLiveTimeshiftStream:
segment ... disagrees with the manifest-reported size` diagnostic
anywhere in the session, ruling out both previously-fixed mechanisms
above (the original size-mismatch bug and the pid-recycling cache gap)
as the cause here.

The `[mpegts] Packet corrupt` noise itself fired at its normal,
already-documented rate (172 occurrences across the session, roughly
every 2-4s -- consistent with the per-segment-muxer-reset hypothesis
this section already describes), so this isn't a new or different
mechanism triggering -- it's the *same* long-documented noise, just
this time not staying cosmetic. Unlike the original size-mismatch bug,
this didn't hang forever: Kodi's own player eventually gave up and
tore the stream down on its own (`Player.GetActivePlayers` came back
empty afterward, Kodi itself stayed up throughout, no crash).

This is the first observed occurrence, not yet a reliable repro -- one
data point, on one channel, after one channel switch. Deliberately not
chased further immediately (a formatting/version-renumber PR was the
actual focus of that testing round); tracked here and in
`docs/OPEN_ITEMS.md`'s Ongoing section as a real, needs-fixing signal
now rather than a purely theoretical "revisit if it stops being
cosmetic" trigger. Next step, whenever this gets picked back up: try to
force a live repro deliberately (let a channel run long enough to hit
the per-segment continuity-counter reset repeatedly) rather than
waiting on another incidental occurrence.

**Update -- a deliberate rapid-channel-switching stress test did not
reproduce it (Windows, addon 0.9.0).** 15 minutes, 105 switches across
six channels (MLB Network, ESPN (1080p), NHL Network, NFL Network, CNN,
NBA TV), random dwell 2-15s per channel. Result: 483 `Packet corrupt`
occurrences (the same already-documented benign rate), zero
size-disagreement diagnostic firings, zero addon errors/crashes/failed
opens. 42 `ActiveAE - large audio sync error` warnings and 91 `timeout
waiting for buffer` warnings (this platform's DXVA render path logs a
different signal than the Amlogic build's `ttd`/`Level` lines) -- every
one of the 133 checked against switch timing, and 130 landed within
-7s to +5s of a channel switch, consistent with the ordinary
audio/decode pipeline reset every switch causes, not a standalone
problem. The single `Stream stalled` event was a clean cold-buffer
catch-up blip on a freshly-switched channel, resolved in under 200ms
with only a 109ms audio correction -- nothing resembling the original
report's sustained -8278ms desync recurred.

Rapid switching specifically does not appear to be what triggers the
escalated failure -- the original report involved continuous playback
with only one switch, not rapid cycling. Narrows the next repro attempt
toward long continuous dwell on a single channel rather than switching
frequency.

**Update -- a 30-minute continuous single-channel dwell (the other half
of the narrowed-down approach above) also did not reproduce it
(Windows, addon 0.9.0).** ESPN (1080p), no switching at all after the
initial open. Result: 782 `Packet corrupt` occurrences (same benign
rate as always, expected volume for 30 minutes), zero size-disagreement
diagnostic firings, zero audio desync warnings, zero stream stalls,
zero addon errors or crashes, and zero `catch-up-to-tail` "gave up"
exhaustions across the entire 30 minutes -- the buffer never once fell
far enough behind to need one. The only event in the whole window was a
single `timeout waiting for buffer` warning 4 seconds after the initial
channel open (the normal open-transition blip, same as every other
channel-open in this investigation), followed by nothing for the
remaining ~29.9 minutes.

Two different stress angles now tried -- 15 minutes of rapid switching
(105 switches) and 30 minutes of continuous single-channel dwell -- and
neither reproduced the escalation. It still stands as a single,
unreplicated occurrence. Next attempt, if pursued further, would need a
different angle: longer duration, a different channel, or looking at
this from the Dispatcharr/plugin server side rather than the Kodi
client side.

### A consistent ~89.4s audio-sync-error reading appears once (or a few times) per fresh stream open -- harmless, distinct from the Packet corrupt investigation above

Found during routine log review, not a targeted investigation (Windows
addon 0.9.0 and Rocky Linux addon 0.9.0, two unrelated machines/
platforms, general viewing rather than a scripted test). Both logged
`ActiveAE - large audio sync error` readings clustered tightly around
**-89,400 to -89,500ms** -- Windows: one isolated occurrence mid-
playback (no channel switch nearby); Rocky Linux: three occurrences in
pairs, all within the first ~4 minutes after Kodi's own launch, none
after. Neither caused any visible playback impact -- no stall, no
elevated `Packet corrupt` rate afterward, no recurrence once past the
initial window.

That specific a magnitude repeating almost exactly across two
independent machines is too consistent to be coincidental noise, but
also doesn't match this file's other findings: no `CPtsTracker`/
`CDropControl` pattern-loss messages immediately preceded the Rocky
Linux occurrences (unlike the one Windows case, which did show a
PTS-pattern recalibration right before it -- possibly two different
paths converging on the same downstream symptom, not necessarily one
mechanism). Leading, unconfirmed guess: a one-time measurement
artifact from how the addon's server-side timeshift buffer establishes
its initial live-edge/clock reference when a stream first opens,
plausibly related to the buffer's own configured visible-window
duration coincidentally landing in this range -- not verified against
the actual `timeshift_buffer` plugin settings on the Dispatcharr
instance these sessions used. Purely informational for now: no
playback impact observed on either machine, not chased further this
pass. Worth a quick look if it's ever cheap to check what
`visible_segments * segment_seconds` actually evaluates to for the
buffers in use, to see if it lines up with ~89-90s.

### 1.0.7 follow-up #2: the diagnostic caught a real, different mismatch -- a cross-buffer-instance cache gap

Reproduced live (macOS, addon 1.0.7, plugin 1.0.3, redeployed and
reloaded): ESPN (1080p) played fine, switched to MLB Network (played
fine), switched back to ESPN -- froze within ~8s of the fresh buffer
being created (`already_running=0`, confirming this is a brand-new
buffer instance, not leftover state). The Content-Range cross-check
added for 1.0.7 fired exactly as designed:

```
ReadLiveTimeshiftStream: segment seg_00001.ts (sequence 1) real size (2883732, from Content-Range)
disagrees with the manifest-reported size this session cached (3139788) -- every later segment's
computed offset may already be misaligned; giving up on this stream rather than risk silent corruption
```

This is the first real, confirmed instance of the theorized mechanism
firing live, not just a plausible theory -- the diagnostic did exactly
its job (caught the disagreement, logged the segment/sequence/both
sizes, refused to keep reading). But net effect for the user was
unchanged from before 1.0.7: a clean detection that still results in
"frozen, never recovers," just now for a well-understood, logged
reason instead of a silent one.

**The size direction matters.** Cached (3,139,788) was *larger* than
real (2,883,732) -- the opposite of what a "sampled before the write
fully settled" race would produce (that would read *smaller* than
final, never larger). That ruled out the original 1.0.5-manifest-cache
race theory for this specific instance and pointed at something else:
the cached value wasn't stale-and-growing, it was **describing a
completely different file**.

**Root cause: `_manifest_cache` invalidation was per-process, not
per-buffer-instance.** `_delete_buffer_state()` (called by
`stop_buffer`/the reaper/dead-buffer cleanup) clears
`_manifest_cache[channel_uuid]`, but only in *that calling worker
process's own memory* -- Dispatcharr's multi-worker deployment means
the `stop_buffer` call tearing down the old ESPN buffer instance isn't
guaranteed to land on the same worker process that had cached
`get_live_manifest` data for it. A channel switched away and back gets
a brand-new ffmpeg process whose `live.m3u8` restarts sequence
numbering from scratch -- so its early segments have the *exact same*
`(sequence, filename)` pairs as the previous instance's, despite being
completely different files with different real content. Whichever
worker never got the (process-local) memo that the old instance was
torn down would happily "recognize" `sequence=1, filename=seg_00001.ts`
as a cache hit and serve the *old* instance's size for the *new*
instance's file -- exactly matching a wrong-in-either-direction size
(this time larger; the original 1.0.5-race theory would only ever
produce smaller), and exactly matching why it hit `sequence=1` this
early (the second segment of a fresh buffer is squarely within the
narrow window before that segment stops being "newest" -- see the
1.0.7 fix above -- so the *newest*-segment protection doesn't cover
it once a worker's stale entry pre-dates the new instance entirely: the
cache lookup itself was returning wrong data for a "known" segment,
which the newest-segment check never had a reason to distrust).

**Fixed:** every cache entry is now tagged with the buffer's own
`state["pid"]` (already tracked for dead-buffer detection) at write
time. On every lookup, a pid mismatch discards the *entire* cached
entry outright -- treated exactly like a cold cache, no partial trust
of any part of it -- rather than trying to reason about which parts
might still be safe. This closes the gap regardless of which worker
built the stale entry or whether `_delete_buffer_state()`'s own
cache-clear ever reached it: correctness no longer depends on
cross-worker message delivery at all, only on data (`pid`) already
present in the authoritative Redis-backed buffer state every worker
reads. The newest-segment-always-re-stat protection from the first
1.0.7 fix is kept alongside this, unrelated failure mode, still worth
having.

Verified via a functional test against the real, unmodified function,
directly reproducing the reported scenario: a cache entry built under
one `pid` or a channel's segment (matching sequence *and* filename)
served in a request carrying a *different* `pid`, with different real
file content at each name -- confirmed the stale entry is fully
discarded and both segments are freshly stat()'d, returning the new
instance's real sizes (500 and 2,883,732 bytes in the test, the latter
matching the exact value from the live incident).

Not yet re-verified against the exact live failure (would need another
macOS pass, channel-switch-away-and-back on ESPN specifically) -- if
this recurs, the same Content-Range diagnostic from the first 1.0.7 fix
will still catch it and log the disagreement, so any remaining gap
would at least be immediately visible rather than silent.

**Distinct from the still-open "recurring, non-fatal `Packet corrupt`
every ~2.5s" item above** -- that one never triggered this diagnostic
across 134 occurrences in a single, continuously-running buffer
instance (no channel switch involved), so it isn't explained by this
fix and remains open, untouched, tracked separately in
`docs/OPEN_ITEMS.md`.

**Update -- the pid-based version above wasn't good enough; verified
live it still failed under heavy testing churn.** Retested (macOS,
plugin 1.0.4) both with the exact repro sequence (no failure that time)
and, after a methodology mistake was caught and the test redone on a
genuinely fresh Kodi log, a cold open of ESPN with *no* channel-
switching involved at all -- the diagnostic fired anyway, on
`seg_00000.ts`, the very first segment of a freshly-created buffer. A
retry on the same fresh session fired again, on a different segment,
with different size values -- twice in a row, no channel switch either
time.

The size direction was the tell again: cached larger than real in both
new firings, the same pattern as the original incident and, like that
one, incompatible with a "sampled before the write settled" race
(which can only ever produce cached *smaller*). Still a cross-instance
identity confusion -- but the pid-based check should have caught it.
The report itself flagged the likely cause: "many hours of repeated
start_buffer/stop_buffer API calls... across many earlier test
rounds" that day. **OS pids get recycled.** Under that much churn (each
`start_buffer` spawning a new ffmpeg process, each `stop_buffer` or
idle-timeout killing one), the pid counter can and reproducibly did
wrap far enough that a stale `_manifest_cache` entry from hours
earlier -- tagged with a pid the OS has since reassigned to a
genuinely new, unrelated ffmpeg instance -- passed the pid-equality
check it should have failed. Not a scenario an ordinary user's normal
viewing pattern would produce (that much buffer churn in one session is
inherently a heavy-testing artifact), but the fix needed to not depend
on that being true.

Fixed by keying identity on `access_token` instead of `pid` -- already
a fresh, cryptographically random value (`secrets.token_urlsafe(24)`)
minted for every genuine new buffer instance (see `_start_buffer`'s own
comment), with no OS-level recycling risk at any timescale, however
much churn a session produces. Also fails closed: a missing/empty
token (state predating the access-token feature, or anything else
going wrong establishing identity) is never treated as matching another
missing token, unlike the pid version's implicit "unknown == unknown"
gap.

Verified via a functional test extending the same suite: the exact
pid-reuse scenario (same pid, different `access_token`, different real
file content) is now correctly treated as a new instance and returns
the real, current sizes -- including the literal 3,097,112-byte value
from the live report. A separate test confirms a missing token never
wrongly matches a prior missing-token entry. Not yet re-verified live
(would need another macOS pass, ideally including the same kind of
heavy same-day buffer churn that exposed the pid gap, to be confident
this specific failure mode is actually closed and not just harder to
hit).

### A malformed `#EXTINF:` duration could fail the whole manifest fetch

Found via a follow-up audit prompted by an analogous bug just found and
fixed in the companion `recording_edl` plugin (see that plugin's own
`docs/RECORDING_EDL.md` entry) -- not a live incident, and not something
this session's earlier reviews of this exact function happened to catch.
Every `int()`/`float()` conversion in this file was re-checked
specifically for the same failure shape: a value that parses
successfully as a float but then raises on the later `round()`/`int()`
conversion, outside (or not fully covered by) whatever `except` clause
was guarding the parse itself.

`_get_live_manifest()`'s own `#EXTINF:` duration parsing
(`int(round(float(...) * 1000))`) had exactly this gap: the whole
expression sits inside a single `try`/`except ValueError`, which
correctly catches `float("nan")`'s later failure (`round(nan)` raises
`ValueError`) but not `float("inf")`'s (`round(inf)` raises
`OverflowError`, a different exception class `except ValueError:`
doesn't match). An `#EXTINF:inf,` line would raise uncaught out of the
parsing loop, with no enclosing `try`/`except` in `_get_live_manifest`
itself either -- caught only by `_get_live_manifest_action`'s own
broad `except Exception as exc:` further up the call stack, which
degrades to a clean `{"status": "error", ...}` response rather than an
unhandled 500. Better than the `recording_edl` version of this bug
(which had no such backstop at all), but the *practical* effect is
still worse than it should be: one malformed duration value fails the
entire manifest fetch -- every segment, not just the one with the bad
duration -- for a function called continuously during active playback
(the addon's own catch-up-to-tail loop and throttled length checks).
Repeated failures here don't hang (the addon's own bounded
catch-up-attempts budget still applies), but they'd produce confusing,
hard-to-diagnose retries rather than a clean per-segment fallback.

ffmpeg is the only realistic writer of `live.m3u8` and isn't expected
to ever emit `inf` as a segment duration -- this is a defensive gap,
not a reproduced live failure, same as `recording_edl`'s. Fixed by
widening the `except` clause to `(ValueError, OverflowError)`; the
existing `duration_ms = 0` fallback already handles the degraded case
correctly, so no further restructuring was needed here (unlike
`recording_edl`, where the conversion had to be pulled inside the
guarded block in the first place). Every other numeric conversion in
this file was checked against the same failure shape and found safe:
the Range-header parsing (`_parse_range`) only ever calls `int()` on a
string directly, never `float()` followed by a separate `round()`/
`int()`, so there's no equivalent NaN/Inf-survives-the-parse gap to
begin with; the admin-configured settings reads (`idle_timeout_seconds`,
`segment_seconds`, etc.) are each either individually protected by a
call site's own broad exception handler (`_start_buffer`'s
`except Exception` around all of `_start_ffmpeg()`) or the reaper
loop's own per-tick `except Exception`, and are type-validated plugin
settings rather than generated file content in the first place.

Verified with a test confirming `round(float("inf"))` genuinely raises
`OverflowError` uncaught in the pre-fix code, then confirming the fixed
parser returns the segment with `duration_ms: 0` instead of failing the
whole manifest.

