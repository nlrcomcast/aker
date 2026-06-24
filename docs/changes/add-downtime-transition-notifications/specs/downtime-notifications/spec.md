## ADDED Requirements

### Requirement: Downtime Lifecycle Notification Event Types

The system SHALL emit four distinct downtime lifecycle notifications for each MAC
address governed by the active schedule, identified by an `eventType` field:
`DOWNTIME_STARTING_SOON`, `DOWNTIME_STARTED`, `DOWNTIME_ENDING_SOON`, and
`DOWNTIME_ENDED`. The `*_STARTING_SOON` and `*_ENDING_SOON` notifications SHALL be
emitted exactly 15 minutes (900 seconds) before the corresponding downtime window
start and end, respectively. The `DOWNTIME_STARTED` and `DOWNTIME_ENDED`
notifications SHALL be emitted at the instant the window starts and ends,
respectively.

#### Scenario: Notification 15 minutes before a downtime window starts
- **WHEN** a MAC's downtime window is scheduled to start at absolute instant `S`
- **THEN** the system emits a `DOWNTIME_STARTING_SOON` notification for that MAC at
  instant `S − 900` seconds

#### Scenario: Notification at downtime window start
- **WHEN** a MAC's downtime window starts at absolute instant `S`
- **THEN** the system emits a `DOWNTIME_STARTED` notification for that MAC at
  instant `S`

#### Scenario: Notification 15 minutes before a downtime window ends
- **WHEN** a MAC's downtime window is scheduled to end at absolute instant `E`
- **THEN** the system emits a `DOWNTIME_ENDING_SOON` notification for that MAC at
  instant `E − 900` seconds

#### Scenario: Notification at downtime window end
- **WHEN** a MAC's downtime window ends at absolute instant `E`
- **THEN** the system emits a `DOWNTIME_ENDED` notification for that MAC at
  instant `E`

### Requirement: Notification Payload Schema

Each notification SHALL be a JSON object containing an `eventType` string, a
`currentTime` string holding the CPE's current time, and an `affectedMacs` array.
Each element of `affectedMacs` SHALL contain `devicemac` (the MAC address string),
`DownStartTime` (the start of that MAC's downtime window), and `DownEndTime` (the
end of that MAC's downtime window). All timestamp fields (`currentTime`,
`DownStartTime`, `DownEndTime`) SHALL be expressed in a human-readable UTC format
(ISO-8601 with a trailing `Z`, e.g. `2026-06-05T20:00:00Z`).

#### Scenario: Payload contains required fields
- **WHEN** the system emits any downtime lifecycle notification
- **THEN** the payload contains `eventType`, `currentTime`, and a non-empty
  `affectedMacs` array
- **AND** every `affectedMacs` element contains `devicemac`, `DownStartTime`, and
  `DownEndTime`

#### Scenario: Timestamps are human-readable UTC
- **WHEN** any timestamp field is written to a notification payload
- **THEN** the value is formatted as UTC ISO-8601 with a trailing `Z`

#### Scenario: currentTime reflects the CPE clock
- **WHEN** a notification is emitted
- **THEN** `currentTime` equals the CPE's current wall-clock time at emission,
  expressed in UTC

### Requirement: Batching Affected MACs Per Transition

When multiple MAC addresses share the same `eventType` and the same transition
instant, the system SHALL emit a single notification whose `affectedMacs` array
contains all such MAC addresses. When MAC addresses transition at different
instants, the system SHALL emit separate notifications, one per distinct instant.

#### Scenario: MACs sharing a transition instant are batched
- **WHEN** two or more MACs transition with the same `eventType` at the same instant
- **THEN** the system emits exactly one notification listing all of those MACs in
  `affectedMacs`

#### Scenario: MACs transitioning at different instants are not batched
- **WHEN** two MACs transition with the same `eventType` but at different instants
- **THEN** the system emits a separate notification for each instant

### Requirement: Telemetry 2.0 Delivery Gating

Downtime lifecycle notifications SHALL be delivered over the Telemetry 2.0 (T2) bus
as a single JSON string via a standard `t2event` (the existing `t2_event_s`
mechanism), and SHALL only be emitted when the `ENABLE_FEATURE_TELEMETRY2_0` feature
flag is enabled at compile time. When the flag is disabled, notification emission
SHALL compile to a no-op and SHALL NOT alter firewall enforcement, schedule
persistence, or existing metrics.

#### Scenario: Notifications emitted on T2-enabled builds
- **WHEN** the build is compiled with `ENABLE_FEATURE_TELEMETRY2_0` enabled
- **AND** a downtime transition occurs
- **THEN** the corresponding notification is sent as a single JSON string over the
  T2 bus via a `t2event`

#### Scenario: No-op on non-T2 builds
- **WHEN** the build is compiled with `ENABLE_FEATURE_TELEMETRY2_0` disabled
- **AND** a downtime transition occurs
- **THEN** no notification is emitted and no other behavior changes

### Requirement: All Event Types Emitted For Short Windows

The system SHALL always emit all four notification types for every downtime window
regardless of its duration. When a downtime window is shorter than 15 minutes, the
`DOWNTIME_STARTING_SOON` and/or `DOWNTIME_ENDING_SOON` trigger instant (window edge
minus 900 seconds) MAY fall before the window actually begins; the notification
SHALL still be emitted at its computed instant and SHALL NOT be suppressed.

#### Scenario: Window shorter than 15 minutes still emits all four notifications
- **WHEN** a MAC's downtime window is shorter than 15 minutes
- **THEN** the system emits `DOWNTIME_STARTING_SOON`, `DOWNTIME_STARTED`,
  `DOWNTIME_ENDING_SOON`, and `DOWNTIME_ENDED` for that MAC

#### Scenario: Lead instant before window start is not suppressed
- **WHEN** a window edge minus 900 seconds falls before the window's start instant
- **THEN** the corresponding `*_SOON` notification is still emitted at that computed
  instant

### Requirement: Persist Already-Sent Notification State Across Restarts

The system SHALL persist the set of notification boundaries it has already emitted,
keyed by absolute instant and event type, to disk. On startup the system SHALL
reload this state and SHALL suppress re-emission of any boundary already sent, while
still emitting any boundary whose instant has passed but was not yet sent. This
ensures a `DOWNTIME_STARTING_SOON` or `DOWNTIME_ENDING_SOON` is not missed when the
process restarts inside the 15-minute lead.

#### Scenario: Restart inside the 15-minute lead does not skip a *_SOON
- **WHEN** the process restarts after a `*_SOON` trigger instant has passed but the
  notification was not yet emitted
- **THEN** the system emits that `*_SOON` notification on startup

#### Scenario: Already-sent notification is not re-emitted after restart
- **WHEN** the process restarts after a notification has already been emitted
- **THEN** the system does not re-emit that notification
