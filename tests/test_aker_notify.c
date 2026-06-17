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
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <CUnit/Basic.h>

#include "../src/aker_notify.h"
#include "../src/aker_notify.c"

/* Stubs for aker_mem */
void *aker_malloc(size_t size) { return malloc(size); }
void aker_free(void *ptr) { free(ptr); }

/*----------------------------------------------------------------------------*/
/*                              Helper Functions                               */
/*----------------------------------------------------------------------------*/

static schedule_t* make_test_schedule(size_t mac_count, const char *macs[],
                                      size_t event_count,
                                      time_t times[],
                                      size_t block_counts[],
                                      uint32_t *blocks[])
{
    schedule_t *s = (schedule_t*) calloc(1, sizeof(schedule_t));
    s->mac_count = mac_count;
    s->macs = (mac_address*) calloc(mac_count, sizeof(mac_address));
    for (size_t i = 0; i < mac_count; i++) {
        strncpy(s->macs[i].mac, macs[i], MAC_ADDRESS_SIZE - 1);
    }

    schedule_event_t *prev = NULL;
    for (size_t i = 0; i < event_count; i++) {
        size_t sz = sizeof(schedule_event_t) + block_counts[i] * sizeof(uint32_t);
        schedule_event_t *e = (schedule_event_t*) calloc(1, sz);
        e->time = times[i];
        e->block_count = block_counts[i];
        for (size_t j = 0; j < block_counts[i]; j++) {
            e->block[j] = blocks[i][j];
        }
        e->next = NULL;
        if (NULL == prev) {
            s->weekly = e;
        } else {
            prev->next = e;
        }
        prev = e;
    }
    return s;
}

static void free_test_schedule(schedule_t *s)
{
    if (!s) return;
    schedule_event_t *e = s->weekly;
    while (e) {
        schedule_event_t *tmp = e;
        e = e->next;
        free(tmp);
    }
    free(s->macs);
    free(s);
}

static int count_notifications(notification_event_t *list)
{
    int count = 0;
    while (list) {
        count++;
        list = list->next;
    }
    return count;
}

/*----------------------------------------------------------------------------*/
/*                                Test Cases                                   */
/*----------------------------------------------------------------------------*/

void test_build_notification_list_simple(void)
{
    /* Two-event schedule: {time:0, blocks:[0]}, {time:43200, blocks:[]} */
    const char *macs[] = {"aa:bb:cc:dd:ee:ff"};
    time_t times[] = {0, 43200};
    size_t block_counts[] = {1, 0};
    uint32_t b0[] = {0};
    uint32_t *blocks[] = {b0, NULL};

    schedule_t *s = make_test_schedule(1, macs, 2, times, block_counts, blocks);
    notification_event_t *list = build_notification_list(s);

    CU_ASSERT_PTR_NOT_NULL(list);
    CU_ASSERT_EQUAL(count_notifications(list), 4);

    /* Verify events are sorted by fire_time */
    notification_event_t *n = list;

    /* First: DOWNTIME_STARTED at time=0 */
    CU_ASSERT_EQUAL(n->fire_time, 0);
    CU_ASSERT_EQUAL(n->type, NOTIFY_DOWNTIME_STARTED);
    CU_ASSERT_EQUAL(n->mac_count, 1);
    CU_ASSERT_EQUAL(n->mac_indexes[0], 0);
    n = n->next;

    /* Second: DOWNTIME_ENDING_SOON at 43200-900=42300 */
    CU_ASSERT_EQUAL(n->fire_time, 42300);
    CU_ASSERT_EQUAL(n->type, NOTIFY_DOWNTIME_ENDING_SOON);
    n = n->next;

    /* Third: DOWNTIME_ENDED at 43200 */
    CU_ASSERT_EQUAL(n->fire_time, 43200);
    CU_ASSERT_EQUAL(n->type, NOTIFY_DOWNTIME_ENDED);
    n = n->next;

    /* Fourth: DOWNTIME_STARTING_SOON at 604800-900=603900 (wrap-around) */
    CU_ASSERT_EQUAL(n->fire_time, 603900);
    CU_ASSERT_EQUAL(n->type, NOTIFY_DOWNTIME_STARTING_SOON);
    CU_ASSERT_PTR_NULL(n->next);

    destroy_notification_list(list);
    free_test_schedule(s);
}

void test_build_notification_list_empty_schedule(void)
{
    notification_event_t *list;

    /* NULL schedule */
    list = build_notification_list(NULL);
    CU_ASSERT_PTR_NULL(list);

    /* Schedule with no weekly events */
    schedule_t s;
    memset(&s, 0, sizeof(s));
    s.weekly = NULL;
    list = build_notification_list(&s);
    CU_ASSERT_PTR_NULL(list);
}

void test_build_notification_list_single_event(void)
{
    /* Single-event schedule: no transitions -> no notifications */
    const char *macs[] = {"aa:bb:cc:dd:ee:ff"};
    time_t times[] = {0};
    size_t block_counts[] = {1};
    uint32_t b0[] = {0};
    uint32_t *blocks[] = {b0};

    schedule_t *s = make_test_schedule(1, macs, 1, times, block_counts, blocks);
    notification_event_t *list = build_notification_list(s);

    /* Single event = no transitions, no notifications */
    CU_ASSERT_PTR_NULL(list);

    destroy_notification_list(list);
    free_test_schedule(s);
}

void test_build_notification_list_overlapping_macs(void)
{
    /* {time:86400, blocks:[0,1,2]}, {time:86460, blocks:[0,1,2,3]} */
    const char *macs[] = {"aa:11:22:33:44:55", "bb:11:22:33:44:55",
                          "cc:11:22:33:44:55", "dd:11:22:33:44:55"};
    time_t times[] = {86400, 86460};
    size_t block_counts[] = {3, 4};
    uint32_t b0[] = {0, 1, 2};
    uint32_t b1[] = {0, 1, 2, 3};
    uint32_t *blocks[] = {b0, b1};

    schedule_t *s = make_test_schedule(4, macs, 2, times, block_counts, blocks);
    notification_event_t *list = build_notification_list(s);

    CU_ASSERT_PTR_NOT_NULL(list);

    /* Should have notifications only for MAC index 3 (newly blocked at 86460)
     * plus wrap-around: MAC 3 unblocked going from event2 back to event1
     * Total: STARTED(3) + STARTING_SOON(3) + ENDED(3) + ENDING_SOON(3) = 4 */
    int total = count_notifications(list);
    CU_ASSERT_EQUAL(total, 4);

    /* Find the STARTED notification at time 86460 */
    notification_event_t *n = list;
    int found_started = 0;
    while (n) {
        if (n->type == NOTIFY_DOWNTIME_STARTED && n->fire_time == 86460) {
            CU_ASSERT_EQUAL(n->mac_count, 1);
            CU_ASSERT_EQUAL(n->mac_indexes[0], 3);
            found_started = 1;
        }
        n = n->next;
    }
    CU_ASSERT_TRUE(found_started);

    destroy_notification_list(list);
    free_test_schedule(s);
}

void test_format_notification_json(void)
{
    const char *macs[] = {"d6:0b:68:4f:15:a0", "c4:84:66:29:0f:9d"};
    time_t times[] = {0};
    size_t block_counts[] = {0};
    uint32_t *blocks[] = {NULL};

    schedule_t *s = make_test_schedule(2, macs, 1, times, block_counts, blocks);

    uint32_t indexes[] = {0, 1};
    time_t scheduled = 1000000;
    time_t current = 1000001;

    char *json = format_notification_json(NOTIFY_DOWNTIME_STARTING_SOON,
                                          scheduled, current, s,
                                          indexes, 2);
    CU_ASSERT_PTR_NOT_NULL(json);
    if (json) {
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"eventType\":\"DOWNTIME_STARTING_SOON\""));
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"affectedMacs\":["));
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"d6:0b:68:4f:15:a0\""));
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"c4:84:66:29:0f:9d\""));
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"scheduledTime\":"));
        CU_ASSERT_PTR_NOT_NULL(strstr(json, "\"currentTime\":"));
        free(json);
    }

    free_test_schedule(s);
}

void test_get_next_notification_time(void)
{
    const char *macs[] = {"aa:bb:cc:dd:ee:ff"};
    time_t times[] = {0, 43200};
    size_t block_counts[] = {1, 0};
    uint32_t b0[] = {0};
    uint32_t *blocks[] = {b0, NULL};

    schedule_t *s = make_test_schedule(1, macs, 2, times, block_counts, blocks);
    notification_event_t *list = build_notification_list(s);

    /* First notification is at time 0 */
    time_t next = get_next_notification_time(list, 0);
    CU_ASSERT_EQUAL(next, 0);

    /* After time 0, next should be 42300 */
    next = get_next_notification_time(list, 1);
    CU_ASSERT_EQUAL(next, 42300);

    /* After all notifications, should return INT_MAX */
    next = get_next_notification_time(list, 700000);
    CU_ASSERT_EQUAL(next, INT_MAX);

    destroy_notification_list(list);
    free_test_schedule(s);
}

void test_events_closer_than_900s(void)
{
    /* Two events 60s apart: {86400, [0,1,2]}, {86460, [0,1,2,3]} */
    const char *macs[] = {"aa:11:22:33:44:55", "bb:11:22:33:44:55",
                          "cc:11:22:33:44:55", "dd:11:22:33:44:55"};
    time_t times[] = {86400, 86460};
    size_t block_counts[] = {3, 4};
    uint32_t b0[] = {0, 1, 2};
    uint32_t b1[] = {0, 1, 2, 3};
    uint32_t *blocks[] = {b0, b1};

    schedule_t *s = make_test_schedule(4, macs, 2, times, block_counts, blocks);
    notification_event_t *list = build_notification_list(s);

    /* Pre-notification for MAC 3 newly blocked at 86460 fires at 86460-900=85560 */
    notification_event_t *n = list;
    int found_pre = 0;
    while (n) {
        if (n->type == NOTIFY_DOWNTIME_STARTING_SOON &&
            n->fire_time == 85560 &&
            n->mac_count == 1 &&
            n->mac_indexes[0] == 3) {
            found_pre = 1;
        }
        n = n->next;
    }
    CU_ASSERT_TRUE(found_pre);

    destroy_notification_list(list);
    free_test_schedule(s);
}

/*----------------------------------------------------------------------------*/
/*                                  Main                                       */
/*----------------------------------------------------------------------------*/

int main(void)
{
    CU_pSuite suite = NULL;

    if (CUE_SUCCESS != CU_initialize_registry()) {
        return CU_get_error();
    }

    suite = CU_add_suite("aker_notify tests", NULL, NULL);
    if (NULL == suite) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    CU_add_test(suite, "build_notification_list simple two-event",
                test_build_notification_list_simple);
    CU_add_test(suite, "build_notification_list empty schedule",
                test_build_notification_list_empty_schedule);
    CU_add_test(suite, "build_notification_list single event",
                test_build_notification_list_single_event);
    CU_add_test(suite, "build_notification_list overlapping MACs",
                test_build_notification_list_overlapping_macs);
    CU_add_test(suite, "format_notification_json",
                test_format_notification_json);
    CU_add_test(suite, "get_next_notification_time",
                test_get_next_notification_time);
    CU_add_test(suite, "events closer than 900s",
                test_events_closer_than_900s);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    unsigned int failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return (failures > 0) ? 1 : 0;
}
