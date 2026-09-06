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

