/**
 * Copyright 2026 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef __NOTIFY_H__
#define __NOTIFY_H__

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "schedule.h"

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

/**
 *  Initializes the downtime notification subsystem and reloads the set of
 *  already-emitted notification boundaries from @p persist_file, if it exists.
 *
 *  @param persist_file path to the file used to persist already-sent
 *                      boundaries (may be NULL to disable persistence)
 */
void notify_init( const char *persist_file );


/**
 *  Releases all resources held by the notification subsystem.
 */
void notify_destroy( void );


/**
 *  Returns the human-readable name of a downtime event type.
 *
 *  @param event the event type
 *
 *  @return a static string, never NULL
 */
const char* notify_event_name( downtime_event_t event );


/**
 *  Builds the JSON payload for a single batched notification.
 *
 *  The payload contains @c eventType, @c currentTime (UTC), and a non-empty
 *  @c affectedMacs array whose elements carry @c devicemac, @c DownStartTime,
 *  and @c DownEndTime (all timestamps UTC ISO-8601).
 *
 *  @param s         the schedule, used to resolve MAC index -> address string
 *  @param event     the event type for this batch
 *  @param current   the CPE current time to stamp into @c currentTime
 *  @param windows   the affected MACs and their resolved window edges
 *  @param count     the number of entries in @p windows (must be > 0)
 *
 *  @return a newly allocated JSON string (caller frees with aker_free), or NULL
 */
char* notify_build_payload( schedule_t *s, downtime_event_t event,
                            time_t current, const downtime_window_t *windows,
                            size_t count );


/**
 *  Evaluates all four event types at @p instant, batches the affected MACs, and
 *  emits a notification for each non-empty batch over the Telemetry 2.0 bus.
 *
 *  Boundaries already recorded as sent are suppressed; newly emitted boundaries
 *  are recorded and persisted.  When @c ENABLE_FEATURE_TELEMETRY2_0 is not
 *  defined the T2 send compiles to a no-op, but bookkeeping still occurs.
 *
 *  @param s        the active schedule
 *  @param instant  the absolute instant whose transitions should be emitted
 *  @param current  the CPE current time to stamp into the payloads
 *
 *  @return the number of notifications emitted
 */
size_t notify_emit( schedule_t *s, time_t instant, time_t current );


/**
 *  Reports whether a given boundary (instant + event type) has already been
 *  emitted.  Exposed for testing the restart-persistence behavior.
 *
 *  @param instant the boundary instant
 *  @param event   the boundary event type
 *
 *  @return true if already sent, false otherwise
 */
bool notify_already_sent( time_t instant, downtime_event_t event );


#endif
