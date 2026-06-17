## Context

Aker's scheduler thread already detects blocked-MAC transitions by comparing the output of `get_blocked_at_time()` on each loop iteration. Today it only acts on transitions by calling `call_firewall()`. This change adds a parallel notification path that computes per-MAC diffs and emits T2 events at transitions and 15 minutes before them.

**Stakeholders:** Aker maintainers, CPE telemetry consumers, parental-controls UX team.

**Constraints:**
- Must remain behind `ENABLE_FEATURE_TELEMETRY2_0` build flag
- Must not alter existing firewall or metrics behavior
- Must be Valgrind-clean
- Thread-safe: notification logic runs inside the scheduler thread under `schedule_lock`

## Goals / Non-Goals

**Goals:**
- Emit T2 notifications for four downtime lifecycle events per affected MAC
- Pre-notify 15 minutes (900 s) before each blocking/unblocking transition
- Correctly handle weekly schedule wrap-around (Saturday → Sunday boundary)
- Correctly handle schedule replacement mid-cycle (cancel stale pre-notifications)

**Non-Goals:**
- Sending notifications via WRP EVENT messages (only T2 for now)
- Batching notifications across multiple transition points
- Persisting notification history to disk
- Supporting configurable pre-notification lead times (hardcoded 900 s)

## Decisions

### 1. Notification computation model: diff-based, on-demand

**Decision:** Compute notifications by diffing the blocked-MAC set between consecutive `schedule_event_t` nodes at schedule load time and caching a notification event list.

**Alternatives considered:**
- *Real-time diff in scheduler loop:* Simpler but cannot produce the 15-minute pre-notification because the scheduler only wakes at transition boundaries today. Would require two-pass wakeup logic.
- *Pre-computed notification list (chosen):* At schedule decode time, walk the event list and compute a parallel sorted linked list of `notification_event_t` structs. The scheduler thread consults this list for the next notification time, enabling a single `pthread_cond_timedwait` with `min(next_transition, next_notification, next_report)`.

**Rationale:** Pre-computation avoids repeated set-diff work on every scheduler loop. The notification list is O(E) where E is the number of schedule events, which is small (typically < 20).

### 2. Notification payload format: JSON string via `t2_event_s`

**Decision:** Serialize the notification payload as a JSON string and pass it to `t2_event_s("AkerDowntimeNotification", json_str)`.

**Alternatives considered:**
- *Msgpack payload via WRP EVENT:* Consistent with metrics reporting, but the requirement explicitly calls for T2 and JSON format.
- *Structured T2 call (t2_event_d):* Requires a different T2 API. The existing codebase only uses `t2_event_s`; sticking with it minimizes risk.

### 3. Scheduler wake-up integration

**Decision:** Extend the existing timeout calculation in `scheduler_thread()` to include the next notification fire time. A new function `get_next_notification_time()` returns the earliest pending notification time. The scheduler picks `min(next_event, next_notification, next_report)` as its `timedwait` deadline.

### 4. Data structures

```
typedef enum {
    NOTIFY_DOWNTIME_STARTING_SOON,
    NOTIFY_DOWNTIME_STARTED,
    NOTIFY_DOWNTIME_ENDING_SOON,
    NOTIFY_DOWNTIME_ENDED
} notify_event_type_t;

typedef struct notification_event {
    time_t fire_time;               /* Weekly time (seconds since Sunday) to fire */
    time_t scheduled_time;          /* The actual transition time this relates to */
    notify_event_type_t type;
    size_t mac_count;
    struct notification_event *next;
    uint32_t mac_indexes[];         /* Indexes into schedule_t.macs[] */
} notification_event_t;
```

The notification list is rebuilt whenever `process_schedule_data()` installs a new schedule. The old list is freed along with the old `schedule_t`.

### 5. Module boundary

New files:
- `src/aker_notify.h` — public API: build notification list, get next time, fire due notifications, destroy list
- `src/aker_notify.c` — implementation
- `tests/test_aker_notify.c` — unit tests with pre-defined schedule fixtures

## Risks / Trade-offs

| Risk | Mitigation |
|------|------------|
| 15-min pre-notification fires after schedule replacement | Notification list is rebuilt atomically with schedule swap; stale list freed |
| Weekly wrap-around edge case (last event → first event of next week) | Diff computation wraps: compare last event's blocked set against first event's blocked set |
| Clock jumps (NTP sync, DST) cause missed or duplicate notifications | Existing `get_unix_time()` + `convert_unix_time_to_weekly()` already handles this; notification times are recomputed on each loop |
| JSON serialization adds code size | Use `snprintf`-based formatting (no JSON library dependency) since payload structure is fixed |

## Migration Plan

- No migration needed; this is a purely additive feature behind an existing build flag.
- Rollback: revert the commits; no data format or API changes to undo.

## Resolved Questions

1. **T2 notification name:** `"AkerDowntimeNotification"` — this is a notification, not a marker.
2. **Payload transport:** JSON string via `t2_event_s("AkerDowntimeNotification", json_string)`.
3. **Sample data corrections:** Confirmed — `time=86490` and `time=250200` entries should use `DOWNTIME_ENDED` (not `DOWNTIME_STARTED`).
4. **`currentTime` field:** Actual CPE system time at the moment of send.
