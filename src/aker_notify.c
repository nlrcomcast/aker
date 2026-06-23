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
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>

#if defined(ENABLE_FEATURE_TELEMETRY2_0)
#include <telemetry_busmessage_sender.h>
#endif

#include "aker_notify.h"
#include "aker_log.h"
#include "aker_mem.h"
#include "time.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
#define PRE_NOTIFICATION_LEAD_S  900  /* 15 minutes */

/*----------------------------------------------------------------------------*/
/*                             Internal Functions                              */
/*----------------------------------------------------------------------------*/

static const char* notify_event_type_str(notify_event_type_t type)
{
    switch (type) {
        case NOTIFY_DOWNTIME_STARTING_SOON: return "DOWNTIME_STARTING_SOON";
        case NOTIFY_DOWNTIME_STARTED:       return "DOWNTIME_STARTED";
        case NOTIFY_DOWNTIME_ENDING_SOON:   return "DOWNTIME_ENDING_SOON";
        case NOTIFY_DOWNTIME_ENDED:         return "DOWNTIME_ENDED";
        default:                            return "UNKNOWN";
    }
}

/**
 *  Insert a notification event in sorted order by fire_time into the list.
 */
static void insert_notification_sorted(notification_event_t **head,
                                       notification_event_t *node)
{
    notification_event_t **pp = head;
    while (*pp && (*pp)->fire_time <= node->fire_time) {
        pp = &((*pp)->next);
    }
    node->next = *pp;
    *pp = node;
}

/**
 *  Create a notification_event_t with the given parameters.
 */
static notification_event_t* create_notification(time_t fire_time,
                                                 time_t scheduled_time,
                                                 notify_event_type_t type,
                                                 uint32_t *mac_indexes,
                                                 size_t mac_count)
{
    notification_event_t *n;
    size_t size = sizeof(notification_event_t) + mac_count * sizeof(uint32_t);

    n = (notification_event_t*) aker_malloc(size);
    if (NULL == n) {
        return NULL;
    }
    n->fire_time = fire_time;
    n->scheduled_time = scheduled_time;
    n->type = type;
    n->mac_count = mac_count;
    n->next = NULL;
    memcpy(n->mac_indexes, mac_indexes, mac_count * sizeof(uint32_t));
    return n;
}

/**
 *  Check if a value exists in an array of uint32_t.
 */
static bool index_in_set(uint32_t *set, size_t count, uint32_t val)
{
    for (size_t i = 0; i < count; i++) {
        if (set[i] == val) {
            return true;
        }
    }
    return false;
}

/**
 *  Compute diffs between two schedule events and add notification events.
 *  newly_blocked = in next but not in current -> STARTED / STARTING_SOON
 *  newly_unblocked = in current but not in next -> ENDED / ENDING_SOON
 */
static void diff_events_and_notify(notification_event_t **list,
                                   schedule_event_t *current,
                                   schedule_event_t *next)
{
    uint32_t newly_blocked[256];
    uint32_t newly_unblocked[256];
    size_t nb_count = 0;
    size_t nu_count = 0;
    time_t pre_time;
    notification_event_t *n;

    /* Find newly blocked: in next->block but not in current->block */
    for (size_t i = 0; i < next->block_count; i++) {
        if (!index_in_set(current->block, current->block_count, next->block[i])) {
            if (nb_count < 256) {
                newly_blocked[nb_count++] = next->block[i];
            }
        }
    }

    /* Find newly unblocked: in current->block but not in next->block */
    for (size_t i = 0; i < current->block_count; i++) {
        if (!index_in_set(next->block, next->block_count, current->block[i])) {
            if (nu_count < 256) {
                newly_unblocked[nu_count++] = current->block[i];
            }
        }
    }

    /* Emit DOWNTIME_STARTED and DOWNTIME_STARTING_SOON for newly blocked */
    if (nb_count > 0) {
        n = create_notification(next->time, next->time,
                                NOTIFY_DOWNTIME_STARTED,
                                newly_blocked, nb_count);
        if (n) {
            insert_notification_sorted(list, n);
        }

        pre_time = next->time - PRE_NOTIFICATION_LEAD_S;
        if (pre_time < 0) {
            pre_time += SECONDS_IN_A_WEEK;
        }
        n = create_notification(pre_time, next->time,
                                NOTIFY_DOWNTIME_STARTING_SOON,
                                newly_blocked, nb_count);
        if (n) {
            insert_notification_sorted(list, n);
        }
    }

    /* Emit DOWNTIME_ENDED and DOWNTIME_ENDING_SOON for newly unblocked */
    if (nu_count > 0) {
        n = create_notification(next->time, next->time,
                                NOTIFY_DOWNTIME_ENDED,
                                newly_unblocked, nu_count);
        if (n) {
            insert_notification_sorted(list, n);
        }

        pre_time = next->time - PRE_NOTIFICATION_LEAD_S;
        if (pre_time < 0) {
            pre_time += SECONDS_IN_A_WEEK;
        }
        n = create_notification(pre_time, next->time,
                                NOTIFY_DOWNTIME_ENDING_SOON,
                                newly_unblocked, nu_count);
        if (n) {
            insert_notification_sorted(list, n);
        }
    }
}

/*----------------------------------------------------------------------------*/
/*                             External Functions                              */
/*----------------------------------------------------------------------------*/

/* See aker_notify.h for details. */
notification_event_t* build_notification_list(schedule_t *s)
{
    notification_event_t *list = NULL;
    schedule_event_t *event;
    schedule_event_t *first;

    if (NULL == s || NULL == s->weekly) {
        return NULL;
    }

    first = s->weekly;
    event = first;

    /* Walk consecutive pairs */
    while (event && event->next) {
        diff_events_and_notify(&list, event, event->next);
        event = event->next;
    }

    /* Wrap-around: last event -> first event of next week */
    if (event && event != first) {
        diff_events_and_notify(&list, event, first);
    } else if (event == first) {
        /* Single event schedule — no transitions, no notifications */
    }

    return list;
}

/* See aker_notify.h for details. */
void destroy_notification_list(notification_event_t *list)
{
    notification_event_t *tmp;
    while (list) {
        tmp = list;
        list = list->next;
        aker_free(tmp);
    }
}

/* See aker_notify.h for details. */
time_t get_next_notification_time(notification_event_t *list, time_t weekly_time)
{
    while (list) {
        if (list->fire_time >= weekly_time) {
            return list->fire_time;
        }
        list = list->next;
    }
    return INT_MAX;
}

/* See aker_notify.h for details. */
char* format_notification_json(notify_event_type_t type,
                               time_t scheduled_time,
                               time_t current_time,
                               schedule_t *s,
                               uint32_t *mac_indexes,
                               size_t mac_count)
{
    char *buf;
    char scheduled_str[32];
    char current_str[32];
    struct tm tm_buf;
    size_t offset;
    size_t buf_size;

    /* Format timestamps as ISO 8601 UTC */
    gmtime_r(&scheduled_time, &tm_buf);
    strftime(scheduled_str, sizeof(scheduled_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    gmtime_r(&current_time, &tm_buf);
    strftime(current_str, sizeof(current_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    /* Estimate buffer size: fixed overhead + MACs */
    buf_size = 256 + (mac_count * (MAC_ADDRESS_SIZE + 4));
    buf = (char*) aker_malloc(buf_size);
    if (NULL == buf) {
        return NULL;
    }

    offset = (size_t) snprintf(buf, buf_size,
        "{\"eventType\":\"%s\","
        "\"scheduledTime\":\"%s\","
        "\"currentTime\":\"%s\","
        "\"affectedMacs\":[",
        notify_event_type_str(type),
        scheduled_str,
        current_str);

    for (size_t i = 0; i < mac_count; i++) {
        if (i > 0) {
            offset += (size_t) snprintf(buf + offset, buf_size - offset, ",");
        }
        if (s && mac_indexes[i] < s->mac_count) {
            offset += (size_t) snprintf(buf + offset, buf_size - offset,
                "\"%s\"", s->macs[mac_indexes[i]].mac);
        }
    }

    snprintf(buf + offset, buf_size - offset, "]}");
    return buf;
}

/* See aker_notify.h for details. */
notification_event_t* fire_due_notifications(notification_event_t *list,
                                             schedule_t *s,
                                             time_t weekly_time,
                                             time_t unix_time)
{
    while (list && list->fire_time <= weekly_time) {
        /* Convert weekly scheduled_time to absolute Unix time */
        time_t scheduled_unix_time;
        time_t delta = list->scheduled_time - weekly_time;
        if (delta < -(SECONDS_IN_A_WEEK / 2)) {
            delta += SECONDS_IN_A_WEEK;
        }
        scheduled_unix_time = unix_time + delta;

        char *json = format_notification_json(list->type,
                                              scheduled_unix_time,
                                              unix_time,
                                              s,
                                              list->mac_indexes,
                                              list->mac_count);
        if (json) {
#if defined(ENABLE_FEATURE_TELEMETRY2_0)
            t2_event_s("AkerDowntimeNotification", json);
#endif
            debug_info("AkerDowntimeNotification: %s\n", json);
            aker_free(json);
        }
        list = list->next;
    }
    return list;
}
