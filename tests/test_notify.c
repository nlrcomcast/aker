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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include <CUnit/Basic.h>

#include "../src/schedule.h"
#include "../src/time.h"
#include "../src/notify.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* Sunday, 2017-11-12 00:00:00 UTC -> weekly time 0. */
#define SUNDAY_MIDNIGHT_UTC   1510444800L

#define LEAD                  NOTIFY_LEAD_SECONDS

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
typedef struct {
    time_t   time;
    uint8_t  block_count;
    uint32_t block[5];
} ev_t;

/*----------------------------------------------------------------------------*/
/*                             Internal helpers                              */
/*----------------------------------------------------------------------------*/
static schedule_t *build_schedule( const char **macs, size_t mac_count,
                                   const ev_t *weekly, size_t weekly_count,
                                   const ev_t *absolute, size_t absolute_count )
{
    schedule_t *s;
    size_t i, j;

    set_unix_time_zone( "UTC" );

    s = create_schedule();
    CU_ASSERT_FATAL( NULL != s );
    s->report_rate_s = 3600;

    CU_ASSERT_FATAL( 0 == create_mac_table( s, mac_count ) );
    for( i = 0; i < mac_count; i++ ) {
        CU_ASSERT( 0 == set_mac_index( s, macs[i], 17, (uint32_t) i ) );
    }

    for( i = 0; i < weekly_count; i++ ) {
        schedule_event_t *e = create_schedule_event( weekly[i].block_count );
        CU_ASSERT_FATAL( NULL != e );
        e->time = weekly[i].time;
        for( j = 0; j < weekly[i].block_count; j++ ) {
            e->block[j] = weekly[i].block[j];
        }
        insert_event( &s->weekly, e );
    }

    for( i = 0; i < absolute_count; i++ ) {
        schedule_event_t *e = create_schedule_event( absolute[i].block_count );
        CU_ASSERT_FATAL( NULL != e );
        e->time = absolute[i].time;
        for( j = 0; j < absolute[i].block_count; j++ ) {
            e->block[j] = absolute[i].block[j];
        }
        insert_event( &s->absolute, e );
    }

    finalize_schedule( s );
    return s;
}

/*----------------------------------------------------------------------------*/
/*                                   Tests                                    */
/*----------------------------------------------------------------------------*/

/*
 * Worked example: 4 managed MACs (d6, c4, 99, 90) plus a spare, with a clean
 * weekly schedule that produces non-overlapping, easily verifiable windows.
 *
 *   t=3600  block {0,1}    -> d6, c4 start
 *   t=7200  block {1}      -> d6 ends, c4 continues
 *   t=10800 block {}       -> c4 ends
 *   t=14400 block {2,3}    -> 99, 90 start
 *   t=18000 block {3}      -> 99 ends, 90 continues
 *   t=21600 block {}       -> 90 ends
 *
 * Expected weekly windows:
 *   d6 (0): [3600,  7200)
 *   c4 (1): [3600, 10800)
 *   99 (2): [14400, 18000)
 *   90 (3): [14400, 21600)
 */
static const char *g_macs[] = {
    "d6:d6:d6:d6:d6:d6",
    "c4:c4:c4:c4:c4:c4",
    "99:99:99:99:99:99",
    "90:90:90:90:90:90",
    "00:00:00:00:00:00",
};

static const ev_t g_weekly[] = {
    { .time = 3600,  .block_count = 2, .block = { 0, 1 } },
    { .time = 7200,  .block_count = 1, .block = { 1 } },
    { .time = 10800, .block_count = 0, .block = { 0 } },
    { .time = 14400, .block_count = 2, .block = { 2, 3 } },
    { .time = 18000, .block_count = 1, .block = { 3 } },
    { .time = 21600, .block_count = 0, .block = { 0 } },
};

static void check_window( schedule_t *s, uint32_t idx, time_t ref,
                          time_t exp_start_weekly, time_t exp_end_weekly )
{
    downtime_window_t win;
    bool ok = get_mac_downtime_window( s, idx, ref, &win );
    CU_ASSERT_TRUE( ok );
    if( ok ) {
        CU_ASSERT_EQUAL( win.start, SUNDAY_MIDNIGHT_UTC + exp_start_weekly );
        CU_ASSERT_EQUAL( win.end,   SUNDAY_MIDNIGHT_UTC + exp_end_weekly );
    }
}

void test_per_mac_windows( void )
{
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    time_t ref = SUNDAY_MIDNIGHT_UTC; /* weekly 0 */

    check_window( s, 0, ref, 3600,  7200  );
    check_window( s, 1, ref, 3600,  10800 );
    check_window( s, 2, ref, 14400, 18000 );
    check_window( s, 3, ref, 14400, 21600 );

    destroy_schedule( s );
}

void test_next_boundary_ordering( void )
{
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    time_t base = SUNDAY_MIDNIGHT_UTC;
    time_t b;

    /* Just after Sunday midnight the very next boundary is the earliest
     * "*_STARTING_SOON" lead, i.e. window start (3600) minus the 15-min lead. */
    b = get_next_notify_boundary( s, base );
    CU_ASSERT_EQUAL( b, base + 3600 - LEAD );

    /* From just past that lead, the next is the d6/c4 window start at 3600. */
    b = get_next_notify_boundary( s, base + 3600 - LEAD );
    CU_ASSERT_EQUAL( b, base + 3600 );

    /* d6 ends at 7200; its ENDING_SOON lead is 7200-900 = 6300. From 3600 the
     * next boundary is the earliest of {7200-900, 10800-900, ...}. */
    b = get_next_notify_boundary( s, base + 3600 );
    CU_ASSERT_EQUAL( b, base + 7200 - LEAD );

    destroy_schedule( s );
}

void test_batch_shared_instant( void )
{
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    time_t base = SUNDAY_MIDNIGHT_UTC;
    downtime_window_t out[8];
    size_t n;

    /* d6 (0) and c4 (1) both START at weekly 3600 -> batched. */
    n = collect_notify_batch( s, base + 3600, DOWNTIME_STARTED, out, 8 );
    CU_ASSERT_EQUAL( n, 2 );

    /* 99 (2) and 90 (3) both START at weekly 14400 -> batched. */
    n = collect_notify_batch( s, base + 14400, DOWNTIME_STARTED, out, 8 );
    CU_ASSERT_EQUAL( n, 2 );

    /* Only d6 ENDS at weekly 7200. */
    n = collect_notify_batch( s, base + 7200, DOWNTIME_ENDED, out, 8 );
    CU_ASSERT_EQUAL( n, 1 );
    if( 1 == n ) {
        CU_ASSERT_EQUAL( out[0].mac_index, 0 );
    }

    destroy_schedule( s );
}

void test_never_and_always_blocked( void )
{
    /* idx 4 ("00") is never blocked -> no window. */
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    downtime_window_t win;
    CU_ASSERT_FALSE( get_mac_downtime_window( s, 4, SUNDAY_MIDNIGHT_UTC, &win ) );
    destroy_schedule( s );
}

void test_window_wraps_sunday_midnight( void )
{
    /* A window that is open across the Sunday-midnight wrap.
     *   t=0      block {0}     (open as the week begins)
     *   t=10800  block {}      (closes 3h into the week)
     *   t=601200 block {0}     (re-opens ~1h before week end, 604800-3600)
     * The last real event blocks {0}, so the timeline is circular and the
     * window spanning the wrap is [601200, 604800+10800). */
    static const ev_t weekly[] = {
        { .time = 0,      .block_count = 1, .block = { 0 } },
        { .time = 10800,  .block_count = 0, .block = { 0 } },
        { .time = 601200, .block_count = 1, .block = { 0 } },
    };
    schedule_t *s = build_schedule( g_macs, 5, weekly,
                                    sizeof(weekly)/sizeof(weekly[0]), NULL, 0 );
    downtime_window_t win;
    /* Reference time just before the wrap (601200 into the week). */
    time_t ref = SUNDAY_MIDNIGHT_UTC + 601200;
    bool ok = get_mac_downtime_window( s, 0, ref, &win );
    CU_ASSERT_TRUE( ok );
    if( ok ) {
        CU_ASSERT_EQUAL( win.start, SUNDAY_MIDNIGHT_UTC + 601200 );
        CU_ASSERT_EQUAL( win.end,   SUNDAY_MIDNIGHT_UTC + SECONDS_IN_A_WEEK + 10800 );
    }
    destroy_schedule( s );
}

void test_short_window_all_four( void )
{
    /* A window shorter than the 15-minute lead: starts at 3600, ends at 3900
     * (5 minutes).  All four boundaries must be derivable; the *_SOON leads
     * (3600-900 and 3900-900) precede the window start but are still valid. */
    static const ev_t weekly[] = {
        { .time = 3600, .block_count = 1, .block = { 0 } },
        { .time = 3900, .block_count = 0, .block = { 0 } },
    };
    schedule_t *s = build_schedule( g_macs, 5, weekly,
                                    sizeof(weekly)/sizeof(weekly[0]), NULL, 0 );
    time_t base = SUNDAY_MIDNIGHT_UTC;
    downtime_window_t out[8];

    CU_ASSERT_EQUAL( collect_notify_batch( s, base + 3600 - LEAD, DOWNTIME_STARTING_SOON, out, 8 ), 1 );
    CU_ASSERT_EQUAL( collect_notify_batch( s, base + 3600,        DOWNTIME_STARTED,       out, 8 ), 1 );
    CU_ASSERT_EQUAL( collect_notify_batch( s, base + 3900 - LEAD, DOWNTIME_ENDING_SOON,   out, 8 ), 1 );
    CU_ASSERT_EQUAL( collect_notify_batch( s, base + 3900,        DOWNTIME_ENDED,         out, 8 ), 1 );

    destroy_schedule( s );
}

void test_absolute_rule_participates( void )
{
    /* A MAC governed only by an absolute (one-time) rule.  The final absolute
     * event is a terminator, so index 0 is blocked across [1000, 2000). */
    static const ev_t absolute[] = {
        { .time = SUNDAY_MIDNIGHT_UTC + 1000, .block_count = 1, .block = { 0 } },
        { .time = SUNDAY_MIDNIGHT_UTC + 2000, .block_count = 0, .block = { 0 } },
    };
    schedule_t *s = build_schedule( g_macs, 5, NULL, 0, absolute,
                                    sizeof(absolute)/sizeof(absolute[0]) );
    downtime_window_t win;
    bool ok = get_mac_downtime_window( s, 0, SUNDAY_MIDNIGHT_UTC, &win );
    CU_ASSERT_TRUE( ok );
    if( ok ) {
        CU_ASSERT_EQUAL( win.start, SUNDAY_MIDNIGHT_UTC + 1000 );
        CU_ASSERT_EQUAL( win.end,   SUNDAY_MIDNIGHT_UTC + 2000 );
    }
    destroy_schedule( s );
}

void test_utc_iso8601_format( void )
{
    char buf[32];
    size_t n = format_utc_iso8601( SUNDAY_MIDNIGHT_UTC, buf, sizeof(buf) );
    CU_ASSERT( n > 0 );
    CU_ASSERT_STRING_EQUAL( buf, "2017-11-12T00:00:00Z" );

    n = format_utc_iso8601( SUNDAY_MIDNIGHT_UTC + 3600, buf, sizeof(buf) );
    CU_ASSERT( n > 0 );
    CU_ASSERT_STRING_EQUAL( buf, "2017-11-12T01:00:00Z" );
}

void test_payload_shape_and_batching( void )
{
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    time_t base = SUNDAY_MIDNIGHT_UTC;
    downtime_window_t batch[8];
    size_t n;
    char *payload;

    /* d6 (0) and c4 (1) both START at weekly 3600 -> one batched payload. */
    n = collect_notify_batch( s, base + 3600, DOWNTIME_STARTED, batch, 8 );
    CU_ASSERT_EQUAL( n, 2 );

    payload = notify_build_payload( s, DOWNTIME_STARTED, base + 3600, batch, n );
    CU_ASSERT_PTR_NOT_NULL_FATAL( payload );

    /* Required fields present. */
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"eventType\":\"DOWNTIME_STARTED\"" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"currentTime\":\"2017-11-12T01:00:00Z\"" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"affectedMacs\":[" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"devicemac\":\"d6:d6:d6:d6:d6:d6\"" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"devicemac\":\"c4:c4:c4:c4:c4:c4\"" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"DownStartTime\":\"2017-11-12T01:00:00Z\"" ) );
    CU_ASSERT_PTR_NOT_NULL( strstr( payload, "\"DownEndTime\":" ) );

    free( payload );
    destroy_schedule( s );
}

void test_persistence_across_restart( void )
{
    const char *persist = "test_notify_sent.dat";
    schedule_t *s = build_schedule( g_macs, 5, g_weekly,
                                    sizeof(g_weekly)/sizeof(g_weekly[0]),
                                    NULL, 0 );
    time_t base = SUNDAY_MIDNIGHT_UTC;
    time_t soon = base + 3600 - LEAD; /* a *_STARTING_SOON instant */
    size_t emitted;

    remove( persist );

    /* Fresh start: the *_SOON has not been sent, so it is emitted. */
    notify_init( persist );
    CU_ASSERT_FALSE( notify_already_sent( soon, DOWNTIME_STARTING_SOON ) );
    emitted = notify_emit( s, soon, soon );
    CU_ASSERT( emitted >= 1 );
    CU_ASSERT_TRUE( notify_already_sent( soon, DOWNTIME_STARTING_SOON ) );
    notify_destroy();

    /* Restart: reload persisted state; the already-sent boundary is suppressed. */
    notify_init( persist );
    CU_ASSERT_TRUE( notify_already_sent( soon, DOWNTIME_STARTING_SOON ) );
    emitted = notify_emit( s, soon, soon );
    CU_ASSERT_EQUAL( emitted, 0 );
    notify_destroy();

    remove( persist );
    destroy_schedule( s );
}

void add_suites( CU_pSuite *suite )
{
    printf( "--------Start of Test Cases Execution For test_notify ---------\n" );
    *suite = CU_add_suite( "tests", NULL, NULL );
    CU_add_test( *suite, "per-MAC windows",            test_per_mac_windows );
    CU_add_test( *suite, "next boundary ordering",     test_next_boundary_ordering );
    CU_add_test( *suite, "batch shared instant",       test_batch_shared_instant );
    CU_add_test( *suite, "never/always blocked",       test_never_and_always_blocked );
    CU_add_test( *suite, "window wraps Sunday",        test_window_wraps_sunday_midnight );
    CU_add_test( *suite, "short window all four",      test_short_window_all_four );
    CU_add_test( *suite, "absolute rule participates", test_absolute_rule_participates );
    CU_add_test( *suite, "UTC ISO-8601 format",        test_utc_iso8601_format );
    CU_add_test( *suite, "payload shape and batching", test_payload_shape_and_batching );
    CU_add_test( *suite, "persistence across restart", test_persistence_across_restart );
}

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
int main( void )
{
    unsigned rv = 1;
    CU_pSuite suite = NULL;

    if( CUE_SUCCESS == CU_initialize_registry() ) {
        add_suites( &suite );

        if( NULL != suite ) {
            CU_basic_set_mode( CU_BRM_VERBOSE );
            CU_basic_run_tests();
            printf( "\n" );
            CU_basic_show_failures( CU_get_failure_list() );
            printf( "\n\n" );
            rv = CU_get_number_of_tests_failed();
        }

        CU_cleanup_registry();
    }

    return rv;
}

int32_t get_max_mac_limit( void )
{
    return 2048;
}
