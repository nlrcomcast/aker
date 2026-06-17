## 1. Notification Data Structures and Builder

- [x] 1.1 Create `src/aker_notify.h` with `notify_event_type_t` enum, `notification_event_t` struct, and public API declarations (`build_notification_list`, `destroy_notification_list`, `get_next_notification_time`, `fire_due_notifications`)
- [x] 1.2 Implement `build_notification_list()` in `src/aker_notify.c` — walk `schedule_event_t` linked list, diff consecutive blocked-index sets, produce sorted notification events (including wrap-around pair)
- [x] 1.3 Implement `destroy_notification_list()` — free all nodes
- [x] 1.4 Add `src/aker_notify.c` and `src/aker_notify.h` to `src/CMakeLists.txt`

**Validation:** Unit tests in task 4. Can be developed in parallel with task 2.

## 2. JSON Payload Serialization

- [x] 2.1 Implement `format_notification_json()` in `src/aker_notify.c` — produce the JSON payload string given event type, scheduled UTC time, current CPE time, and affected MAC list
- [x] 2.2 Implement `send_notification_t2()` — wraps `t2_event_s()` call behind `ENABLE_FEATURE_TELEMETRY2_0` guard; accepts formatted JSON string

**Validation:** Unit test verifies JSON output matches expected format. Can be developed in parallel with task 1.

## 3. Scheduler Integration

- [x] 3.1 Add notification list pointer alongside `current_schedule` in `scheduler.c` file-scoped variables
- [x] 3.2 In `process_schedule_data()`: after installing new schedule, call `build_notification_list()` and store result; free old notification list with old schedule
- [x] 3.3 Implement `get_next_notification_time()` in `src/aker_notify.c` — given current unix time, return the next notification fire time (or `INT_MAX`)
- [x] 3.4 In `scheduler_thread()`: add `get_next_notification_time()` result to the `min(next_event, next_report)` timeout calculation
- [x] 3.5 In `scheduler_thread()`: after `pthread_cond_timedwait` returns, call `fire_due_notifications()` to send any notifications whose fire time ≤ current time
- [x] 3.6 Guard all notification calls with `#if defined(ENABLE_FEATURE_TELEMETRY2_0)`

**Depends on:** Tasks 1 and 2.

## 4. Unit Tests

- [x] 4.1 Create `tests/test_aker_notify.c` with CUnit test suite
- [x] 4.2 Test: `build_notification_list` with the example schedule from the requirements — verify correct event types, fire times, and affected MAC indexes
- [x] 4.3 Test: `build_notification_list` with empty schedule — verify NULL/empty list
- [x] 4.4 Test: `build_notification_list` with single-event schedule — verify wrap-around notifications
- [x] 4.5 Test: `format_notification_json` — verify JSON structure and field values
- [x] 4.6 Test: `get_next_notification_time` — verify correct next time for various current-time inputs
- [x] 4.7 Test: events closer than 900 s apart — verify both pre-notifications are generated independently
- [x] 4.8 Add `test_aker_notify` target to `tests/CMakeLists.txt` with Valgrind integration
- [x] 4.9 Run full test suite (`make test`) and Valgrind — confirm clean

**Can be developed in parallel with tasks 1–2; final run depends on task 3.**

## 5. Documentation and Changelog

- [x] 5.1 Add entry to `CHANGELOG.md` under `[Unreleased]` describing the new T2 downtime notifications
