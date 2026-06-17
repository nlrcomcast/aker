## ADDED Requirements

### Requirement: Downtime Transition Diff Computation

The system SHALL compute per-MAC blocking-state transitions by comparing the blocked-MAC index set of each consecutive pair of `schedule_event_t` nodes (including the wrap-around pair of last → first for weekly schedules).

For each transition the system SHALL classify every affected MAC into exactly one of:
- **Newly blocked** — MAC index present in the next event's block list but absent from the current event's block list
- **Newly unblocked** — MAC index present in the current event's block list but absent from the next event's block list

The system SHALL produce a sorted notification event list from these diffs at schedule load time.

#### Scenario: Simple two-event weekly schedule

- **GIVEN** a weekly schedule with events `[{time:0, indexes:[0]}, {time:43200, indexes:[]}]` and MACs `["aa:bb:cc:dd:ee:ff"]`
- **WHEN** the notification list is built
- **THEN** four notification events are produced:
  - `fire_time=0, type=DOWNTIME_STARTED, mac_indexes=[0]`
  - `fire_time=42300, type=DOWNTIME_ENDING_SOON, mac_indexes=[0]`
  - `fire_time=43200, type=DOWNTIME_ENDED, mac_indexes=[0]`
  - `fire_time=603900, type=DOWNTIME_STARTING_SOON, mac_indexes=[0]` (wrap-around: 15 min before next week's time=0 → 604800−900=603900)

#### Scenario: Adjacent events with overlapping MAC sets

- **GIVEN** a weekly schedule with events `[{time:86400, indexes:[0,1,2]}, {time:86460, indexes:[0,1,2,3]}]` and 4 MACs
- **WHEN** the diff is computed between event 1 and event 2
- **THEN** only MAC index 3 is classified as "newly blocked"; MACs 0, 1, 2 are unchanged and produce no notification at time 86460

#### Scenario: Weekly wrap-around

- **GIVEN** the last weekly event has `indexes=[]` and the first weekly event has `indexes=[0,1,2]`
- **WHEN** the wrap-around diff is computed
- **THEN** MACs 0, 1, 2 are classified as "newly blocked" at the first event's time, and `DOWNTIME_STARTING_SOON` fires 900 seconds before that time (wrapping into the previous week if necessary)

---

### Requirement: Pre-Transition Notification (15 Minutes Before)

The system SHALL emit a T2 notification exactly 900 seconds (15 minutes) before each blocking-state transition for the affected MACs.

- `DOWNTIME_STARTING_SOON` — 900 s before MACs become blocked
- `DOWNTIME_ENDING_SOON` — 900 s before MACs become unblocked

The system SHALL NOT emit a pre-notification if the pre-notification time coincides with or falls after the transition time (e.g., two events less than 900 s apart where the earlier event already triggered).

#### Scenario: Normal 15-minute pre-notification

- **GIVEN** a schedule transition at weekly time 86400 that blocks MACs [0, 1, 2]
- **WHEN** the scheduler thread wakes at weekly time 85500
- **THEN** a `DOWNTIME_STARTING_SOON` T2 notification is sent with `affectedMacs` = the MAC addresses at indexes 0, 1, 2

#### Scenario: Events closer than 15 minutes apart

- **GIVEN** two transitions at times 86400 and 86460 (60 seconds apart) where MAC 3 is newly blocked at 86460
- **WHEN** the pre-notification time for the second transition is computed as 86460 − 900 = 85560
- **THEN** the `DOWNTIME_STARTING_SOON` for MAC 3 fires at 85560, which is before the first transition at 86400 — this is valid and both pre-notifications are sent independently

---

### Requirement: Transition Notification

The system SHALL emit a T2 notification at each blocking-state transition for the affected MACs.

- `DOWNTIME_STARTED` — when MACs become blocked
- `DOWNTIME_ENDED` — when MACs become unblocked

#### Scenario: MACs enter downtime

- **GIVEN** a schedule transition at weekly time 86400 with indexes [0, 1, 2] and the previous state has indexes []
- **WHEN** the scheduler processes the transition
- **THEN** a `DOWNTIME_STARTED` T2 notification is sent with `affectedMacs` = MACs at indexes 0, 1, 2

#### Scenario: MACs exit downtime

- **GIVEN** a schedule transition at weekly time 115200 with indexes [0, 2] and the previous state has indexes [1, 2, 3]
- **WHEN** the scheduler processes the transition
- **THEN** a `DOWNTIME_ENDED` T2 notification is sent with `affectedMacs` = MACs at indexes 1, 3 (the ones removed from the blocked set)

#### Scenario: Mixed transition — some MACs start, others end

- **GIVEN** a transition where the blocked set changes from [0, 1] to [1, 2]
- **WHEN** the scheduler processes the transition
- **THEN** two separate notifications are sent:
  - `DOWNTIME_ENDED` for MAC index 0
  - `DOWNTIME_STARTED` for MAC index 2

---

### Requirement: T2 Notification Payload Format

Each T2 notification SHALL be a JSON string sent via `t2_event_s` with the following structure:

```json
{
  "eventType": "<DOWNTIME_STARTING_SOON|DOWNTIME_STARTED|DOWNTIME_ENDING_SOON|DOWNTIME_ENDED>",
  "scheduledTime": "<ISO 8601 UTC timestamp of the transition>",
  "currentTime": "<ISO 8601 UTC timestamp of CPE time at send>",
  "affectedMacs": ["<mac1>", "<mac2>", ...]
}
```

- `scheduledTime` SHALL be the UTC time corresponding to the schedule transition that triggered this notification.
- `currentTime` SHALL be the CPE's current system time at the moment the notification is sent.
- `affectedMacs` SHALL contain only the MACs whose blocking state changes at this transition (not all blocked MACs).

#### Scenario: Payload for DOWNTIME_STARTING_SOON

- **GIVEN** a transition at weekly time 86400 blocking MACs ["d6:0b:68:4f:15:a0", "c4:84:66:29:0f:9d"]
- **WHEN** the pre-notification fires 900 s before the transition
- **THEN** the T2 payload is:
  ```json
  {
    "eventType": "DOWNTIME_STARTING_SOON",
    "scheduledTime": "<UTC time corresponding to weekly time 86400>",
    "currentTime": "<CPE system time>",
    "affectedMacs": ["d6:0b:68:4f:15:a0", "c4:84:66:29:0f:9d"]
  }
  ```

#### Scenario: Payload for DOWNTIME_ENDED

- **GIVEN** a transition at weekly time 115200 unblocking MACs ["c4:84:66:29:0f:9d", "90:84:66:29:0f:11"]
- **WHEN** the transition notification fires
- **THEN** the T2 payload is:
  ```json
  {
    "eventType": "DOWNTIME_ENDED",
    "scheduledTime": "<UTC time corresponding to weekly time 115200>",
    "currentTime": "<CPE system time>",
    "affectedMacs": ["c4:84:66:29:0f:9d", "90:84:66:29:0f:11"]
  }
  ```

---

### Requirement: Scheduler Wake-Up for Notifications

The scheduler thread SHALL include the next pending notification fire time in its `pthread_cond_timedwait` timeout calculation, alongside the existing next-event and next-report times.

The effective timeout SHALL be `min(next_schedule_event, next_notification, next_report)`.

#### Scenario: Notification time is earlier than next schedule event

- **GIVEN** the next schedule event is at unix time T and the next notification fires at T − 900
- **WHEN** the scheduler computes the `timedwait` deadline
- **THEN** the deadline is set to T − 900

#### Scenario: No pending notifications

- **GIVEN** no schedule is loaded or all notifications have been sent for this cycle
- **WHEN** the scheduler computes the `timedwait` deadline
- **THEN** the notification time is `INT_MAX` and does not affect the deadline

---

### Requirement: Notification List Lifecycle

The notification event list SHALL be rebuilt whenever a new schedule is installed via `process_schedule_data()`. The previous notification list SHALL be freed along with the previous `schedule_t`.

When a schedule is cleared (empty data), all pending notifications SHALL be cancelled (list freed, no cancellation notification sent).

#### Scenario: Schedule replacement

- **GIVEN** the scheduler has an active notification list for schedule A
- **WHEN** a new schedule B is installed via `process_schedule_data()`
- **THEN** the notification list for schedule A is destroyed and a new list is built from schedule B

#### Scenario: Schedule deletion

- **GIVEN** the scheduler has an active notification list
- **WHEN** `process_schedule_data()` is called with `len=0`
- **THEN** the notification list is freed and no further notifications are sent until a new schedule arrives

---

### Requirement: Build Flag Guard

All notification code SHALL be guarded by the `ENABLE_FEATURE_TELEMETRY2_0` preprocessor flag, consistent with the existing T2 metrics integration.

When the flag is not defined, notification computation and sending SHALL be compiled out with no runtime overhead.

#### Scenario: T2 disabled build

- **GIVEN** `ENABLE_FEATURE_TELEMETRY2_0` is not defined
- **WHEN** the project is compiled
- **THEN** no notification-related code is included in the binary and no T2 headers are required
