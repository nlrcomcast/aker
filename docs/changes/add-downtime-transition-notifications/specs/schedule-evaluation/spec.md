## ADDED Requirements

### Requirement: Per-MAC Downtime Window Computation

The system SHALL derive, for each MAC address governed by the active schedule, its
downtime windows defined as the maximal consecutive spans during which that MAC's
index appears in successive schedule events' block lists. A window SHALL start at
the event time where the MAC's index first appears after an event in which it was
absent (or after no prior blocking), and SHALL end at the next event time where the
MAC's index is absent. The computation SHALL apply to both weekly recurring rules
and absolute one-time rules. For weekly rules the schedule SHALL be treated as a
circular timeline: a MAC blocked continuously across the Sunday-midnight wrap forms
a single window joining the end of the week to its start, with edges and lead points
computed modulo `SECONDS_IN_A_WEEK`. A MAC blocked across every event with no gap
has no window edge and SHALL produce no notifications.

#### Scenario: MAC blocked across consecutive events forms one window
- **WHEN** a MAC's index appears in the block lists of several consecutive events
  and is then absent at a later event
- **THEN** the system computes a single downtime window spanning from the first
  blocking event time to the first subsequent non-blocking event time

#### Scenario: A single event is a start for one MAC and an end for another
- **WHEN** at one event time a MAC's index newly appears while another MAC's index
  newly disappears
- **THEN** the system treats that instant as a window start for the first MAC and a
  window end for the second MAC

#### Scenario: MAC with multiple separated windows
- **WHEN** a MAC's index appears, disappears, and later reappears across the
  schedule events
- **THEN** the system computes two distinct downtime windows for that MAC

#### Scenario: Window spanning the weekly wrap is treated circularly
- **WHEN** a MAC remains blocked from before Sunday midnight to after it
- **THEN** the system computes a single downtime window across the wrap with edges
  and lead points normalized modulo `SECONDS_IN_A_WEEK`

#### Scenario: Absolute rule MAC participates
- **WHEN** a MAC's downtime is governed by an absolute one-time rule
- **THEN** the system computes that MAC's downtime window from the absolute rule the
  same way it does for weekly rules

### Requirement: Next Notification Boundary Wake-Up

The scheduler worker thread SHALL compute the next notification boundary at or after
the current time, where boundaries include each downtime window start, each window
end, and each window edge minus 900 seconds. The worker SHALL fold this boundary
into its wait deadline together with the next schedule event time and the next
metrics report time, selecting the earliest, so it wakes in time to emit each
notification. Window-edge lead points that fall before the start of the week SHALL
be normalized modulo `SECONDS_IN_A_WEEK`.

#### Scenario: Worker wakes at the earliest pending boundary
- **WHEN** the next notification boundary is earlier than both the next schedule
  event time and the next metrics report time
- **THEN** the worker's wait deadline is set to that notification boundary

#### Scenario: Lead point crossing the week boundary is normalized
- **WHEN** a window edge minus 900 seconds falls before the start of the current week
- **THEN** the boundary is normalized modulo `SECONDS_IN_A_WEEK` so it maps to the
  correct instant within the week

#### Scenario: Notifications emitted when the boundary is reached
- **WHEN** the worker wakes at a notification boundary
- **THEN** it emits every notification whose transition instant equals the current
  instant and recomputes the next boundary before waiting again
