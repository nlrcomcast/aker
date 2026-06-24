## Why

Aker currently enforces MAC-blocking schedules silently: the only outward signal
when a device enters or leaves a downtime window is the firewall command and the
periodic assurance metrics. End users and upstream services have no advance warning
or confirmation when a managed device is about to lose (or regain) network access.
This change adds proactive, per-MAC downtime lifecycle notifications so subscribers
can be alerted before and at the moment each device transitions.

## What Changes

- Add four new push notifications, emitted over the Telemetry 2.0 (T2) bus, that
  track the downtime lifecycle of each MAC governed by the active schedule:
  - `DOWNTIME_STARTING_SOON` — 15 minutes before a MAC's downtime window begins.
  - `DOWNTIME_STARTED` — at the moment a MAC's downtime window begins.
  - `DOWNTIME_ENDING_SOON` — 15 minutes before a MAC's downtime window ends.
  - `DOWNTIME_ENDED` — at the moment a MAC's downtime window ends.
- Compute **per-MAC downtime windows** from the existing weekly/absolute schedule
  by tracking, for each MAC, the maximal consecutive spans during which its index
  appears in successive schedule events' block lists. Both weekly and absolute rules
  participate, and the weekly schedule is treated as a circular timeline so a MAC
  blocked across the Sunday-midnight wrap forms a single window.
- Always emit all four notification types for every window regardless of duration;
  for windows shorter than 15 minutes the `*_SOON` trigger may occur before the
  window actually begins.
- Persist the set of already-emitted notification boundaries to disk and reload it on
  startup so a `*_STARTING_SOON`/`*_ENDING_SOON` is not missed if the process restarts
  inside the 15-minute lead.
- Batch MACs that share an identical transition time and event type into a single
  notification payload (the `affectedMacs` array), and emit separate notifications
  for distinct transition times.
- Emit a JSON payload per notification containing `eventType`, `currentTime`
  (CPE time), and `affectedMacs[]` with each MAC's resolved `DownStartTime` and
  `DownEndTime`, all timestamps in a human-readable UTC format. Delivery uses a
  standard `t2event` (single JSON string via `t2_event_s`).
- Extend the scheduler's wake-up calculation so the worker thread also wakes at the
  next notification boundary (window edges and their 15-minute lead points), not
  only at block/unblock transitions and report times.
- Gate all notification delivery behind the existing `ENABLE_FEATURE_TELEMETRY2_0`
  compile-time flag so non-T2 builds are unaffected.

## Impact

- Affected specs:
  - `downtime-notifications` (new capability) — notification event model, payload
    schema, and T2 delivery.
  - `schedule-evaluation` (new capability documenting current behavior, with added
    requirements) — per-MAC downtime window computation and next-transition
    wake-time integration.
- Affected code:
  - [src/scheduler.c](src/scheduler.c) — hook notification evaluation into the
    worker loop and extend the wake-up time selection.
  - [src/schedule.c](src/schedule.c) / [src/schedule.h](src/schedule.h) — per-MAC
    downtime window computation and next-notification-boundary helpers.
  - [src/time.c](src/time.c) / [src/time.h](src/time.h) — UTC ISO-8601 timestamp
    formatting and weekly↔unix boundary helpers for the 15-minute lead.
  - New `src/notify.c` / `src/notify.h` — payload construction, T2 emission via a
    standard `t2event`, and persistence/reload of already-sent notification
    boundaries, guarded by `ENABLE_FEATURE_TELEMETRY2_0`.
  - [src/CMakeLists.txt](src/CMakeLists.txt) and
    [tests/CMakeLists.txt](tests/CMakeLists.txt) — register the new translation
    unit and its tests.
- No backward-incompatible changes: schedule payload format, persistence, WRP CRUD
  handling, and existing metrics are unchanged. Notifications are additive and
  feature-flagged.
