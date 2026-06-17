## Why

Aker currently enforces MAC-based downtime schedules by toggling firewall rules at transition boundaries, but provides no proactive notifications to downstream systems or end users about upcoming or active downtime events. Adding T2 push notifications for schedule transitions enables real-time awareness of downtime state changes, which supports user-facing features such as parental-control alerts and device-management dashboards.

## What Changes

- **New module: `aker_notify`** — computes per-MAC transition diffs between consecutive schedule events and emits T2 notifications with JSON payloads
- **Scheduler thread modification** — the `pthread_cond_timedwait` timeout calculation now also considers pre-notification times (event time − 900 s) so the thread wakes 15 minutes before each transition
- **New T2 event markers** — four event types sent via the existing `t2_event_s` / Telemetry 2.0 path:
  - `DOWNTIME_STARTING_SOON` — 15 min before MACs become blocked
  - `DOWNTIME_STARTED` — when MACs become blocked
  - `DOWNTIME_ENDING_SOON` — 15 min before MACs become unblocked
  - `DOWNTIME_ENDED` — when MACs become unblocked
- **Build system** — new source/header pair added to `src/CMakeLists.txt`; test executable added to `tests/CMakeLists.txt`

## Impact

- Affected specs: `downtime-notifications` (new capability)
- Affected code:
  - `src/scheduler.c` — timeout calculation, notification trigger points
  - `src/schedule.c` / `src/schedule.h` — new helper to diff blocked-MAC lists between events
  - New files: `src/aker_notify.c`, `src/aker_notify.h`
  - `src/CMakeLists.txt`, `tests/CMakeLists.txt`
  - New test: `tests/test_aker_notify.c`
- No **BREAKING** changes — the existing scheduler, firewall, and metrics flows are unchanged
- Guarded by `ENABLE_FEATURE_TELEMETRY2_0` build flag (same as existing T2 metrics)

## Resolved Clarifications

1. **T2 notification name:** `"AkerDowntimeNotification"` (notification, not a marker).
2. **Sample data corrections confirmed:**
   - At `time=86490`: MAC `d6:0b:68:4f:15:a0` → **`DOWNTIME_ENDED`** (was incorrectly listed as `DOWNTIME_STARTED`).
   - At `time=250200`: MACs `d6:0b:68:4f:15:a0`, `c4:84:66:29:0f:9d` → **`DOWNTIME_ENDED`** (was incorrectly listed as `DOWNTIME_STARTED`).
3. **Payload transport:** JSON string via `t2_event_s("AkerDowntimeNotification", json_string)`.
4. **Schedule replacement:** Pending pre-notifications for the old schedule are cancelled silently (list freed, no cancellation notification).
5. **Weekly wrap-around:** Yes, `DOWNTIME_STARTING_SOON` fires 15 minutes before the first event of the next week cycle.
