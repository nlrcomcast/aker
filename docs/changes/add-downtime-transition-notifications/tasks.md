## 1. Per-MAC downtime window computation

- [x] 1.1 Add a helper in [src/schedule.c](src/schedule.c) /
  [src/schedule.h](src/schedule.h) that, given a schedule, a MAC index, and an
  instant, returns that MAC's current/next downtime window edges (start, end) by
  walking the ordered event list (weekly and absolute), handling the
  `SECONDS_IN_A_WEEK` wrap.
- [x] 1.2 Add a helper that returns, for an instant, the next notification boundary
  across all MACs and all four event types (window start, window end, and each edge
  minus 900 s), normalized modulo `SECONDS_IN_A_WEEK` for week-crossing leads.
- [x] 1.3 Add unit tests in `tests/` driven by the worked example schedule that
  assert the computed windows and boundaries for each MAC (d6, c4, 99, 90) match the
  expected transition instants.

## 2. Notification payload and T2 delivery

- [x] 2.1 Create `src/notify.c` / `src/notify.h` with a function that builds the JSON
  payload (`eventType`, `currentTime`, `affectedMacs[]` with `devicemac`,
  `DownStartTime`, `DownEndTime`) and emits it over T2, guarded by
  `ENABLE_FEATURE_TELEMETRY2_0` (no-op when disabled).
- [x] 2.2 Add UTC ISO-8601 (`%Y-%m-%dT%H:%M:%SZ`) timestamp formatting in
  [src/time.c](src/time.c) / [src/time.h](src/time.h) for `currentTime` and the
  per-MAC window edges.
- [x] 2.3 Implement batching so MACs sharing the same `eventType` and instant are
  placed in one payload's `affectedMacs` array.
- [x] 2.4 Add unit tests asserting payload shape, UTC formatting, and batching using
  the worked example (e.g. `c4` + `90` batched for `DOWNTIME_ENDING_SOON`).

## 3. Scheduler integration

- [x] 3.1 In [src/scheduler.c](src/scheduler.c), fold the next notification boundary
  (task 1.2) into the `pthread_cond_timedwait` deadline alongside
  `get_next_unixtime()` and `next_report_time`, selecting the earliest.
- [x] 3.2 On wake, emit every notification whose transition instant equals the
  current instant; suppress duplicate emission within a wake cycle and recompute the
  next boundary before waiting again.
- [x] 3.3 Recompute boundaries from the now-current schedule after a schedule
  replacement (the existing `cond_var` signal path).
- [x] 3.4 Add a scheduler-level test that advances time across the worked example and
  asserts the ordered sequence of emitted notifications.

## 4. Restart persistence

- [x] 4.1 Persist the set of already-emitted notification boundaries (keyed by
  absolute instant and event type) to disk alongside the schedule data.
- [x] 4.2 On startup, reload the persisted state, suppress re-emission of boundaries
  already sent, and emit any boundary whose instant has passed but was not yet sent.
- [x] 4.3 Add tests covering a restart inside the 15-minute lead (the `*_SOON` is
  still emitted) and a restart after emission (no re-emission).

## 5. Build, edge cases, and validation

- [x] 5.1 Register `src/notify.c` in [src/CMakeLists.txt](src/CMakeLists.txt) and the
  new test executables in [tests/CMakeLists.txt](tests/CMakeLists.txt).
- [x] 5.2 Add tests for week-boundary and DST edge cases: a window straddling Sunday
  midnight treated circularly, a `*_SOON` lead that crosses the week wrap, and a
  timezone with DST.
- [x] 5.3 Add a test for a downtime window shorter than 15 minutes asserting all four
  event types are emitted and the `*_SOON` lead instant before window start is not
  suppressed.
- [ ] 5.4 Verify a clean build with `-Werror -Wall` and confirm the non-T2 build
  (flag disabled) compiles the emission path to a no-op with no behavior change.
  _(Not executed here: no C build toolchain available in this environment. Code was
  reviewed statically for `-Werror -Wall -W` cleanliness; the scheduler/main emission
  path is fully gated behind `ENABLE_FEATURE_TELEMETRY2_0` so the flag-disabled build
  is a no-op.)_
- [ ] 5.5 Run the full suite (`make test`) under Valgrind and confirm no new leaks
  and all notification tests pass.
  _(Not executed here: no compiler/Valgrind toolchain available. Tests authored in
  `tests/test_notify.c` and registered in `tests/CMakeLists.txt`; payloads and persistence
  buffers use matched aker_malloc/aker_free to avoid leaks.)_
