*(part of the pvr.dispatcharrai notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Recurring (day-of-week) timer rules

Implemented on top of Dispatcharr's own `RecurringRecordingRule` model,
which already existed in its source with nothing on this addon's side
wired up to it. Confirmed fully live/reachable before building against it
(not assumed): model -> serializer -> `RecurringRecordingRuleViewSet`
(`GET`/`POST /api/channels/recurring-rules/`, `GET`/`PUT`/`PATCH`/`DELETE
.../{id}/`) -> an hourly Celery beat task
(`maintain_recurring_recordings`) -> `sync_recurring_rule_impl`, which
materializes real `Recording` rows up to 14 days ahead for every enabled
rule, tagging each with `custom_properties.rule = {"type": "recurring",
"id": <rule id>, ...}`.

## Kodi-side representation: parent rule + child instances

Uses Kodi's standard repeating-timer convention (`PVRTimer::
SetParentClientIndex()`, confirmed in kodi-dev-kit's `pvr_timers.h`) --
not a bespoke scheme:

- The rule itself is one "parent" timer (`kTimerTypeRecurring`,
  `PVR_TIMER_TYPE_IS_REPEATING`), `ClientIndex = rule.id |
  kRecurringRuleIndexFlag` (`0x20000000`, distinct from series rules'
  own `0x40000000` hash-based namespace -- recurring rules have a real
  numeric id, so no hashing is needed here, just a separate bit).
- Each already-materialized `Recording` (an upcoming or in-progress
  occurrence) is a plain one-time timer, same as ever, but with
  `SetParentClientIndex(rule.id | kRecurringRuleIndexFlag)` set when its
  `custom_properties.rule.id` is present -- linking it back to its
  parent in Kodi's own timer list UI.
- Deleting the parent (`DeleteTimer()`) deletes the Dispatcharr rule,
  which also purges its own *future* (`start_time__gte=now`) recordings
  server-side -- confirmed live: deleting a rule while its current
  occurrence was actively recording correctly left that one alone
  (Dispatcharr's own purge query only matches future recordings), while
  an earlier test deleting a rule *before* its occurrence's start time
  removed both the rule and the not-yet-started recording together.
  Deleting an individual child instance instead falls through to the
  ordinary one-time-recording delete path (`DeleteRecording()`) --
  Dispatcharr's own idempotency check has no "user explicitly skipped
  this day" concept, so the next hourly sync could re-materialize a
  deleted single occurrence if it's still within the rule's own window;
  this is a Dispatcharr-side limitation, not something addressed here.

**Update: `UpdateTimer()` is now implemented** (see the "UpdateTimer()"
entry in `docs/RECORDINGS.md` for the full cross-timer-type story), so
`PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE` is declared for this type and a
rule's `enabled` flag can be flipped straight from Kodi's timer list --
Kodi implements that action by calling `UpdateTimer()` with everything
else unchanged and just the state flipped, the same call full edits use.
Deleting the timer is still the way to stop a rule for good, not just
pause it.

No per-timer "end date" either: Kodi's own repeating-timer type
attributes have `PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY` but no equivalent
"last day" flag (confirmed against kodi-dev-kit's full
`PVR_TIMER_TYPE_SUPPORTS_*` list), while Dispatcharr's serializer
requires a real `end_date` on every create regardless. Delete the timer
whenever you actually want it to stop.

**Update: a rolling `end_date` window, not a flat 3-year one.** This
used to set `end_date` to `start_date + 3 years` on creation, on the
theory that Dispatcharr only lazily materializes ~14 days ahead
regardless of how far out `end_date` sits, making a far-future value
"free". **Confirmed live that theory was wrong**: creating a real weekly
rule with a 3-year `end_date` caused Dispatcharr to eagerly materialize
*every* occurrence between `start_date` and `end_date` synchronously,
right at creation -- 157 real `Recording` rows for one rule, which would
have shown as 157 child timers in Kodi's own timer list (more for a rule
repeating on several days a week). Now a rule's `end_date` is kept to a
rolling `kRecurringRuleWindowDays` (30) window instead, topped back up
periodically by `RenewRecurringRules()` (piggybacking on the existing
recording-refresh background thread, no separate thread or setting)
once less than half that window remains -- so a recurring rule still
behaves like "create once, forget about it" from the user's side, just
without the eager-materialization cost. Confirmed live, separately, that
extending `end_date` is safe to do while an occurrence is actively
recording: it only regenerates *future* (not yet started) occurrences
-- a real in-progress recording, id 458, kept its id, `started_at`, and
file path completely unchanged across a mid-recording `end_date` PATCH,
and went on to complete normally (`status: "completed"`,
`remux_success: true`, real bytes written). `RenewRecurringRules()`
still skips a rule with an occurrence currently recording or starting
within the next hour regardless, as defense in depth rather than relying
solely on that server-side scoping.

## The timezone problem, and why it needed a user-facing setting

Confirmed against Dispatcharr's own source (`sync_recurring_rule_impl`
in `apps/channels/tasks.py`), not assumed: a rule's `start_time`/
`end_time` are naive `"HH:MM:SS"` values with **no timezone attached**,
combined with each target date using Dispatcharr's own configured
system timezone CoreSetting (`CoreSettings.get_system_time_zone()`,
readable via `GET /api/core/settings/`'s `system_settings.time_zone`
value -- an IANA zone name, e.g. `"America/Chicago"`, confirmed live
against a real instance). There is no server-side normalization at all
-- the `RecurringRecordingRuleSerializer` only does naive
same-timezone comparisons for validation ordering.

This addon operates entirely in UTC internally (every other timestamp
it handles is UTC, confirmed consistent throughout its whole history),
and doesn't bundle a real IANA timezone database -- adding one for
correct automatic DST-aware conversion across Windows/macOS/CoreELEC
would be a large, disproportionate dependency for this one feature.
Given the real, practical alternative -- **refuse to create a recurring
rule at all unless Dispatcharr's configured system timezone happens to
be UTC** -- would have blocked the feature outright for a real user's
own instance (confirmed configured to `America/Chicago`, not UTC), the
setting `recurring_rule_utc_offset_minutes` (default `0`) was added
instead: a plain manual UTC-offset the user sets to match Dispatcharr's
*current* offset (e.g. `-360` for US Central Standard Time, `-300` for
Central Daylight Time), used to shift Kodi's UTC-based timer start/end
time-of-day into Dispatcharr's own wall-clock convention before sending
it, and shift back the same way when reading rules back for display.
Deliberately simple, not a real timezone library: a zone that observes
DST needs this setting flipped by 60 minutes twice a year, or a
recurring timer will fire an hour off until it's updated -- explained
directly in the setting's own help text.

The conversion math itself needs no timezone library either way: every
value that actually matters here (Kodi's `GetStartTime()`/
`GetEndTime()`/`GetFirstDay()`) is already a UTC `time_t`, and a UTC
`time_t`'s own modulo-86400 gives an exact, DST-free calendar-day/
time-of-day split with no `gmtime`/`timegm` round-trip needed -- the
*only* place a real timezone enters is the explicit, user-supplied
offset shift.

## Confirmed live, end-to-end

Bypassed Kodi's own timer-creation GUI for the create/read verification
(Kodi's JSON-RPC `PVR.AddTimer` only supports the "record from EPG
broadcast" flow, confirmed via `JSONRPC.Introspect` -- no field-level
control over weekdays/times/channel for a manual recurring rule), so
this addon's own client-side conversion math was verified independently
against a real API call using the exact values it would compute, then
against the addon's actual `GetTimers()` output over JSON-RPC once the
rule existed server-side:

1. Directly created a real rule via the API (channel with a real,
   working stream and real EPG; `days_of_week: [3]` for Thursday,
   `start_time`/`end_time` "00:50:00"/"00:52:00", matching what this
   addon's own code would compute for a Kodi timer with weekday bit 3
   set, `recurring_rule_utc_offset_minutes` at `-300` (confirmed CDT,
   the real live offset at the time of testing)). Confirmed the rule's
   own `perform_create` immediately materialized a real `Recording`
   (not waiting on the hourly task) with `start_time:
   "2026-09-03T05:50:00Z"` -- exactly the expected UTC instant for
   00:50 CDT, confirming the offset assumption and Dispatcharr's own
   interpretation of it match.
2. Restarted Kodi and read the rule back through the addon's actual
   `GetTimers()` over JSON-RPC: `weekdays: ["thursday"]` (confirms the
   bitmask round-trip), `starttime`/`endtime`: `"2026-09-03
   05:50:00"`/`"...05:52:00"` (this addon's own reverse conversion,
   independently reproducing the exact UTC values from step 1),
   `firstday: "2026-09-03"`, `istimerrule: true` for the parent
   (`timerid 2`) and `istimerrule: false` for its child instance
   (`timerid 3`, correctly showing `state: "recording"` once its start
   time arrived -- also confirmed via a real-time-updates
   `recording_started` event in `kodi.log` at exactly `00:50:00`).
3. Deleted the parent timer via `PVR.DeleteTimer` while its child
   instance was actively recording: the rule and Kodi's own parent
   timer entry disappeared, but the still-recording child correctly
   remained (Dispatcharr's own purge only removes *future*
   recordings) -- it went on to finish normally
   (`status: "completed"`, `remux_success: true`, real bytes written)
   as a genuine, playable recording, confirming the whole pipeline
   works for real, not just at the metadata level.

Cleaned up all test rules/recordings afterward.

## Auto-computing the offset for common zones, instead of asking for it

The manual `recurring_rule_utc_offset_minutes` setting above works, but has
an obvious flaw a user pointed out directly: if it's correctly set, it
should always equal whatever Dispatcharr's own configured timezone
currently is -- there's no legitimate reason for it to differ, and if it
drifts (most commonly: a DST-observing zone that needs flipping twice a
year, and nobody remembers to), recurring timers silently fire at the
wrong wall-clock time with no error surfaced anywhere.

**Step one, surfacing the zone name.** Dispatcharr's configured system
timezone is readable as a plain IANA zone name via
`GET /api/core/settings/`'s `system_settings.time_zone` field (confirmed
live: `"America/Chicago"`) -- this was already true, just not read by this
addon. Added `DispatcharrClient::GetSystemTimeZone()` (generalizing the
existing single-purpose `FindDvrSettingsRow()` into
`FindCoreSettingsRow(key, ...)` so both the DVR-padding row and this new
`system_settings` row share the same lookup) and a new read-only
`dispatcharr_timezone_info` setting, synced on every restart the same
"only rewrite if actually different" way the DVR padding settings already
work. Doesn't solve the drift problem by itself -- just gives a concrete
reference instead of making the user separately check Dispatcharr's own
admin panel -- but is a genuine, if small, improvement on its own.

**Step two, actually computing the offset.** A zone *name* alone doesn't
give a numeric offset without real DST transition rules, which is exactly
why a full IANA timezone database was ruled out for this addon to bundle.
But DST rules for a handful of common zones are simple, stable, and
well-documented enough to hardcode directly:

- **US/Canada** (Energy Policy Act of 2005, in effect since 2007): starts
  2:00am local *standard* time on the 2nd Sunday of March, ends 2:00am
  local *daylight* time on the 1st Sunday of November.
- **UK/EU**: starts 01:00 UTC on the last Sunday of March, ends 01:00 UTC
  on the last Sunday of October -- defined directly in UTC, no per-zone
  offset needed for the transition moments themselves, unlike the
  US/Canada rule.

Implemented as `DispatcharrClient::ComputeKnownZoneOffsetMinutes()` --
pure UTC `time_t` arithmetic (nth-weekday-of-month / last-weekday-of-month
helpers, reusing the existing `PortableTimeGm()` cross-platform helper, no
platform timezone API or bundled database involved at all), covering 25
zones (US Eastern/Central/Mountain/Pacific/Alaska + Arizona/Hawaii's
no-DST cases, five Canadian zones, UK/Ireland, six Central European zones,
three Eastern European zones, plus UTC itself). Deliberately excludes
Southern Hemisphere zones (Australia's DST runs the opposite direction,
October-April, with its own date rules) and anywhere else not on this
list -- out of scope for this pass, falls back to the existing manual
entry.

**Verified independently before trusting it against real timers**: the
exact same nth-weekday/last-weekday/UTC-conversion algorithm was
reimplemented in Python (using its trusted stdlib `datetime`, not this
addon's own logic, as the independent check) and run against published,
verifiable US and EU DST transition dates for 2025-2027 -- every single
computed date matched exactly. Also cross-checked live: computing
`America/Chicago`'s offset for "right now" returned `-300` (CDT), matching
the real, already-known-correct value already configured on a live
instance.

**A real correctness bug found and fixed before it ever shipped as
final**: the first version of this computed the offset once at addon
*startup* and wrote it into `recurring_rule_utc_offset_minutes` as a plain
number -- self-healing on every restart, but stale for anyone who leaves
Kodi running continuously across an actual DST transition (rare, but a
real gap, not hypothetical: Kodi is exactly the kind of app people leave
running for weeks). Fixed by not caching a *computed offset* at all --
instead a new `recurring_rule_timezone` setting stores the *zone name*
(auto-selected to match Dispatcharr's own reported zone when it's one of
the 25 known ones, "manual" otherwise), and
`PVRDispatcharr::EffectiveRecurringRuleUtcOffsetMinutes()` calls
`ComputeKnownZoneOffsetMinutes()` fresh at the two actual points the
offset is used (`GetTimers()`'s read-back, and
`ComputeRecurringRuleFields()`'s create/update path) rather than once at
construction. Deliberately not cached in a member either, unlike most
other settings this class caches in an `std::atomic` -- both call sites
are synchronous PVR callbacks on Kodi's own thread, not a background
polling loop, so there's no thread-safety reason to cache it the way
`m_recordingRefreshMinutes` needs to for its own thread.

Confirmed live end-to-end after the fix: deliberately set the manual
offset to a wrong value (`0`) with `recurring_rule_timezone` still on
`America/Chicago`, restarted, and confirmed via `kodi.log` it corrected
back to reading through the zone-based computation rather than trusting
the stale manual value -- the log line
(`setting recurring_rule_timezone=America/Chicago (known zone)`) confirms
the sync fired and the dropdown, not the manual number, is what's actually
authoritative for a known zone.
