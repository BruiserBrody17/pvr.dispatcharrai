*(the project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs; check items off and add new ones here as they come up. The 1.0-era release-checklist history this file used to keep below -- version bumps, README/plugin-README rewrites, the four-platform smoke-test pass, release packaging -- was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history if ever needed. "Ongoing" below is the live section: add anything new there, regardless of which release it surfaces in.)*

# Open items

## Ongoing (more will likely come up)

- **Three real recordings/timers hot-path inefficiencies, found via a
  full-codebase Efficiency review (2026-09-10, not yet applied -- would
  need live-hardware verification this pass didn't have).**
  `RefreshInProgressRecordingManifest()` re-fetches the *entire*
  recordings list (a full `GET /api/channels/recordings/` plus JSON
  parse of every recording) on every throttled call (up to ~2/sec
  during in-progress-recording playback/scrubbing), just to read one
  recording's `isInProgress` flag -- a targeted single-recording lookup
  (Dispatcharr's REST API likely supports `GET
  /api/channels/recordings/{id}/`, a standard DRF `retrieve` action
  matching the already-used `{id}/stop/`/`{id}/extend/` siblings, but
  this is unconfirmed against real source/a live instance) would replace
  an O(all recordings) cost with O(1) on this hot path.
  `GetRecordingsAmount()`/`GetRecordings()` and
  `GetTimersAmount()`/`GetTimers()` are each called back-to-back by Kodi
  on every recordings/timers refresh with no caching between the pair
  (unlike channels/EPG, which have `EnsureChannelsLoaded()`/
  `EnsureEpgLoaded()` staleness caches) -- doubles/triples the real
  REST-fetch cost of what's logically one refresh. Not applied this
  pass: adding a new, unconfirmed API endpoint or a caching layer with
  its own staleness/thread-safety behavior are both real behavior
  changes needing live verification, not pure refactors -- see
  `chore/simplify-pass`/PR #12 for the full review this came out of
  (including a `FindRecordingById()` helper that *did* get applied,
  consolidating the repeated fetch-and-scan code without changing its
  cost).
  **Update: the single-recording-lookup piece implemented and confirmed
  live (2026-09-10).** Confirmed `GET /api/channels/recordings/{id}/`
  directly against the live instance first (real, HTTP 200, identical
  shape to a list item), closing the "unconfirmed" gap. New
  `DispatcharrClient::GetRecordingById()` (a REST call -- not to be
  confused with the pre-existing `PVRDispatcharr::FindRecordingById()`,
  which scans an already-fetched in-memory list) replaces the
  `GetRecordings()` call in `RefreshInProgressRecordingManifest()`;
  `GetRecordings()`'s own per-item parsing was pulled out into
  `ParseRecordingJson()` so both call sites stay identical rather than
  duplicating the field-mapping logic. Live-verified against an actual
  in-progress recording during real playback: `kodi.log`'s own timing
  breakdown showed `GetRecordingById 0.006-0.009s` on every refresh
  (down from a full 41-recording list fetch+parse), `finished=0`
  correctly reflected throughout.
  **Update: the `GetRecordingsAmount()`/`GetTimersAmount()` double-fetch
  piece also implemented and confirmed live (2026-09-10) -- all three
  original findings now closed.** New `EnsureRecordingsLoaded()`/
  `EnsureTimerRulesLoaded()`, mirroring `EnsureChannelsLoaded()`/
  `EnsureEpgLoaded()`'s own staleness-cache shape but with a
  `kRecordingsAndTimersCacheTtlSeconds` (2s) TTL instead of
  `channel_refresh_hours`/`epg_refresh_hours` -- short enough that a
  user's own change is never meaningfully delayed even before the
  invalidation below, long enough to collapse the Amount()+List() pair
  Kodi calls back-to-back into one real fetch.
  `TriggerRecordingUpdate()`/`TriggerTimerUpdate()` (Kodi SDK base-class
  methods, not something this addon defines, so their own implementation
  can't be edited directly) are now only ever called through two new
  wrappers, `InvalidateAndTriggerRecordingUpdate()`/
  `InvalidateAndTriggerTimerUpdate()`, which reset the relevant cache
  timestamp to force a fresh fetch on the very next call before
  delegating to the real trigger -- applied mechanically across all ~10
  existing call sites. `PVRDispatcharr::FindRecordingById()` was also
  simplified to call the new `GetRecordingById()` directly instead of
  its own separate full-list fetch+scan, now that that REST call exists,
  a small additional win beyond the original three findings.
  Live-verified against the real instance: 8 rapid-fire `PVR.GetTimers`/
  `PVR.GetRecordings` calls (matching Kodi's own Amount()+List() pattern)
  produced only 2 real cache refreshes in `kodi.log`, correctly
  straddling the 2s TTL, instead of 8 independent fetches under the old
  code. Separately confirmed invalidation: added a real one-time timer
  via `PVR.AddTimer`, and the very next `PVR.GetTimers` call already
  reflected the updated recordings count (a momentary miss right at the
  exact instant of creation traced to a pre-existing sub-second race in
  the `isInProgress`/`isUpcoming` time-window check, unrelated to this
  cache -- resolved within ~3s and confirmed as the correct, pre-existing
  behavior, not a regression from this change).
- **Rename the project from `pvr.dispatcharrai` to `pvr.dispatcharr`
  (requested 2026-09-09).** Mechanically straightforward in-repo: `git
  grep -il dispatcharrai` found 25 files at request time (27 by the time
  this was actually done, since more docs/tooling had landed by then),
  plus the two directories whose names carried the id
  (`pvr.dispatcharrai/`/`packaging/coreelec/pvr.dispatcharrai/`). Needed
  updating: `addon.xml.in`'s `<addon id="...">`, `CMakeLists.txt`'s
  `project()`/`build_addon()`, `.github/workflows/build.yml` (addon-defs
  paths, `ADDONS_TO_BUILD`, artifact/zip names), the CoreELEC
  `package.mk` (`PKG_NAME`/`PKG_SITE`/`PKG_URL`/`PKG_SHORTDESC`), docs
  (`docs/BUILDING.md` heaviest), and ~50 hardcoded `"pvr.dispatcharrai: "`
  log-prefix strings across `src/*.cpp` (cosmetic, not functionally
  required). The two companion Python plugins
  (`dispatcharr-plugin/recording_edl`, `dispatcharr-plugin/timeshift_buffer`)
  keep their own unrelated ids but reference `pvr.dispatcharrai` by name
  in READMEs/`plugin.json` `help_url`s/many `plugin.py` comments -- those
  needed updating too. Renaming the GitHub repo itself
  (`BruiserBrody17/pvr.dispatcharrai`) was a separate decision, made
  after the in-repo rename landed -- see the "Update" below for the
  actual rename and the URL cleanup that followed it. Every GitHub URL
  (repo `<source>`/`PKG_SITE`/`PKG_URL`/`help_url`s/clone commands/README
  release links) was deliberately left pointing at the *old* repo name
  in the initial in-repo-rename commit, specifically because this
  decision hadn't been made yet at that point -- only the addon's own
  id, directory names, and in-repo local-checkout-directory conventions
  changed in that first pass.
  **The real cost isn't the repo, it's that Kodi treats an id change as
  a brand-new addon, not an upgrade.** The addon id is both the
  installed folder name and the `userdata/addon_data/<id>/settings.xml`
  storage key, so every device currently running this addon (Windows,
  Rocky Linux laptop, ODROID N2+/CoreELEC, macOS) needs the old addon
  removed and the new-id build installed fresh -- existing settings
  (host/port/credentials, API key, timezone selection, padding,
  timeshift mode) do **not** carry over automatically; either hand-copy
  each device's `addon_data` folder to the new id or reconfigure from
  scratch, still undecided. Also expect to need the same "Settings ->
  PVR & Live TV -> Guide -> Clear data" step already documented above
  (Kodi's EPG database keys off the client id) on every device after the
  switch. Proposed order: rename on a branch -> decide
  GitHub-repo-rename yes/no -> rebuild + fresh-install Windows first and
  verify clean -> roll the same fresh-install to Rocky Linux,
  ODROID/CoreELEC, and macOS (via the peer session) -> settle the
  settings-carryover question per device -> cut a release under the new
  name once all four platforms are confirmed working.
  **Update: mechanical in-repo rename done and confirmed compiling
  (2026-09-10), on branch `rename/pvr-dispatcharr` -- not yet merged,
  not yet installed anywhere.** All 27 files updated; every GitHub URL
  deliberately left pointing at the real, current repo name (see above).
  `PKG_SHA256` in the renamed `package.mk` reset to the all-zeros
  placeholder, per this file's own versioning convention -- the existing
  real checksum was computed against the old (un-renamed) `0.9.3` tag's
  actual tarball content, so it no longer matches anything this renamed
  source would produce. Verified live: reconfigured the local Windows
  build workspace with a new `addon-defs/pvr.dispatcharr/` entry
  (pointing the same `file://` URL at this repo's root) and did a full,
  from-scratch `cmake`+MSBuild build under the new target name -- built
  and installed cleanly to `install/pvr.dispatcharr/pvr.dispatcharr.dll`
  with no errors. `clang-format`/`ruff` both pass (the log-prefix string
  length change shifted a handful of multi-line `kodi::Log()` calls'
  wrapping, caught by `clang-format --dry-run -Werror` and fixed).
  `tools/check_doc_refs.py` also updated (its own hardcoded
  `resources/settings.xml` path) and still passes clean against the
  baseline.
  **Update: GitHub repo renamed too (2026-09-10) -- `BruiserBrody17/
  pvr.dispatcharrai` is now `BruiserBrody17/pvr.dispatcharr`, decided and
  executed the same session.** `gh repo rename`, then this Windows
  workspace's own `origin` updated via `git remote set-url` (confirmed
  still tracking correctly afterward -- GitHub's redirect made the
  transition seamless, no re-clone needed). Every GitHub URL this repo's
  own files deliberately left pointing at the old name in the initial
  in-repo-rename commit above (repo `<source>`/`PKG_SITE`/`PKG_URL`/
  `help_url`s/clone commands/README release links, 11 files) was then
  updated to the new name too, now that the reason to hold off no longer
  applies -- rather than leaning on GitHub's redirect indefinitely.
  `docs/BUILDING.md`'s pinned-checksum example URL
  (`.../archive/0.3.0.tar.gz`) updated the same way; the tag itself
  (`0.3.0`) didn't change, just which repo name it's addressed through.
  Still pending: the Rocky Linux laptop, ODROID/CoreELEC, and macOS
  peer-session workspaces each have their own `origin` still pointing at
  the old repo name -- each needs its own `git remote set-url` (or a
  fresh clone) whenever that device is next touched; not urgent since
  the old URL keeps working via GitHub's redirect, just cleanup.
  **Update: fresh-installed and verified live on this Windows machine
  (2026-09-10) -- Windows now confirmed clean, the first of four
  platforms.** Rather than reconfiguring from scratch, carried over the
  existing `addon_data/pvr.dispatcharrai/settings.xml` verbatim to a new
  `addon_data/pvr.dispatcharr/` -- both are just files, no id baked into
  the content itself, so this preserves host/port/credentials/API key/
  timezone/padding/timeshift settings exactly. Installed the renamed
  build alongside the old one (different ids, so no conflict), disabled
  `pvr.dispatcharrai` and enabled `pvr.dispatcharr` via
  `Addons.SetAddonEnabled`, confirmed via `PVR.GetClients` that exactly
  one client (`pvr.dispatcharr`) was active afterward, not both. A burst
  of `PVR::CPVREpg::Update: ... Client '-1' not found` errors appeared
  once, right at the enable/disable transition -- Kodi's EPG database
  cleaning up the old client's now-orphaned tables, a one-time artifact
  of the switch, not a recurring problem (confirmed: no further
  occurrences afterward). Everything else came up clean with the carried-
  over settings: realtime updates connected, background channel/EPG
  refresh succeeded, recordings/timer-rules caches populated with the
  real live counts (43 recordings, 11 series rules -- same numbers as
  under the old addon, confirming the same account), and `PVR.GetChannels`
  returned the full real 9,080-channel lineup. Live playback smoke-tested
  end to end, not just data loading: opened a real channel (Channel H,
  same generic-label convention as Channels A-G elsewhere in this file's
  history -- see the earlier "Packet corrupt"/EPG-matching entries),
  confirmed via `kodi.log` the stream URL was genuinely routed through
  the new addon (`pvr.dispatcharr_68829.pvr`), audio decoder opened
  successfully, only the same already-documented benign startup noise
  (`non-existing SPS/PPS referenced`, a transient audio-sync
  adjustment) -- no new errors. The old `pvr.dispatcharrai` install was
  left in place, disabled rather than deleted, as a rollback path.
  Still not done: the other three devices (Rocky Linux laptop,
  ODROID/CoreELEC, macOS via the peer session), or the eventual release
  cut.
  **Update: fresh-installed and verified live on macOS (2026-09-10) --
  second of four platforms.** No persistent build workspace survived
  from earlier sessions, so this was a from-scratch setup (fresh Kodi
  source checkout, a separate local addon copy synced via `rsync -az
  --delete`, `addon-defs` pointing at it) -- built cleanly on the first
  attempt. Same settings-carryover approach as Windows: copied
  `addon_data/pvr.dispatcharrai/settings.xml` verbatim to a new
  `addon_data/pvr.dispatcharr/settings.xml` (byte-identical, confirmed
  via `diff`). Installed the renamed build alongside the old one,
  recorded real baseline counts under the old addon first (9,080
  channels, 7 recordings, 50 timers via `PVR.GetChannels`/
  `GetRecordings`/`GetTimers`), then disabled `pvr.dispatcharrai` and
  enabled `pvr.dispatcharr` via `Addons.SetAddonEnabled`. `PVR.GetClients`
  confirmed exactly one active client afterward (`pvr.dispatcharr`,
  clientid 2 -- the old one had been clientid 1). Unlike Windows, no
  `Client '-1' not found` burst appeared at all here (not a discrepancy --
  the task description flagged it as "likely", not guaranteed); no
  errors in `kodi.log` either way. Re-ran the same three counts under
  the new addon: identical (9,080/7/50), confirming the same account/
  data, not something broken. EPG also confirmed loading real programme
  data for a real channel (`PVR.GetBroadcasts` on Channel H returned 112
  real broadcasts with real, distinct titles -- not placeholder/empty
  data). Live playback smoke-tested end to end on Channel H: `kodi.log`
  confirmed the stream
  genuinely routed through the new addon
  (`pvr.dispatcharr_68829.pvr`, `CallTimeshiftPluginAction(start_buffer)`
  logged under the `pvr.dispatcharr` prefix), and a real screenshot
  confirmed live video actually playing (not just a JSON-RPC state
  claim -- `Player.GetActivePlayers`/`GetProperties` returned transient
  empty/`None` responses immediately after `Player.Open` that turned out
  to be socket-timing noise in the test harness, not a real problem;
  retrying a few seconds later, and the screenshot, confirmed playback
  was actually healthy the whole time: `canseek: true, speed: 1`). Only
  the same already-documented benign startup noise appeared
  (`non-existing SPS/PPS referenced`, a `-245ms` self-correcting
  `ActiveAE::SyncStream` adjustment) -- no new errors. One channel-id
  gotcha worth noting for future sessions: Kodi's PVR `channelid`s are
  scoped to the active client instance, not stable across a client
  switch -- a channelid cached from before the switch (752, used in
  earlier macOS sessions) returned "Invalid params" against the new
  client and had to be re-looked-up by channel label instead. Old
  `pvr.dispatcharrai` left in place, disabled, as a rollback path.
  Still not done: the other two devices (Rocky Linux laptop,
  ODROID/CoreELEC), or the eventual release cut.
- [x] **Follow-up API survey: three more genuinely implementable findings,
  beyond the recording-management ones below (found 2026-09-08, all
  four resolved by 2026-09-09).** Diffed all ~196 of Dispatcharr's real API paths
  (its live `/api/schema/`) against every endpoint this addon actually
  calls (grepped from `src/DispatcharrClient.cpp`), then checked the
  promising gaps against Dispatcharr's real source rather than guessing:
  (1) `GET /api/core/version/` -- public, no auth, `{version,
  timestamp}` -- closes `GetBackendVersion()`'s own documented "no
  confirmed server-version endpoint" gap directly; (2) `GET
  /api/core/timezones/` -- the full ~400+ real IANA timezone list vs.
  this addon's own curated ~25, could broaden `recurring_rule_timezone`
  coverage; (3) Dispatcharr's `SystemNotification` system (`GET
  /api/core/notifications/` etc.) is real and operationally relevant
  (version-update/setting-recommendation/warning/info, with a real
  priority field), not just dev chatter -- a plausible, secondary
  feature to surface high-priority ones as Kodi GUI notifications, no
  design work done beyond confirming it's real. Also checked and ruled
  low-value: `POST .../catchup/sessions/{id}/position/` is purely
  cosmetic for Dispatcharr's own admin dashboard, doesn't affect this
  addon's playback. Also noted: Dispatcharr has a whole separate VOD
  API (`/api/vod/*` -- movies/series/episodes/categories) this addon
  doesn't touch, but that's architecturally out of scope for a
  `kodi.pvrclient`-type addon, not a gap to close here. Full detail in
  `docs/API_NOTES.md`'s table and its new "System notifications" section.
  **Update: a fourth finding, from a broader "what Dispatcharr product
  features exist" pass rather than just an endpoint diff.** Dispatcharr
  supports named, curated channel subsets (`ChannelProfile`/
  `ChannelProfileMembership`, with user accounts assignable to one) --
  a real feature (e.g. a "Kids" or "Sports only" lineup) this addon has
  no way to let a user pick; `GetChannels()` always pulls every channel,
  unfiltered. Confirmed against the real `ChannelViewSet.get_queryset()`
  that this is opt-in filtering (`?channel_profile_id=`), not a
  server-enforced access boundary -- so not implementing it isn't a
  security gap, just a missed curation feature. `GET
  /api/channels/profiles/` lists what's available (empty on this
  particular single-user instance, but the mechanism is real). See
  `docs/API_NOTES.md`'s new "Channel profiles" section.
  **Update: items (1) and (2) implemented and confirmed live
  (2026-09-09).** `GetBackendVersion()` now reports Dispatcharr's real
  version (confirmed live: `0.30.0`, visible in Kodi's own System Info ->
  PVR service panel), fetched once at startup via new
  `DispatcharrClient::GetServerVersion()`. `recurring_rule_timezone`'s
  known-zone table broadened from 25 to ~50 entries using
  `GET /api/core/timezones/` as the reference for what's real vs.
  hand-guessed -- the practical scope turned out narrower than "genuinely
  comprehensive": the actual bottleneck is DST *rule* coverage (only two
  hand-verified rule families exist), not the zone name list, so this
  stayed within those families plus confirmed no-DST zones rather than
  claiming all ~440. New `DispatcharrClient::GetSupportedTimezones()`
  also feeds a refined startup diagnostic (distinguishes "real zone, no
  DST rule for it" from "not a recognized zone at all"). A genuine,
  unrelated regression was found and fixed along the way: this project's
  own earlier privacy scrub had collaterally replaced the real, working
  `America/Chicago` entry in `kKnownTimeZones`/`settings.xml`/
  `strings.po` with the literal string "REDACTED_TZ", silently breaking
  DST auto-detection for Central time -- confirmed via a real restart
  that `recurring_rule_timezone` had been stuck on `manual` because of
  it, and that the fix restores auto-detection. Full account, including
  why the regression happened and why restoring a generic dropdown entry
  isn't a privacy re-exposure, in `docs/RECURRING_RULES.md`'s "Update"
  note.
  **Update: 0.9.2 (batching this item plus the three recording-management
  features below) confirmed on CoreELEC/ODROID N2+ (2026-09-09).** Real
  cross-compile via the CoreELEC package.mk path (`docs/BUILDING.md`),
  deployed over SSH. `PVR.BackendVersion` correctly read back `0.30.0` via
  `XBMC.GetInfoLabels` (no GUI screenshot needed for this one -- a real
  Kodi skin infolabel, more direct than the Windows check). Timezone fix
  also confirmed independently on this device: `recurring_rule_timezone`
  auto-resolved to `America/Chicago` on first load of the new build (this
  device's own 0.9.1 install predated the REDACTED_TZ regression
  entirely, so this wasn't a regression-recovery test here, just
  first-time-correct confirmation). Basic live-playback smoke check
  (Channel A) also came back clean, no errors in `kodi.log`. Recording
  rename/file size/extend-recording themselves were **not** re-tested on
  this device -- all three are plain REST calls plus generic Kodi PVR
  API plumbing with no platform-specific code path, the same reasoning
  already applied when deciding not to cross-platform-test rename after
  it was first confirmed on Windows.
  **Update: also confirmed on the Rocky Linux laptop (2026-09-09) --
  it had been sitting at `0.9.0`, several releases behind.** Same checks,
  same clean result: `PVR.BackendVersion` read back `0.30.0`,
  `recurring_rule_timezone` auto-resolved to `America/Chicago`, live
  playback of Channel A came back clean with no errors. Hit two build/
  deploy issues specific to this platform, both now written up in
  `docs/BUILDING.md`: the live-checkout build harness silently no-ops
  against updated source unless two separate stale-marker locations are
  cleared first (not just one, and not just after a genuine failure --
  already-documented advice that turned out incomplete), and launching
  the Kodi Flatpak non-interactively over SSH needs the real logged-in
  session's display environment exported first or Kodi's own process
  crashes on startup (a pre-existing documented gotcha, just re-hit
  here).
  **Update: also confirmed on macOS (2026-09-09).** No persistent build
  workspace survived from earlier sessions, so this was a from-scratch
  setup: a fresh Kodi source checkout plus a separate local copy of the
  addon (not the real repo directly), with `addon-defs` pointing at that
  copy and synced from the real repo via `rsync -az --delete` first, same
  pattern as Windows/Rocky Linux. Built cleanly on the first attempt --
  a brand-new `ExternalProject` tree has no stale stamps to clear, so the
  live-checkout stale-marker gotcha above didn't actually come up this
  time. Same checks, same clean result: addon reports `0.9.2` via
  `Addons.GetAddonDetails`, `PVR.BackendVersion` read back `0.30.0` via
  `XBMC.GetInfoLabels`, `recurring_rule_timezone` auto-resolved to
  `America/Chicago` (this device's install was previously at `0.9.1`,
  predating the REDACTED_TZ regression, so again first-time-correct
  rather than regression-recovery). Live-playback smoke test needed a
  second attempt: the first channel tried was a placeholder EVENT-type
  channel with no real stream behind it and failed with the addon's own
  generic "ffmpeg exited before producing any segments" error -- not a
  regression, just a dead test channel; switching to Channel A gave a
  clean fresh-buffer open with no errors in `kodi.log`. All four
  platforms (Windows, CoreELEC/ODROID, Rocky Linux, macOS) are now
  confirmed current on `0.9.2`.
  **Update: (3) and (4) considered and deliberately not pursued
  (2026-09-09).** System notifications: Kodi GUI notifications are
  toast-style interruptions over whatever's currently playing --
  version-available/setting-recommendation chatter is exactly the kind
  of backend-admin noise that has no business popping up mid-playback in
  a living-room context, and this was only ever "plausible, secondary,
  no design work done" to begin with, never a real commitment. Channel
  profiles: if the filtering already happens server-side (Dispatcharr's
  own admin UI), a Kodi-side profile picker is a second UI for the same
  thing -- the one scenario where it would've earned its keep is
  multiple Kodi devices sharing one Dispatcharr account wanting
  *different* subsets (a device-level need account-level filtering can't
  express), but that's theoretical here: this instance's channel
  profiles are empty, not an actual gap for the current setup. Neither
  ruled out permanently -- revisit if either premise changes (e.g. a
  second Kodi device actually wants a different lineup than the main
  one).
- [x] **Three recording-management features TVHeadend has that this addon
  doesn't, all confirmed implementable against Dispatcharr's real API
  (found 2026-09-08, all three implemented by 2026-09-09).** Recording rename/
  description edit (`POST /api/channels/recordings/{id}/update-metadata/`,
  writes into the exact `custom_properties.program.*` fields already
  read on the way in), recording file size (not a JSON field, but a
  `HEAD` request against `/api/channels/recordings/{id}/file/` gets it
  from `Content-Length`), and extending an in-progress recording (`POST
  .../extend/`, a real dedicated endpoint this addon currently doesn't
  call at all). Rename is the most user-visible and cleanest to add.
  Two other candidates (recording undelete, per-recording retention)
  were checked and ruled out -- Dispatcharr's `DELETE` is immediately
  destructive with no trash table, and there's no retention/lifetime
  concept anywhere in its API. A fourth (resume position/play count)
  has no server-side backing either; only implementable as a weaker,
  addon-local-only hack, not pursued. Full write-up, including how each
  was confirmed against Dispatcharr's actual source (not just its
  OpenAPI schema, which mis-describes this endpoint's request body) in
  `docs/RECORDINGS.md`'s "Recording-management feature gaps vs.
  TVHeadend" section.
  **Update: rename implemented and confirmed live (2026-09-09).**
  `SetSupportsRecordingsRename(true)` plus a `RenameRecording()`
  callback calling `DispatcharrClient::RenameRecording()` (new). Tested
  end-to-end via Kodi's own GUI (its rename dialog, not JSON-RPC --
  Kodi has no JSON-RPC method for this at all): renamed a real
  in-progress-turned-stopped recording from "Get Up" to "RENAMETEST",
  confirmed both through Kodi's own `PVR.GetRecordings` and directly
  against Dispatcharr's REST API -- `custom_properties.program.title`
  updated to the new value, `user_edited: true` set, description left
  untouched (confirming the addon correctly sends only `{"title": ...}`
  for a pure rename). File size and extending an in-progress recording
  remain unimplemented.
  **Update: file size implemented and confirmed live (2026-09-09).**
  Turned out simpler than originally scoped above -- no `HEAD` request
  needed at all. `custom_properties.bytes_written` is already present in
  the same `GetRecordings()` payload this addon already fetches;
  confirmed against Dispatcharr's real source
  (`apps/channels/tasks.py`) that it's a sum of the recording's HLS
  segment file sizes, written once at finalization, not updated live
  during an active recording. `Recording::bytesWritten` parses it
  (defaulting to 0 when the key is absent), `SetSupportsRecordingSize(true)`
  plus `PVRRecording::SetSizeInBytes()` surface it. Tested live end to
  end on Windows across the full lifecycle of a real instant recording:
  in-progress (`custom_properties` genuinely lacks the key, confirmed via
  temporary debug logging -- 0 shown), just-stopped-not-yet-finalized
  (key still absent), and finalized (key present, real value). Kodi's own
  GUI showed the result two ways -- the recordings list's per-folder
  "Total: 25.22 MB", and a dedicated "Size: 25.22 MB" line in the
  recording's own info panel -- both matching the raw byte count
  (26,448,968) exactly. Also confirmed the boundary case: a recording
  that never captured real stream data (`status=interrupted`, a test
  channel with no live backing) correctly showed `bytesWritten=0` and
  Kodi's info panel omitted the Size line entirely rather than showing a
  misleading zero.
  **Update: extending an in-progress recording implemented and
  confirmed live (2026-09-09) -- all three original items now done.**
  Routed through `GetTimers()`'s existing `PVR_TIMER_STATE_RECORDING`
  timer for an in-progress recording: editing its end time in Kodi's
  Timers window now calls new `DispatcharrClient::ExtendRecording()`
  (`POST .../extend/`, `{"extra_minutes": N}`) instead of the generic
  reschedule PATCH a not-yet-started timer still uses. That distinction
  mattered: checked against the real `extend` endpoint's own source
  first and found it deliberately bypasses Django's `pre_save` signal
  because that signal revokes the running Celery recording task -- a
  plain PATCH against an already-recording item would have stopped it,
  not extended it. Tested live: extended a real in-progress recording's
  end time by 15 minutes via Kodi's actual Timer-edit dialog, confirmed
  via `PVR.GetTimers` that the end time moved forward *and* `state`
  stayed `"recording"` throughout -- direct proof the task kept running
  rather than being revoked. Full account, including the numeric-pad
  GUI quirks hit along the way, in `docs/RECORDINGS.md`.
- [x] **In-progress recording playback had the same seek-bar
  wall-clock-position bug live timeshift had -- fixed and live-confirmed
  (2026-09-08).** `GetStreamTimes()`'s in-progress-recording branch
  hardcoded `startTime=0`, the exact root cause PR #2 fixed for live
  timeshift. Fixed more simply than the live case: a recording's real
  start time is already known (`PVRRecording::GetRecordingTime()`,
  passed through `OpenRecordedStream()` into
  `OpenInProgressRecordingStream()`), no cold-start-trim anchor needed.
  Confirmed live: seeks in both directions landed at the exact expected
  position during an actual in-progress recording's playback. See
  `docs/TIMESHIFT.md`'s "PVR.TimeshiftProgress*"/seek bar section's
  second "Update" for the full account.
  **Update: also confirmed on CoreELEC/ODROID N2+ (2026-09-09).** Same
  seek-both-directions test, same clean result. Windows and CoreELEC
  now confirmed; macOS/Rocky Linux still untested for this specific
  fix.
- [x] **A consistent ~89.4s audio-sync-error reading on fresh stream opens,
  harmless -- mechanism found, closed (2026-09-08/09).** Seen independently on Windows and Rocky
  Linux (addon 0.9.0 on both), clustered right around -89,400 to
  -89,500ms, only near the start of a stream and never recurring. No
  playback impact on either machine. Purely informational -- not
  chased further. The leading guess (the buffer's own configured
  visible-window duration, `visible_segments * segment_seconds`,
  coincidentally landing in this range) was checked live against
  `timeshift_buffer`'s real source (2026-09-09) and ruled out: that
  formula always reduces to just `buffer_minutes * 60`, three orders
  of magnitude too large for any realistic buffer setting to land near
  89-90s.
  **Update: mechanism found, via Kodi-core's own source (2026-09-09).**
  Reproduced a fourth time (Windows). `ActiveAE.cpp`'s `large audio
  sync error` warning logs the *raw, uncapped* clock-vs-PTS difference
  before it gets clamped to a sane bound for actual use -- right at
  stream open, before the audio clock locks onto a stable reference,
  that raw value can transiently read something enormous, then never
  recurs for the rest of that session. Addon-independent: this is
  Kodi-core's own audio clock bootstrap, not anything this addon does.
  See `docs/TIMESHIFT.md`'s section of the same name for the detail.
- ~~In-progress recording playback never received the catch-up-budget
  hardening that live timeshift already has~~ -- **Already fixed;
  this entry was stale.** When this was written (2026-09-08, after
  finding a substantial *uncommitted* refactor sitting on the Rocky
  Linux laptop's checkout and discarding it per instruction, since its
  authorship/testing history was unknown), the assumption was that the
  in-progress-recording path still had the old, unhardened
  single-segment/1.5x-margin catch-up math. It didn't -- that exact
  refactor (shared `EstimateSegmentDurationMs()`/
  `ComputeCatchUpAttempts()` helpers, 5-segment average, 1500ms floor,
  3x margin) had already been committed to `master` three days
  earlier, in `1ddf3d9` (2026-09-05), and live-verified there (a
  forced tail-seek showed the seek-probe path getting its fast
  1-attempt budget and a genuine catch-up wait getting the full 3x
  budget and succeeding). The Rocky Linux checkout that prompted this
  entry was evidently just behind `master` at the time. That commit
  never touched this file or `docs/TIMESHIFT.md`, which is why the
  gap looked open later -- corrected here, and the fix itself is now
  written up in `docs/TIMESHIFT.md`'s "Catch-up budget hardening
  shared between live and recording playback" section.
- **Recurring, non-fatal `Packet corrupt` on server-side live timeshift,
  post-1.0 (surfaced during 1.0.7 verification, 2026-09-07).** Confirmed
  live on macOS (Channel B 1080p): ffmpeg's mpegts demuxer logs `Packet
  corrupt` roughly once every ~2.5s throughout an otherwise-healthy
  4+ minute playback session (close to the plugin's 2s
  `segment_seconds`), with Kodi's own demuxer evidently resyncing
  cleanly each time -- no stall, no visible playback impact, real seeks
  worked fine. Ruled out as the *same* mechanism as 1.0.7's
  permanent-freeze fix (that fix's own diagnostic, which directly
  detects a segment-size disagreement, never fired once across 134
  occurrences of this). **Refined hypothesis (2026-09-07, still not
  confirmed, deliberately not pursued further):** ffmpeg's `-f segment`
  muxer opens a fresh `AVFormatContext` per segment file -- `-c copy`
  skips re-encoding, but the TS *muxing* layer (including each PID's
  continuity counter) is regenerated fresh per file, normal for
  independently-playable HLS segments but not for this addon's own
  design of concatenating them into one raw byte stream. Real
  corroborating precedent already in this codebase: `-reset_timestamps
  1` was deliberately removed for the exact same class of problem
  (per-segment muxer state resetting), just for PTS continuity instead
  of continuity counters. Deliberately left unfixed: no known ffmpeg
  flag suppresses the reset, and the alternative (binary-patching
  continuity counters at each splice in the live read path) is real
  complexity/risk for a symptom still confirmed cosmetic. See
  `docs/TIMESHIFT.md`'s "1.0.6 follow-up" section's second "Update" note
  for the full reasoning. Not blocking any release -- flagged so it
  doesn't get lost, not because it's currently causing visible harm.
  (Not the same bug as the next item below -- that one *did* trigger
  the size-disagreement diagnostic; this one still hasn't, across 134+
  occurrences.)
  **Update (2026-09-07): stopped being cosmetic once.** First observed
  real failure on macOS (a news channel, one mid-session channel
  switch): two `Stream stalled, start buffering` events, the second with
  a severe, sustained audio desync (`ActiveAE - large audio sync error`
  holding at -8278ms) and `[h264]` decode errors, ending in Kodi's own
  player tearing the stream down on its own (no permanent hang, no
  crash). Same underlying noise, same ~2-4s rate, still not the
  size-disagreement mechanism (zero diagnostic firings this session
  either) -- just the first time it's caused visible harm instead of a
  silent resync. One occurrence, not yet a reliable repro. This is now
  a real, needs-fixing signal rather than a purely theoretical
  "revisit if it stops being cosmetic" trigger. See `docs/TIMESHIFT.md`'s
  "The 'cosmetic' Packet corrupt noise stopped being cosmetic once"
  section. Next step: try to force a live repro deliberately rather
  than waiting on another incidental occurrence.
  **Update (2026-09-08): a 15-minute, 105-switch rapid-channel-cycling
  stress test (Windows, addon 0.9.0) did not reproduce it.** Same
  benign `Packet corrupt` rate as always, zero size-disagreement
  firings, zero errors/crashes, and every audio-sync/buffer-timeout
  warning landed within seconds of a channel switch (normal
  pipeline-reset noise), not a standalone problem. Rapid switching
  specifically doesn't appear to be the trigger -- the original report
  was continuous playback with only one switch. Next attempt should
  try long continuous dwell on a single channel instead. See
  `docs/TIMESHIFT.md`'s same section for the full test breakdown.
  **Update (2026-09-08): a 30-minute continuous Channel B dwell (the
  follow-up angle above) also did not reproduce it.** Same benign
  `Packet corrupt` rate, zero size-disagreement firings, zero audio
  desync, zero stalls, zero `catch-up-to-tail` "gave up" exhaustions
  across the full 30 minutes. Two different stress angles tried now
  (rapid switching and long dwell), neither reproduced it -- still a
  single, unreplicated occurrence. See `docs/TIMESHIFT.md`'s same
  section for the full breakdown.
  **Update (2026-09-08): a second, real occurrence -- this time
  self-recovered.** macOS, addon 0.9.0, ~20-hour continuous session
  (Channel A): a burst of three `Stream stalled` events in ~3.6s,
  audio desync peaking at -5064ms, but this time Kodi's own resync
  machinery pulled it back under threshold within about a second with
  no manual intervention and no user-visible interruption -- unlike the
  first occurrence, which required manually stopping the player. Same
  noise, same rate, zero size-disagreement firings across the full
  session. Two data points now with different outcomes (fatal vs.
  self-recovering), suggesting a second branch point beyond just
  cosmetic-vs-escalates. Still not enough signal to act on. See
  `docs/TIMESHIFT.md`'s same section for the full detail.
  **Update (2026-09-08): a reliable, on-demand trigger found --
  seeking to the live edge, reproduced 3-for-3 in one session
  (macOS).** Severity escalated across the three reproductions,
  peaking at 248,392ms (over 4 minutes) of desync and 439
  hardware-decoder failures in the third one, still not fully
  recovered when the user gave up waiting and restarted. Confirmed
  not a regression from PR #2 (its diff doesn't touch the seek/read
  path). This changes the situation from "wait for it to happen
  incidentally" to "can be deliberately tested" -- worth deciding
  whether to actually chase this now rather than keep just recording
  occurrences. See `docs/TIMESHIFT.md`'s same section for the full
  writeup and the reasoning for why live-edge seeks are a plausible
  trigger (landing on an arbitrary point with no guaranteed clean
  H.264 keyframe/SPS-PPS boundary, unlike a seek into already-settled
  earlier segment data).
  **Update: the segment-wraparound hypothesis is ruled out -- this is
  channel/stream-specific.** Real segment-file-reuse threshold for the
  affected instance is ~10 hours (`buffer_minutes=300`); both real
  incidents happened at 1-5 minutes of buffer age, nowhere close.
  Confirmed on Windows too: 6 live-edge seeks on a 2-minute buffer,
  zero corruption. Controlled trials then found the real variable:
  Channel B (1080p) resisted 4 escalating trials (up to 1,606 decode
  errors, zero audio-sync errors); Channel A (720p) reproduced the
  severe form instantly on the *mildest* method (240 decode errors,
  238 audio-sync errors, 109s peak desync, first attempt). Same
  method/client/machine -- only the channel changed. Next: more Channel A
  trials to confirm, plus a same-resolution comparison channel to
  isolate whether it's resolution or this specific stream's encode.
  **Update: likely root cause found.** A bare `ffprobe` probe of Channel A's raw Dispatcharr proxy stream -- no seek, no Kodi, no addon,
  just a cold TCP connection -- throws ~12 `non-existing PPS 0
  referenced`/`no frame!` errors per second, continuously, for the
  whole 10s test window; Channel B's same probe is essentially clean (1
  unrelated warning). Channel A's keyframe interval is a steady
  2.002s, exactly matching this instance's `segment_seconds=2` setting;
  Channel B's is a steady but unaligned 2.503s. Working theory: Channel A's
  segment-aligned GOP means Dispatcharr's segmenter cuts every segment
  exactly on a keyframe, and something in that exact-alignment path
  corrupts the PPS NAL unit at those cut points almost every time --
  explaining both the worse baseline noise and why a live-edge seek
  (an extra demuxer resync) tips it into the severe cascade so much
  more easily than on Channel B. Points at Dispatcharr's own segmenter for
  this channel's encode, not this addon/Kodi/OS. Not yet confirmed
  against a second segment-aligned channel or a second misaligned one.
  **Update: confirmed via real GUI keypresses, not just synthetic
  seeks.** Peer report -- `StepBack`x7 then `StepForward`x6 (real
  remote/keyboard input, same path as the original Channel B incidents)
  drove a live-edge seek on Channel A and reproduced the identical
  failure signature, peaking ~170s desync, recovering at real-time
  pace. Closes the GUI-vs-JSON-RPC confound. Also: this buffer was
  ~37 minutes old (not freshly opened), so reproducibility on this
  channel doesn't depend on buffer age either.
  **Update: mid-buffer seek control test says this is fixable
  client-side.** Same Channel A buffer, direct A/B: seeking to a
  genuine mid-buffer point produced only ordinary baseline noise (0
  audio-sync-error lines); seeking to the live edge moments later on
  that same buffer immediately produced 2,393 audio-sync-error lines.
  The cascade is specific to the live edge, not the corrupted stream
  in general -- reopens increasing `SeekLiveTimeshiftStream()`'s tail
  backoff margin (currently 1 segment) as a plausible, worth-trying
  client-side mitigation.
  **Update: implemented and tested -- backing off 3 segments instead
  of 1 eliminated the cascade across 12/12 trials on Channel A
  (Windows), vs. reproducing on the 2nd of 6 attempts pre-fix.** Zero
  `large audio sync error` lines in any of 12 attempts; ordinary
  baseline noise unchanged. Not proof it's fully eliminated (12 clean
  trials, not exhaustive), and not yet tested against an older buffer
  or on macOS/CoreELEC -- worth continued normal-use monitoring.
  **Update: macOS confirmation, clean pass.** 10/10 scripted attempts
  (same method as Windows) plus a separate 69-keypress real-keyboard
  session, both on Channel A -- zero `large audio sync error` lines
  in either. Two platforms, two input methods, the one channel that
  reproduced instantly pre-fix, all clean. Buffer age and CoreELEC
  still untested.
  **Update: CoreELEC/ODROID N2+ confirmation, clean pass -- and a
  stronger one than the desktop platforms got (2026-09-09).** 12/12
  live-edge-seek attempts on Channel B (real ESPN channel, not Channel
  A -- deliberately avoided this run), zero `large audio sync error`
  lines in any attempt. Notable: the baseline `non-existing PPS 0
  referenced` noise ran much higher here than Channel B's established
  clean baseline on Windows/macOS (10-269 per ~20s attempt window here
  vs. essentially none there) -- see the new note in
  `docs/TIMESHIFT.md`'s same section. Despite that elevated baseline
  noise, the fix still held with zero escalations, a more demanding
  condition than the earlier clean passes.
  **Update: re-run on Channel A itself on CoreELEC -- the hardest
  channel, still clean (2026-09-09).** Same 12-attempt method, this
  time on the actual channel that's reliably reproduced the severe
  cascade instantly on every other platform. Zero `large audio sync
  error` lines across all 12, with baseline noise running even higher
  than the Channel B run above. All four platform/channel combinations
  tested now (Windows/Channel A, macOS/Channel A, CoreELEC/Channel B,
  CoreELEC/Channel A) are clean.
  **Update: the live-edge-seek fix (PR #3) is now fully closed --
  older-buffer condition tested too, clean (2026-09-09).** Every prior
  trial used a fresh 2-5 minute buffer; the real incident that started
  this investigation happened on a ~37-minute-old one. Left a Channel A
  buffer open and completely untouched for 40 minutes on Windows, then
  ran the same 12-attempt test -- zero `large audio sync error` lines,
  baseline noise in the same ordinary range as every fresh-buffer
  trial. Every dimension tested (two channels, three platforms, two
  input methods, fresh and aged buffers) is now clean. The remaining
  cosmetic baseline `Packet corrupt`/`non-existing PPS 0 referenced`
  noise this whole investigation started from is still present and
  still deliberately unfixed (see this item's own opening paragraph
  above) -- that part was never the target of PR #3, only the severe
  escalation was.
- [x] ~~A periodic, self-correcting `ActiveAE::SyncStream` error spike
  on a suspiciously exact ~8.6s cadence~~ -- **Closed: not actually
  periodic, confirmed on both platforms that ever saw it
  (2026-09-08/09).** Originally found by the macOS peer during what
  looked like ordinary steady-state playback. Tested on Windows (14
  min, zero seeking): one occurrence total, landing 4 seconds after
  that session's own ~89.4s large-sync-error transient at the same
  fresh `Player.Open` -- not periodic. Re-tested on macOS itself (21.6
  min, zero seeking, addon 0.9.1): same result, one occurrence ~3.9s
  after buffer open, nothing else. The peer also traced their original
  observation to a mislabeled sample -- those log lines actually landed
  shortly after a batch of scripted live-edge-seek attempts, not during
  clean steady-state; each clamped-to-tail seek forcing its own small
  demuxer-resync transient plausibly explains the "recurring" look
  without any genuine time-based periodicity. Same family as the
  ~89.4s transient above: a one-off clock-settling blip at stream
  open, not a recurring issue. See `docs/TIMESHIFT.md`'s "A periodic,
  self-correcting ~8.6s `ActiveAE::SyncStream` spike" section for the
  full account.
- [x] **A second, related `Packet corrupt`/freeze, confirmed root-caused
  and fixed, then re-verified live (2026-09-07).** Switching away from a
  channel and back could reproduce a real segment-size disagreement --
  this one *did* trigger the 1.0.7 diagnostic live. Root cause: the
  plugin's per-worker manifest cache wasn't invalidated across a buffer
  restart in a multi-worker deployment (a new ffmpeg process reuses the
  same segment filenames/sequence numbers as its predecessor). A first
  fix tied cache entries to the buffer's own process ID -- itself then
  confirmed live to have a gap under heavy testing churn (OS pids get
  recycled; a stale entry tagged with a since-reassigned pid passed the
  check it should have failed). Fixed properly by keying on the
  buffer's own access token instead (a fresh, random value per genuine
  instance, no reuse risk regardless of churn). Re-verified live on
  macOS across two independent repro conditions, including the specific
  churn pattern that broke the pid-based version -- both clean. Shipped
  in `1.0.7` / `timeshift_buffer` `1.0.5`. See `docs/TIMESHIFT.md`'s
  "1.0.7 follow-up #2" section for the full account.
- [x] **All four platforms now have a completed smoke-test pass** (Windows,
  Rocky Linux, CoreELEC/ODROID, macOS) -- Local timeshift mode is
  confirmed live on all four, closing out what was the last real gap in
  platform coverage.
- ~~A genuine, unexplained anomaly from the macOS pass~~ -- **Likely
  already explained and fixed the next day, entry just never
  reconciled (2026-09-08 review).** Original note (2026-09-05):
  `kodi.log` showed a full `UpdateClients: Recreating PVR client`
  (clean DLL unload/reload, no crash, playback fine right after)
  immediately after dismissing a resume-prompt dialog, with no
  settings change involved -- ruling out the known beta.2
  spurious-restart bug (settings-write-triggered), and not
  reproducing on a second attempt. One day later (2026-09-06,
  `899e5a7`), a separate macOS session hit the identical log signature
  -- `UpdateClients: Recreating PVR client`, clean reload, no crash --
  this time while opening an in-progress recording, and that one *was*
  root-caused and fixed: `OpenRecordedStream()`/`ReadRecordedStream()`
  occasionally self-heal a near-expiry API key mid-call and persist it
  via `SetSettingString()`, which loops back through
  `OnAddonSettingChanged()` and (correctly, by that guard's own logic)
  requests a restart, tearing down the stream that had just opened.
  See `docs/RECORDINGS.md`'s "A self-heal API-key regeneration..."
  section for the full account. Dismissing a resume-prompt dialog for
  a recording leads directly into `OpenRecordedStream()` -- the exact
  vulnerable call site -- and "no settings change involved" is exactly
  how a silent, addon-internal self-heal write would look from a
  tester's side; not reproducing on a second attempt also fits, since
  the trigger depends on the API key's natural expiry timing rather
  than firing every time. Not provable with certainty (the original
  log's own API-key-regeneration line isn't preserved to check
  directly), but strong enough that this shouldn't still read as an
  open mystery. Left here, corrected, rather than deleted, for the
  same reason the original was kept: useful context if anything like
  it resurfaces.
- ~~Catch-up and recurring-timer creation have no clean way to exercise
  via Kodi's JSON-RPC alone~~ -- **Decided: counted as tested anyway, not
  worth building a dedicated driving mechanism for.** The underlying
  logic is platform-independent and already has real live-verification
  from earlier sessions; the gap is in this testing technique
  (JSON-RPC-driven, not GUI-driven), not in the feature.

## Tooling / infrastructure (not addon-specific)

- **No automated test suite exists (requested 2026-09-10) -- recommended
  approach, not yet started.** Two genuinely separable problems, since
  this addon can't be compiled standalone (needs Kodi's own binary-addon
  build harness) while the two companion plugins are plain Python with
  no such constraint:
  - **Python plugins** (`dispatcharr-plugin/recording_edl`,
    `dispatcharr-plugin/timeshift_buffer`): the lower-effort starting
    point -- add `pytest` alongside the existing `ruff.toml`. Both
    plugins already have real, isolated bugs that were verified with
    one-off manual test scripts during development (e.g.
    `docs/RECORDING_EDL.md`'s `_parse_edl()` nan/inf fix, "Verified
    with a test reproducing the exact pre-fix crash") -- a real pytest
    suite would just formalize and keep that same style of test instead
    of writing it, running it once, then discarding it.
  - **C++ addon**: don't attempt to test `PVRDispatcharr`/
    `DispatcharrClient` wholesale -- that would mean mocking Kodi's
    entire addon-instance API and/or standing up a fake Dispatcharr
    HTTP server, disproportionate effort for what's fundamentally still
    manual/live-hardware verification territory (per this project's own
    established `CLAUDE.md`/README convention). Instead, add a small,
    separate CMake target/executable (a header-only framework like
    Catch2 would avoid a real dependency) that compiles and tests only
    the Kodi/Dispatcharr-independent pure-logic pieces already living in
    `src/` -- `XmlTvParser`'s field extraction, the recurring-rule
    timezone offset math (`ComputeKnownZoneOffsetMinutes()` and its
    nth-weekday/last-weekday helpers), `MapCategoriesToGenreType()`'s
    keyword scan, and the broadcast-id hash in `GetEPGForChannel()`.
    None of these touch `kodi::`-namespaced types, so none need the
    Kodi ABI at all -- a real, buildable-standalone test target, not a
    redesign of the addon's own architecture.
- [x] **No doc-linting exists (requested 2026-09-10) -- built (2026-09-10).**
  Motivated directly by this session's own experience: found and fixed 9
  dangling/stale references across `docs/`/`CHANGELOG.md` in one pass,
  several tracing back to a feature removal or CI change that was never
  cross-checked against the docs describing it (see `CLAUDE.md`'s new
  convention bullet on this). An off-the-shelf markdown-link-checker
  (e.g. `markdown-link-check`) would catch a genuinely different class of
  bug than what this session actually found -- every dangling reference
  found here was a plain-English citation (`See docs/EPG.md's "Section
  Title" section`, or a backtick-quoted function/setting name), not a
  broken `[text](url)` hyperlink.
  `tools/check_doc_refs.py`: a small, dependency-free script specific to
  this project's own citation convention, three checks against
  `docs/*.md`/`CHANGELOG.md`: (1) `docs/X.md's "..." section`-style
  citations (also recognizing `[X.md](X.md)`-style relative links and
  this project's bold `**Title**:` paragraph markers, not just real `#`
  headings) against that file's real headings; (2) backtick-quoted
  `Name()` function citations against `src/*.cpp`/`*.h` *and* both
  plugins' `plugin.py` (an early pass missed the plugins entirely, a real
  gap, not just noise); (3) backtick-quoted setting ids on lines
  mentioning "setting" against `resources/settings.xml` *and* both
  plugins' own settings/action ids (a separate namespace from the Kodi
  addon's settings.xml).
  First real run found 78 hits after fixing several bugs in the checker
  itself found via its own output (the multi-line lookback for which file
  a citation belongs to, the markdown-link form, the bold-paragraph
  heading style, and the missing plugin.py corpus) -- the remaining hits
  are overwhelmingly genuine, correct citations to *external* code this
  project's docs deliberately reference by design (Kodi-core source,
  hls.js, Python stdlib/syscalls) or to deliberately-documented
  historical/removed settings, not real dangling references. Confirmed by
  spot-checking rather than assumed: `enable_live_timeshift` (flagged as
  "setting not found") is explicitly, correctly documented in
  `docs/TIMESHIFT.md` as a removed setting, replaced by
  `live_timeshift_mode`. This is inherent to a lightweight static-text
  heuristic given how citation-heavy this project's own docs style is by
  design (see `CLAUDE.md`'s "Confirmed live" citations bullet).
  **Update: baselined and wired into CI (2026-09-10).** Re-triaging the
  same 76 known-legitimate hits (a couple resolved themselves once the
  checker's own remaining bugs above were fixed) on every run wasn't
  useful, so `tools/doc_refs_baseline.txt` records them (keyed on the
  citation itself, not its line number, so an unrelated doc edit doesn't
  shift the baseline) and the CI `lint` job now runs
  `python3 tools/check_doc_refs.py` unconditionally, failing only on a
  genuinely new dangling reference not already in the baseline --
  confirmed by a throwaway test file with a fake dangling function
  reference, which the script correctly flagged and failed on (exit 1),
  while a real, clean run stayed silent (exit 0). `--update-baseline`
  accepts new legitimate hits (a real external citation, or documenting a
  newly-reverted approach) into the baseline by hand when that judgment
  call is needed -- this still isn't a fully automatic pass/fail gate for
  *legitimacy*, just for *novelty*.
