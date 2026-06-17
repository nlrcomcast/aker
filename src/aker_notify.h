/**
 * Copyright 2017 Comcast Cable Communications Management, LLC
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
#ifndef __AKER_NOTIFY_H__
#define __AKER_NOTIFY_H__

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "schedule.h"

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

/**
 *  Build a sorted notification event list from the given schedule's weekly
 *  event list by diffing consecutive blocked-MAC index sets.
 *
 *  @param s  the schedule to build notifications from
 *
 *  @return the head of the sorted notification list, or NULL if none
 */
notification_event_t* build_notification_list(schedule_t *s);

/**
 *  Destroy (free) a notification event list.
 *
 *  @param list  the head of the list to destroy
 */
void destroy_notification_list(notification_event_t *list);

/**
 *  Get the next notification fire time (as weekly seconds) at or after
 *  the given weekly time.
 *
 *  @param list          the notification list
 *  @param weekly_time   current weekly time in seconds since Sunday
 *
 *  @return the next fire time, or INT_MAX if none pending
 */
time_t get_next_notification_time(notification_event_t *list, time_t weekly_time);

/**
 *  Fire all due notifications whose fire_time <= current weekly time.
 *  Sends T2 events with JSON payloads for each due notification.
 *
 *  @param list          the notification list
 *  @param s             the schedule (for MAC address lookup)
 *  @param weekly_time   current weekly time
 *  @param unix_time     current unix time (for currentTime field)
 *
 *  @return pointer to the first un-fired notification (new list head for tracking)
 */
notification_event_t* fire_due_notifications(notification_event_t *list,
                                             schedule_t *s,
                                             time_t weekly_time,
                                             time_t unix_time);

/**
 *  Format a notification as a JSON string payload.
 *
 *  @param type           the notification event type
 *  @param scheduled_time the scheduled transition unix time
 *  @param current_time   the current CPE system time
 *  @param s              the schedule (for MAC lookup)
 *  @param mac_indexes    array of MAC indexes
 *  @param mac_count      number of MACs
 *
 *  @return malloc'd JSON string, or NULL on failure. Caller must free.
 */
char* format_notification_json(notify_event_type_t type,
                               time_t scheduled_time,
                               time_t current_time,
                               schedule_t *s,
                               uint32_t *mac_indexes,
                               size_t mac_count);

#endif
