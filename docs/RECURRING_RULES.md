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

No `PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE`: this addon doesn't
implement `UpdateTimer()` at all (a pre-existing gap, see
`docs/RECORDINGS.md`), so there's no way to flip a rule's `enabled` flag
from Kodi's timer list -- delete the timer to stop it for good instead.

No per-timer "end date" either: Kodi's own repeating-timer type
attributes have `PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY` but no equivalent
"last day" flag (confirmed against kodi-dev-kit's full
`PVR_TIMER_TYPE_SUPPORTS_*` list), while Dispatcharr's serializer
requires a real `end_date` on every create regardless. A newly-created
rule gets `start_date + 3 years` (`kRecurringRuleDefaultYears`) --
cheap, since only the next 14 days actually materialize into real
`Recording` rows at any given time (Dispatcharr's own rolling horizon);
delete the timer whenever you actually want it to stop.

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
