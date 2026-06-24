## Context

Aker evaluates a `schedule_t` (weekly recurring and absolute one-time rules) on a
background thread and, on each transition, computes the set of blocked MAC addresses
and invokes an external firewall command. Today the schedule is interpreted at the
**whole-set** level: [src/schedule.c](src/schedule.c) `get_blocked_at_time()` returns
the blocked-MAC string for "now", and `get_next_unixtime()` returns the next event
boundary so the worker can sleep until then.

The new requirement is **per-MAC lifecycle awareness**: each MAC has its own downtime
window(s), and four notifications must fire relative to each window's edges. Because
different MACs enter and leave at different events, the per-set view is insufficient —
the same schedule event can simultaneously be a "start" for one MAC and an "end" for
another (e.g., at `t=86490` in the example, MAC `90` starts while MAC `0` ends).

Constraints:
- C99, must compile under `-Werror -Wall`; embedded/Yocto build must keep working.
- Weekly rule times are "seconds since the most recent Sunday" and wrap at
  `SECONDS_IN_A_WEEK` (604800). A 15-minute lead can cross the week boundary
  (negative or wrapped time).
- Timezone-aware: weekly evaluation uses local time; notification timestamps must be
  UTC and human-readable.
- T2 delivery already exists only behind `ENABLE_FEATURE_TELEMETRY2_0`.

## Goals / Non-Goals

- Goals:
  - Deterministically compute, for any MAC and any instant, its current/next downtime
    window edges from the active schedule.
  - Emit the four notifications at the correct instants (window edge, and edge minus
    900 s), batching MACs that share an identical instant + event type.
  - Integrate notification boundaries into the existing single-thread wake-up model
    without adding new threads.
  - Keep timestamps in human-readable UTC (ISO-8601, `Z` suffix).
- Non-Goals:
  - No new transport: notifications reuse the T2 bus, not WRP or a new socket.
  - No change to firewall enforcement, schedule format, or metrics semantics.

## Decisions

- **Decision: Derive per-MAC windows on the fly from the existing event list rather
  than restructuring `schedule_t`.** For a given MAC index, walk the ordered weekly
  (and absolute) events; the MAC is "down" across `[event_i.time, event_j.time)` where
  `event_i` is the first event whose block list contains the index after an event that
  did not, and `event_j` is the next event whose block list omits it. This reuses the
  already-sorted SLL and avoids changing the on-disk/MsgPack format.
  - Alternatives considered: (a) Precompute and store explicit per-MAC window lists in
    `schedule_t` at `finalize_schedule()` time — faster lookups but larger memory
    footprint and more invasive to the data model and tests; deferred unless profiling
    shows the on-the-fly walk is too costly. (b) Emit notifications from a separate
    timer thread — rejected to preserve the single-worker-thread architecture.

- **Decision: Notification scheduling is edge-driven, computed each time the worker
  wakes.** On every wake the worker asks "what is the next notification instant at or
  after now?" across all four event types and all MACs, takes the minimum, and folds it
  into the existing `pthread_cond_timedwait` deadline alongside `get_next_unixtime()`
  and `next_report_time`. When the deadline is reached, it emits every notification
  whose instant equals "now" (within the wake granularity).
  - This mirrors the existing `get_next_unixtime()` pattern, so the concurrency model
    and tests stay consistent.

- **Decision: 15-minute lead points are first-class boundaries.** For each window edge
  `E`, the boundaries `E` (the `*_STARTED`/`*_ENDED` instant) and `E − 900` (the
  `*_STARTING_SOON`/`*_ENDING_SOON` instant) are both candidate wake times. Weekly
  `E − 900` is normalized modulo `SECONDS_IN_A_WEEK` so a lead that crosses Sunday
  midnight is handled (consistent with `finalize_schedule()`'s wrap sentinel).

- **Decision: Batch by (instant, eventType).** All MACs whose transition occurs at the
  same absolute instant and event type are collected into one payload's `affectedMacs`
  array; distinct instants produce distinct notifications. This matches the worked
  example (e.g., `c4` + `90` batched for `DOWNTIME_ENDING_SOON` at `t=114300`).

- **Decision: New `notify.c`/`notify.h` translation unit for payload + T2 emission,
  guarded by `ENABLE_FEATURE_TELEMETRY2_0`.** Keeps JSON assembly and the T2 call out
  of `scheduler.c`, consistent with the module-per-concern convention. When the flag is
  off, emission compiles to a no-op so non-T2 builds are unaffected.

- **Decision: Timestamps via `gmtime` + `strftime` (`%Y-%m-%dT%H:%M:%SZ`).** Produces
  the human-readable UTC format shown in the examples. `currentTime` is the CPE wall
  clock; `DownStartTime`/`DownEndTime` are the window edges converted from weekly time
  to the corresponding absolute UTC instant for the current occurrence.

- **Decision: Both weekly and absolute rules participate in the per-MAC lifecycle.**
  The window-derivation walk applies uniformly to weekly recurring and absolute
  one-time rules, so every MAC governed by either rule type receives the full set of
  notifications.

- **Decision: Treat the weekly schedule as a circular timeline.** A MAC blocked
  continuously across the Sunday-midnight wrap is handled by joining the end of the
  week to its start; window edges and their 900 s lead points are computed modulo
  `SECONDS_IN_A_WEEK`, consistent with the existing wrap sentinel from
  `finalize_schedule()`. A MAC blocked across every event with no gap has no window
  edge and therefore produces no notifications.

- **Decision: Persist "already sent" notification state across restarts.** The set of
  notification boundaries already emitted (keyed by absolute instant and event type)
  is persisted to disk alongside the schedule data, so a process restart inside the
  15-minute lead does not skip a `*_STARTING_SOON`/`*_ENDING_SOON` notification. On
  startup the worker reloads this state and suppresses re-emission of any boundary
  already sent, while still emitting any boundary whose instant has passed but was
  not yet sent.

- **Decision: Always emit all four event types, even when a downtime window is
  shorter than 15 minutes.** For very short windows the `*_STARTING_SOON` and/or
  `*_ENDING_SOON` lead point (edge − 900 s) may fall before the window actually
  begins; the notification is still emitted at its computed instant. The four event
  types are independent and none is suppressed based on window duration.

- **Decision: The T2 event marker is a standard `t2event`.** Each notification is sent
  as a single JSON string over the T2 bus via the existing `t2_event_s` mechanism
  (as used today for `Akermetrics_split`), so no new T2 consumer contract is required.

## Risks / Trade-offs

- **Duplicate or missed notifications around wake granularity / clock steps.** The
  worker wakes on a deadline; if the system clock jumps, an edge could be skipped or
  re-fired. → Compare the target instant against a small window and track the last
  emitted boundary per event type to suppress duplicates within a wake cycle.
- **Restart gaps.** A process restart between `E − 900` and `E` could skip the
  `*_SOON` notification. → Persist the set of already-emitted boundaries to disk and
  reload on startup; the worker re-emits any passed-but-unsent boundary and
  suppresses ones already sent.
- **Short windows (< 15 min).** The `*_SOON` lead point can precede the window start.
  → Accepted by design: all four event types are always emitted at their computed
  instants regardless of window duration.
- **Week-boundary and DST correctness.** Lead points crossing Sunday midnight, and DST
  shifts, can mis-place a boundary. → Reuse `convert_unix_time_to_weekly()` semantics
  and the existing wrap-sentinel approach; cover with targeted tests including a TZ
  with DST and a window straddling midnight Sunday.
- **Schedule replacement mid-window.** A new schedule arriving via WRP can change a
  MAC's membership. → Recompute boundaries from the now-current schedule on the next
  wake (the schedule swap already signals `cond_var`).

## Migration Plan

- Additive and feature-flagged; no data migration. Steps:
  1. Land `notify.c`/`notify.h` (no-op when `ENABLE_FEATURE_TELEMETRY2_0` is off).
  2. Add per-MAC window + next-notification-boundary helpers to `schedule.c`.
  3. Integrate boundary into the `scheduler.c` wake calculation and emit on deadline.
  4. Register new sources/tests in CMake; add unit tests using the worked example.
- Rollback: revert the change set; with the flag off there is no behavioral change to
  enforcement, persistence, or metrics.

## Resolved Questions

- **Persist "already sent" state across restarts?** Yes — persisted to disk and
  reloaded on startup so a `*_SOON` is not missed if the process restarts inside the
  15-minute lead.
- **Do absolute rules participate, or weekly only?** Both — absolute and weekly rules
  participate in the same per-MAC lifecycle.
- **T2 event marker / payload form?** A standard `t2event`: each notification is a
  single JSON string sent via `t2_event_s`, reusing the existing T2 mechanism.
- **MAC blocked across the week wrap?** Treat the weekly schedule as a circular
  timeline; window edges and leads are computed modulo `SECONDS_IN_A_WEEK`.
- **Window shorter than 15 minutes?** Always emit all four event types; the `*_SOON`
  trigger may occur before downtime actually begins for very short intervals.
