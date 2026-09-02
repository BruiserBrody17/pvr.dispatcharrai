# Troubleshooting & known limitations

*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

## Channel switching fails after the first channel (root cause: IPv6)

**If channel N+1 never plays after channel N worked, and this repeats for
every subsequent switch (not just once), check whether your Dispatcharr
host resolves to both an IPv4 and an IPv6 address, and whether the IPv6
route is actually reachable.** This was root-caused (not guessed) against a
real deployment and is almost certainly the first thing to check before
suspecting the addon, Dispatcharr, or the upstream IPTV provider.

What was actually observed, via Kodi's own `debug.setextraloglevel=64`
(`LOGCURL`) trace: opening the *first* channel, curl tried the host's IPv6
address, that failed/timed out, and after several retries across ~15s it
fell back to IPv4 and succeeded. Opening the *second* channel to the same
host, curl tried IPv6 **only** -- it never fell back to IPv4 at all, and sat
timed out for the full 30s Kodi gives it. This repeated identically for every
subsequent switch, including switching back to a channel that had played
fine moments earlier -- i.e. this has nothing to do with which channel, or
even that a switch is happening at all; it's specifically about Kodi's own
HTTP connection handling to that *host* misbehaving after the first request.

In the deployment this was diagnosed against, the Dispatcharr host's IP was
a Gluetun (VPN client) container's LAN IP -- Gluetun commonly blocks IPv6
outright as a leak-prevention measure, so the AAAA record pointed at an
address nothing could ever actually reach, even though DNS kept advertising
it. **The fix was entirely DNS-side: remove/disable the AAAA record for the
Dispatcharr hostname (or otherwise ensure it only resolves to an address
that's genuinely reachable over IPv6, or doesn't advertise IPv6 at all).**
Confirmed: switching straight to the plain IPv4 address in the addon's Host
setting fixed it immediately, before the DNS change was made.

This is *not* something this addon's code can reliably route around --
Kodi's `CCurlFile`/libcurl URL options (see `xbmc/filesystem/CurlFile.cpp`)
don't expose a way to force IPv4-only resolution, and having the addon
resolve the hostname to a hardcoded IPv4 address itself for the stream URL
would break certificate validation for anyone using HTTPS. If you hit this
and can't fix the DNS/network layer, using the bare IPv4 address in the
addon's Host setting instead of a hostname is the reliable workaround.

Two mitigations were tried and ruled out before finding this, kept here so
they aren't re-attempted:
- **A fixed delay before switching** (a `channel_switch_delay_seconds`
  setting was added, default 2s) -- didn't help even at 10+ seconds, which
  is what proved this wasn't a timing race with Dispatcharr's proxy
  releasing the previous connection. The setting itself has since been
  removed from the addon (it never fixed anything, and left in as a
  no-op escape hatch nobody ended up needing) -- if you're reading this
  looking for it in current settings.xml, it's gone.
- **`|Connection=close` on the stream URL** (a real Kodi/`CurlFile` URL
  option, confirmed applied in the log) -- didn't help either, which is what
  proved this wasn't ordinary HTTP keep-alive/connection-pool reuse.

Also ruled out: Dispatcharr/Unraid worker capacity (rapid channel zapping
through Dispatcharr's own web UI against the same instance worked
perfectly), and a dead/rate-limited channel (the exact failing URL streamed
real data fine when requested independently, and switching *back* to the
first, previously-working channel failed identically).


## Known Kodi-core quirks (confirmed not this addon's bug)

- **A completed recording's duration/"runtime" can be permanently wrong in
  Kodi's recordings list and info panel -- stuck at some small value from
  early in the recording, even though this addon reports the correct
  final duration on every single call.** Confirmed this addon is not the
  cause: added a temporary diagnostic log directly in
  `PVRDispatcharr::GetRecordings()` and confirmed, in the same Kodi
  session that displayed the wrong value, that this addon computed and
  handed Kodi the exact correct duration (matching Dispatcharr's live API
  exactly) on every call -- Kodi's own JSON-RPC `runtime` property still
  reported the old, wrong number regardless.
  Root cause, confirmed by reading Kodi's own source
  (`xbmc/video/VideoInfoTag.cpp`): `CVideoInfoTag::GetDuration()` --
  inherited by `CPVRRecording`, since a PVR recording *is* a video
  library item -- doesn't just return the duration this addon sets via
  `SetDuration()`. It prefers a separately-tracked, *stream-probed*
  duration (`m_streamDetails.GetVideoDuration()`, populated by Kodi's own
  player/library code when it actually opens and reads the file's real
  media streams) unless that probed value is under 60% of the tag's own
  duration. If Kodi probed this recording's file early -- while it was
  still being written, so only a couple of minutes existed on disk -- and
  persisted that small probed duration, it can keep winning over this
  addon's correct value indefinitely.
  This explains why a full Kodi restart doesn't fix it, unlike a plain
  "phantom recording" (see below): recordings themselves are **not**
  persisted locally (confirmed: Kodi's own PVR database, `TV46.db`, has
  no `recordings` table at all -- the in-memory list is rebuilt fresh
  from this addon every session), but the *stream-probed* duration lives
  in Kodi's separate, persistent video library database, keyed by the
  recording's synthetic file path, which survives every restart.
  **Confirmed (not just plausible) to be the exact same root cause as a
  related symptom**: Kodi's Home screen "Recent recordings" widget shows
  full media flags (resolution/codec/audio format, e.g. `H.264` `1080 HD`
  `DOLBY` `5.1`) for the affected recording but only bare runtime for an
  unaffected one -- reproduced side by side on the same install, same
  widget, hovering each item in turn. Both symptoms come from the same
  `m_streamDetails` field: populated, the media flags show *and* the
  small probed duration can win; empty, neither happens.
  Narrowed down further which playback path actually populates it: the
  one affected recording here was specifically played through this
  addon's **in-progress playback feature** while it was still being
  written (at the time, gated behind the now-removed
  `enable_inprogress_playback` setting and routed through the separate
  `inputstream.ffmpegdirect` addon -- see `docs/RECORDINGS.md` for the
  native-demuxer mechanism, unconditional and no longer routed through
  ffmpegdirect at all, that later replaced it); the unaffected one was
  only ever played through this addon's native completed-recording path
  (`OpenRecordedStream()`/`CInputStreamPVRRecording`). That's a real,
  mechanistic difference, not a coincidence, for the mechanism as it
  existed at the time: it means **using the in-progress playback feature
  to check in on a recording early has a lasting side effect on how Kodi
  displays it later, once it's completed** -- a permanently wrong
  duration in the library view, even though the recording itself and its
  actual playback are both completely fine. Not fixable from this addon's
  side (see above), but worth knowing before using that feature on a
  recording you also plan to watch normally once it's done.
  **Re-verified against the current native-demuxer in-progress mechanism
  and confirmed no longer reproducing.** Recorded a fresh 3-minute
  recording, opened it via `Player.Open` 8 seconds after it started
  (`canseek: false`, `time`/`totaltime` both `0` at that point -- as early
  as this can realistically be caught), played roughly 15-20 real seconds
  of it, then stopped. `kodi.log` confirmed
  `CSaveFileState::DoWork - Saving file state for video item pvr://...`
  fired on that stop -- the same file-state/stream-details-writing
  machinery the original finding was rooted in, so this genuinely
  exercised the mechanism rather than missing it. Stopped the underlying
  Dispatcharr recording early (`POST .../stop/`, real recorded length
  ~54s) and restarted Kodi fresh to force a clean re-fetch.
  `PVR.GetRecordingDetails`' `runtime` showed `175` (this addon's own
  `end_time - start_time` value, matching the already-documented -- and
  separate -- "stopping early doesn't rewrite `end_time`" quirk noted
  elsewhere in this file, not a stuck small value), and opening the
  completed file for actual playback showed a live `totaltime` of `1:01`,
  closely matching its real recorded length -- both numbers internally
  consistent with a correctly-probed file, not a leftover small value
  from the early session (which would have looked like something under
  ~20-30s, not either of these). Repeated once more (fresh recording,
  same method) with the same result. Reran with the exact same
  reproduction steps that caught the original bug, and it's gone --
  whether that's because the current mechanism routes through
  `CInputStreamPVRRecording`'s own native demuxer (the same class the
  always-unaffected completed-recording path uses) rather than a separate
  inputstream addon's differently-behaved reporting, as originally
  guessed above, wasn't traced further in Kodi-core source to confirm as
  the actual mechanism -- but the observed behavior itself is confirmed
  live, twice, not just plausible.
  Not something this addon can fix or work around: there's no PVR client
  API to tell Kodi "forget the stream details/library metadata you
  cached for this file," and this addon doesn't (and shouldn't) touch
  Kodi's video library database directly. Actual playback is unaffected
  either way -- confirmed separately that the real file plays with its
  correct, full-length duration once actually opened; this is purely a
  library/metadata display quirk for an item Kodi has cached stream
  details for.

## Known limitations with more than one Kodi client

Not bugs in this addon -- inherent to running multiple, fully independent
Kodi installations (e.g. one on Windows, one on macOS) against the same
Dispatcharr server, worth writing down since it's easy to mistake for one:

- **A recording created on one Kodi instance doesn't appear on another
  until that other instance happens to refresh.** This addon has no
  push/notification channel from Dispatcharr (it's a plain REST poller),
  and `TriggerTimerUpdate()`/`TriggerRecordingUpdate()` only tell *that
  specific running addon instance's* Kodi to re-fetch -- they have no way
  to reach a separate Kodi installation's separate addon instance.
  Originally confirmed with no fix at all: a recording created via one
  Kodi's guide didn't appear on a second, independently-running Kodi until
  that second instance was restarted. Substantially mitigated now (see the
  periodic recordings/timers refresh below) -- every install triggers on
  its own schedule regardless of the others' activity, so the wait is
  bounded by `recording_refresh_minutes` instead of "until next restart."
  Not eliminated: still a poll, not a push, so there's still up to that
  many minutes of lag, and two installs mid-refresh-cycle can briefly
  disagree.
- **Playback resume position doesn't carry over between Kodi
  instances.** Kodi's "resume from where you left off" bookmark is
  stored in that Kodi installation's own local video database, not
  anywhere this addon controls or Dispatcharr is aware of -- watching
  partway through a recording on one device has no way to inform a
  different device's Kodi where to resume. Kodi's PVR API does have
  purpose-built hooks for exactly this
  (`GetRecordingLastPlayedPosition`/`SetRecordingLastPlayedPosition`,
  meant for backends that track resume position server-side instead of
  relying on Kodi's local database), which this addon doesn't currently
  implement. It's a real, legitimate feature to add if cross-device
  resume matters, but not attempted here yet -- it would need a place to
  actually store the position server-side, and `custom_properties` is the
  only candidate on the `Recording` object, whose write semantics
  (whether a `PATCH` merges or replaces the field) need to be verified
  against a **disposable test recording** before ever touching a real
  one, given `custom_properties` was already confirmed to be fully
  replaced rather than merged on `POST` create (see above).
- **One install's addon can silently invalidate another install's stored
  API key, breaking recording playback with no obvious cause.** Dispatcharr
  keeps exactly one active API key per account (see the permissions note
  above); if two Kodi installs share an account and each generates its own
  key once, whichever install last regenerated invalidates the other's
  stored copy. Confirmed end-to-end (Windows + macOS installs against the
  same account, same recording): the macOS side got `CCurlFile ... Failed:
  HTTP returned code 401` on a key that was valid when it was generated,
  while a fresh `generate/` call from either side proved the account only
  keeps one key alive at a time. Fixed by making `OpenRecordingStream()`/
  `ReadRecordingStream()` treat a 401 as "this key was invalidated
  elsewhere," not a hard failure: they call `GenerateApiKey()` and retry
  once before giving up, and `PVRDispatcharr` re-persists the new key to
  the addon's settings if it changed. This doesn't stop the two installs
  from continuing to invalidate each other's cached key -- it just makes
  that invisible, since whichever side needs the key next self-heals
  within one HTTP round-trip instead of failing outright. Verified by
  deliberately corrupting a live install's stored key and confirming
  playback still succeeded, with the corrected key written back to
  `settings.xml` automatically.

## Still unconfirmed (verify before relying on in production)

- Whether a one-time recording that doesn't match any real EPG programme
  (a fully manual/custom time range with nothing airing to auto-enrich
  from) ever gets a title *from Dispatcharr itself*. Less consequential
  than it used to be: the pending-title cache above now shows whatever
  title Kodi's manual-timer dialog was given regardless, so this only
  matters for whether Dispatcharr's own data independently agrees once
  its enrichment (or lack thereof) resolves -- not tested.
- The account used to verify this (`claude`) initially got "You do not
  have permission to perform this action" trying to create a recording or
  series rule -- same account that could log in, browse channels, and
  stream fine. Raising that account's role in Dispatcharr's admin UI
  resolved it. If you hit the same error, that's a Dispatcharr-side
  permissions setting, not an addon bug -- check the account's role before
  assuming otherwise.

