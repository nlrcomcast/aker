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
#include <ctype.h>
#include <signal.h>
#include <limits.h>

#include "schedule.h"
#include "time.h"
#include "process_data.h"
#include "aker_log.h"
#include "aker_mem.h"
#include "main.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
char* __convert_event_to_string( schedule_t *s, schedule_event_t *e );
int __validate_mac( const char *mac, size_t len );
static bool __event_blocks( schedule_event_t *e, uint32_t mac_index );
static bool __weekly_window( schedule_t *s, uint32_t mac_index, time_t weekly,
                             time_t *start, time_t *end );
static bool __absolute_window( schedule_t *s, uint32_t mac_index,
                               time_t unixtime, time_t *start, time_t *end );
static time_t __weekly_to_unix( time_t unixtime, time_t weekly_now,
                                time_t weekly_target );



/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
 
/* See schedule.h for details. */
schedule_t* create_schedule( void )
{
    schedule_t *s;

    s = (schedule_t*) aker_malloc( sizeof(schedule_t) );
    if( NULL != s ) {
        memset( s, 0, sizeof(schedule_t) );
    }

    return s;
}


/* See schedule.h for details. */
schedule_event_t* create_schedule_event( size_t block_count )
{
    schedule_event_t *s = NULL;
    size_t size;
    size_t max_macs = get_max_mac_limit();

    if (block_count > max_macs) {
        debug_error("create_schedule_event() Error Request %d exceeds maximum %d\n",
                     block_count, max_macs);
        return s;
    }

    size = sizeof(schedule_event_t) + block_count * sizeof(int);

    s = (schedule_event_t*) aker_malloc( size );
    if( NULL != s ) {
        memset( s, 0, size );
        s->block_count = block_count;
    }

    return s;
}

/* See schedule.h for details. */
schedule_event_t* copy_schedule_event( schedule_event_t *e )
{
    schedule_event_t *n;

    n = NULL;
    if( NULL != e ) {
        n = create_schedule_event( e->block_count );
        if( NULL != n ) {
            size_t i;

            for( i = 0; i < e->block_count; i++ ) {
                n->block[i] = e->block[i];
            }
        }
    }

    return n;
}


/* See schedule.h for details. */
int insert_event(schedule_event_t **head, schedule_event_t *e )
{
    schedule_event_t *cur, *prev;

    if( (NULL == head) || (NULL == e) ) {
        return 0;
    }

    cur = prev = *head;
    while( (NULL != cur) && (cur->time < e->time) ) {
        prev = cur;
        cur = cur->next;
    }

    e->next = cur;
    if( (NULL != prev) && (e->time == prev->time) ) {
        /* A duplicate entry is a mistake in the schedule. */
        return -1;
    } else if( (NULL == prev) || (e->time < prev->time) ) {
        *head = e;
    } else {
        prev->next = e;
    }

    return 0;
}


/* See schedule.h for details. */
int finalize_schedule( schedule_t *s )
{
    int rv = 0;

    if( NULL != s ) {
        if( NULL != s->weekly ) {
            /* Ensure that we have the right starting point: the last event
             * from the previous week's schedule. */
            if( 0 < s->weekly->time ) {
                schedule_event_t *e, *p;

                p = s->weekly;
                while( NULL != p->next ) {
                    p = p->next;
                }

                e = copy_schedule_event( p );
                if( NULL != e ) {
                    e->time = p->time - SECONDS_IN_A_WEEK;
                    rv = insert_event( &s->weekly, e );
                } else {
                    rv = -1;
                }
            }
        }
    }

    return rv;
}

/* See schedule.h for details. */
void destroy_schedule( schedule_t *s )
{
    if( NULL != s ) {
        schedule_event_t *n;

        while( NULL != s->absolute ) {
            n = s->absolute->next;
            aker_free( s->absolute );
            s->absolute = n;
        }

        while( NULL != s->weekly ) {
            n = s->weekly->next;
            aker_free( s->weekly );
            s->weekly = n;
        }

        if( NULL != s->macs ) {
            aker_free( s->macs );
        }
        
        if (NULL != s->time_zone) {
            aker_free( s->time_zone);
        }

        aker_free( s );
    }
}


/* See schedule.h for details. */
char* get_blocked_at_time( schedule_t *s, time_t unixtime )
{
    schedule_event_t *abs_prev, *abs_cur, *w_prev, *w_cur;
    char *rv;
    time_t weekly, last_abs;

    weekly = convert_unix_time_to_weekly( unixtime );

    rv = NULL;

    if( NULL != s ) {
        /* Check absolute schedule first */
        abs_prev = s->absolute;
        abs_cur = NULL;
        if( NULL != abs_prev ) {
            abs_cur = abs_prev->next;
        }

        while( (NULL != abs_cur) && (abs_cur->time <= unixtime) ) {
            abs_prev = abs_cur;
            abs_cur = abs_cur->next;
        }

        /* Make the default relative value of the absolute time in the future
         * so it's ignored. */
        last_abs = weekly + 1;
        if( NULL != abs_prev ) {
            if( (NULL != abs_cur) && (abs_prev->time <= unixtime) ) {
                /* In the absolute schedule */
                rv = __convert_event_to_string( s, abs_prev );
                goto done;
            }

            last_abs = convert_unix_time_to_weekly( abs_prev->time );
        }

        /* Either we're not in the abs schedule or it just ended
         * and we need to figure out the next event time for the end. */

        /* Get the relative schedule */
        w_prev = s->weekly;
        w_cur = NULL;
        if( NULL != w_prev ) {
            w_cur = w_prev->next;
        }

        while( (NULL != w_cur) && (w_cur->time <= weekly) ) {
            w_prev = w_cur;
            w_cur = w_cur->next;
        }

        /* If the abs time event is the most recent, use it as long
         * as it's in the past.  Otherwise use the weekly schedule. */
        if( NULL != w_prev) {
            if( (w_prev->time < last_abs) && (last_abs <= weekly) ) {
                rv = __convert_event_to_string( s, abs_prev );
            } else {
                rv = __convert_event_to_string( s, w_prev );
            }
        } else {
            if( (NULL != abs_prev) && (abs_prev->time <= unixtime) ) {
                rv = __convert_event_to_string( s, abs_prev );
            }
        }
    }

done:
    debug_info( "Time: %ld (%ld) -> '%s'\n", unixtime, weekly, rv );
    return rv;
}


/* See schedule.h for details. */
int create_mac_table( schedule_t *s, size_t count )
{
    s->macs = (mac_address*) aker_malloc( count * sizeof(mac_address) );
    if( NULL == s->macs ) {
        return -1;
    }
    s->mac_count = count;

    memset( s->macs, 0, count * sizeof(mac_address) );

    return 0;
}



/* See schedule.h for details. */
int set_mac_index( schedule_t *s, const char *mac, size_t len, uint32_t index )
{
    int rv;

    rv = -1;
    if( (NULL != s) && (index < s->mac_count) ) {
        rv = __validate_mac( mac, len );
        if( 0 == rv ) {
            memcpy( &s->macs[index].mac[0], mac, len );
            s->macs[index].mac[len] = '\0';
        }
    }

    return rv;
}


/* See schedule.h for details. */
time_t get_next_unixtime(schedule_t *s, time_t unixtime)
{
    schedule_event_t *p;
    time_t next_unixtime = INT_MAX, first_weekly = INT_MAX;
    uint32_t num_events = 0;

    if( NULL != s ) {
        time_t weekly;

        /* Check absolute schedule first */
        for( p = s->absolute; NULL != p; p = p->next ) {
            if( (p->time > unixtime) && (p->time < next_unixtime) ) {
                next_unixtime = p->time;
                goto done;
            }
        }

        /* Check the relative schedule next */
        weekly = convert_unix_time_to_weekly( unixtime );

        for( p = s->weekly; NULL != p; p = p->next ) {
            time_t t = (unixtime - weekly) + p->time;
            if( (p->time > weekly) && (t < next_unixtime) ) {
                next_unixtime = t;
            }

            if( 0 < p->time ) {
                if( 0 == num_events ) {
                    first_weekly = p->time;
                }
                num_events++;
            }
        }

        if( 0 == num_events ) {
            next_unixtime = INT_MAX;
        } else if ( INT_MAX == next_unixtime ) {
            next_unixtime = (unixtime - weekly) + first_weekly + SECONDS_IN_A_WEEK;
        }
    }

done:
    debug_info( "Next unix time: %ld\n", next_unixtime );
    return next_unixtime;
}


/* See schedule.h for details. */
bool get_mac_downtime_window( schedule_t *s, uint32_t mac_index,
                              time_t unixtime, downtime_window_t *win )
{
    time_t weekly;
    time_t w_start = 0, w_end = 0;
    time_t a_start = 0, a_end = 0;
    bool have_weekly, have_abs;

    if( (NULL == s) || (NULL == win) ) {
        return false;
    }

    weekly = convert_unix_time_to_weekly( unixtime );

    have_weekly = __weekly_window( s, mac_index, weekly, &w_start, &w_end );
    if( have_weekly ) {
        /* Project the weekly (Sunday-relative) edges onto the absolute timeline. */
        w_start = __weekly_to_unix( unixtime, weekly, w_start );
        w_end   = __weekly_to_unix( unixtime, weekly, w_end );
    }

    have_abs = __absolute_window( s, mac_index, unixtime, &a_start, &a_end );

    if( !have_weekly && !have_abs ) {
        return false;
    }

    win->mac_index = mac_index;

    if( have_weekly && have_abs ) {
        /* Prefer a window that currently contains now; otherwise the one that
         * starts soonest. */
        bool w_now = (w_start <= unixtime) && (unixtime < w_end);
        bool a_now = (a_start <= unixtime) && (unixtime < a_end);

        if( a_now && !w_now ) {
            win->start = a_start; win->end = a_end;
        } else if( w_now && !a_now ) {
            win->start = w_start; win->end = w_end;
        } else if( a_start <= w_start ) {
            win->start = a_start; win->end = a_end;
        } else {
            win->start = w_start; win->end = w_end;
        }
    } else if( have_abs ) {
        win->start = a_start; win->end = a_end;
    } else {
        win->start = w_start; win->end = w_end;
    }

    return true;
}


/* See schedule.h for details. */
time_t get_next_notify_boundary( schedule_t *s, time_t unixtime )
{
    time_t next = INT_MAX;
    size_t i;

    if( NULL == s ) {
        return INT_MAX;
    }

    for( i = 0; i < s->mac_count; i++ ) {
        downtime_window_t win;
        time_t ref = unixtime;
        int guard;

        /* Walk forward window-by-window for this MAC, gathering the four
         * candidate boundaries (start, end, and each minus the lead) until we
         * find the earliest that is strictly in the future.  The guard bounds
         * the walk in case every boundary is in the past. */
        for( guard = 0; guard < 8; guard++ ) {
            time_t cand[4];
            size_t c;

            if( !get_mac_downtime_window( s, (uint32_t) i, ref, &win ) ) {
                break;
            }

            cand[0] = win.start - NOTIFY_LEAD_SECONDS;
            cand[1] = win.start;
            cand[2] = win.end - NOTIFY_LEAD_SECONDS;
            cand[3] = win.end;

            for( c = 0; c < 4; c++ ) {
                if( (cand[c] > unixtime) && (cand[c] < next) ) {
                    next = cand[c];
                }
            }

            /* Advance past this window's end to discover the next window. */
            ref = win.end + 1;

            /* If we already found a boundary at or before this window's end we
             * cannot do better for this MAC by looking further out. */
            if( next <= win.end ) {
                break;
            }
        }
    }

    debug_info( "Next notify boundary: %ld\n", next );
    return next;
}


/* See schedule.h for details. */
size_t collect_notify_batch( schedule_t *s, time_t instant,
                             downtime_event_t event,
                             downtime_window_t *out, size_t max )
{
    size_t count = 0;
    size_t i;

    if( (NULL == s) || (NULL == out) ) {
        return 0;
    }

    for( i = 0; (i < s->mac_count) && (count < max); i++ ) {
        downtime_window_t win;
        bool matched = false;
        int pass;

        /* A downtime window is half-open [start, end): at the exact end instant
         * get_mac_downtime_window() resolves to the *next* window, so for the
         * end-based edges we also probe one second earlier to catch the window
         * that is closing precisely now. */
        for( pass = 0; (pass < 2) && !matched; pass++ ) {
            time_t ref = (0 == pass) ? instant : (instant - 1);
            time_t edge;

            if( !get_mac_downtime_window( s, (uint32_t) i, ref, &win ) ) {
                break;
            }

            switch( event ) {
                case DOWNTIME_STARTING_SOON: edge = win.start - NOTIFY_LEAD_SECONDS; break;
                case DOWNTIME_STARTED:       edge = win.start;                       break;
                case DOWNTIME_ENDING_SOON:   edge = win.end - NOTIFY_LEAD_SECONDS;   break;
                case DOWNTIME_ENDED:         edge = win.end;                         break;
                default:                     edge = INT_MAX;                         break;
            }

            if( edge == instant ) {
                out[count++] = win;
                matched = true;
            }
        }
    }

    return count;
}


/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/


/**
 *  Returns true if the event's block list contains the given MAC index.
 *
 *  @param e         the event to inspect
 *  @param mac_index the MAC index to look for
 *
 *  @return true if blocked by this event, false otherwise
 */
static bool __event_blocks( schedule_event_t *e, uint32_t mac_index )
{
    size_t i;

    if( NULL == e ) {
        return false;
    }

    for( i = 0; i < e->block_count; i++ ) {
        if( e->block[i] == mac_index ) {
            return true;
        }
    }

    return false;
}


/**
 *  Converts a weekly time (seconds since Sunday midnight) into the absolute
 *  Unix instant of its occurrence relative to @p unixtime / @p weekly_now.
 *
 *  @param unixtime    the reference absolute instant
 *  @param weekly_now  the weekly time corresponding to @p unixtime
 *  @param weekly_target the weekly time to project
 *
 *  @return the absolute Unix instant of weekly_target in the current week
 */
static time_t __weekly_to_unix( time_t unixtime, time_t weekly_now,
                                time_t weekly_target )
{
    return (unixtime - weekly_now) + weekly_target;
}


/**
 *  Computes the current-or-next downtime window for a MAC over the absolute
 *  (one-time) schedule.  Absolute event times are already absolute Unix
 *  instants and the timeline is linear (no weekly wrap).  Consistent with
 *  get_blocked_at_time(), the final absolute event acts as a terminator and
 *  cannot itself open a window.
 *
 *  @param s         the schedule to evaluate
 *  @param mac_index the MAC index to resolve
 *  @param unixtime  the reference absolute instant
 *  @param start     [out] the window start (absolute Unix time)
 *  @param end       [out] the window end (absolute Unix time)
 *
 *  @return true if a current-or-upcoming absolute window exists, else false
 */
static bool __absolute_window( schedule_t *s, uint32_t mac_index,
                               time_t unixtime, time_t *start, time_t *end )
{
    schedule_event_t *p;
    time_t cur_start = 0;
    bool in_window = false;
    time_t best_start = INT_MAX, best_end = INT_MAX;
    bool found = false;

    if( (NULL == s) || (NULL == s->absolute) ) {
        return false;
    }

    for( p = s->absolute; (NULL != p) && (NULL != p->next); p = p->next ) {
        bool now_blocked = __event_blocks( p, mac_index );

        if( now_blocked && !in_window ) {
            cur_start = p->time;
            in_window = true;
        } else if( !now_blocked && in_window ) {
            time_t w_end = p->time;
            in_window = false;

            if( (cur_start <= unixtime) && (unixtime < w_end) ) {
                *start = cur_start;
                *end = w_end;
                return true;
            }
            if( (cur_start >= unixtime) && (cur_start < best_start) ) {
                best_start = cur_start;
                best_end = w_end;
                found = true;
            }
        }
    }

    /* A window still open when the terminator event is reached closes at the
     * terminator's time. */
    if( in_window && (NULL != p) ) {
        time_t w_end = p->time;

        if( (cur_start <= unixtime) && (unixtime < w_end) ) {
            *start = cur_start;
            *end = w_end;
            return true;
        }
        if( (cur_start >= unixtime) && (cur_start < best_start) ) {
            best_start = cur_start;
            best_end = w_end;
            found = true;
        }
    }

    if( found ) {
        *start = best_start;
        *end = best_end;
        return true;
    }

    return false;
}


/**
 *  Computes the current-or-next downtime window for a MAC over the weekly
 *  schedule, treated as a circular timeline.  Returned edges are weekly times
 *  (seconds since Sunday midnight) and the end may be numerically larger than
 *  SECONDS_IN_A_WEEK when the window wraps past Sunday midnight.
 *
 *  The walk collects every maximal blocked span as a (rising-edge, falling-edge)
 *  pair, then selects the span that is current at @p weekly or, failing that,
 *  the soonest upcoming one (wrapping to the first span of the next week when
 *  @p weekly is past them all).
 *
 *  @param s         the schedule to evaluate
 *  @param mac_index the MAC index to resolve
 *  @param weekly    the reference weekly time
 *  @param start     [out] the window start (weekly time)
 *  @param end       [out] the window end (weekly time, possibly > 1 week)
 *
 *  @return true if a window exists, false if the MAC is never blocked or is
 *          blocked across every event with no gap
 */
static bool __weekly_window( schedule_t *s, uint32_t mac_index, time_t weekly,
                             time_t *start, time_t *end )
{
    schedule_event_t *p, *last_real;
    size_t total, blocking;
    bool prev_blocked;
    time_t cur_start = 0;
    bool in_window = false;
    time_t first_start = INT_MAX, first_end = INT_MAX;
    time_t best_start = INT_MAX, best_end = INT_MAX;
    bool found = false;

    if( (NULL == s) || (NULL == s->weekly) ) {
        return false;
    }

    /* Count real events (skipping the negative-time wrap sentinel) and how many
     * block this MAC, to detect the never-blocked and always-blocked cases. */
    total = blocking = 0;
    last_real = NULL;
    for( p = s->weekly; NULL != p; p = p->next ) {
        if( 0 > p->time ) {
            continue;
        }
        total++;
        last_real = p;
        if( __event_blocks( p, mac_index ) ) {
            blocking++;
        }
    }

    if( (0 == blocking) || (blocking == total) ) {
        return false;
    }

    /* Seed the "previous" membership from the last real event so the circular
     * timeline is honoured: a window open at week's end continues into the
     * next week. */
    prev_blocked = __event_blocks( last_real, mac_index );
    if( prev_blocked ) {
        /* A window is already open as the week begins; it started at the last
         * rising edge of the previous week.  Find that edge by scanning for the
         * last falling->rising transition, expressed as a negative offset. */
        bool pblk = false;
        time_t open_at = last_real->time - SECONDS_IN_A_WEEK;
        for( p = s->weekly; NULL != p; p = p->next ) {
            bool b;
            if( 0 > p->time ) {
                continue;
            }
            b = __event_blocks( p, mac_index );
            if( b && !pblk ) {
                open_at = p->time - SECONDS_IN_A_WEEK;
            }
            pblk = b;
        }
        cur_start = open_at;
        in_window = true;
    }

    for( p = s->weekly; NULL != p; p = p->next ) {
        bool now_blocked;

        if( 0 > p->time ) {
            continue;
        }
        now_blocked = __event_blocks( p, mac_index );

        if( now_blocked && !in_window ) {
            cur_start = p->time;
            in_window = true;
        } else if( !now_blocked && in_window ) {
            time_t w_start = cur_start;
            time_t w_end = p->time;

            in_window = false;

            if( INT_MAX == first_start ) {
                first_start = w_start;
                first_end = w_end;
            }

            /* Current window: weekly falls within [start, end). */
            if( (w_start <= weekly) && (weekly < w_end) ) {
                *start = w_start;
                *end = w_end;
                return true;
            }

            /* Soonest upcoming window starting at or after now. */
            if( (w_start > weekly) && (w_start < best_start) ) {
                best_start = w_start;
                best_end = w_end;
                found = true;
            }
        }
    }

    if( found ) {
        *start = best_start;
        *end = best_end;
        return true;
    }

    /* Past every window this week: wrap to the first window of next week. */
    if( INT_MAX != first_start ) {
        *start = first_start + SECONDS_IN_A_WEEK;
        *end = first_end + SECONDS_IN_A_WEEK;
        return true;
    }

    return false;
}



/**
 *  Convert a block pointing to a list of macs in a schedule into a string
 *  of the MAC addresses.
 *
 *  @param s the schedule to use to for resolution
 *  @param e the event to convert
 *
 *  @return the string with the list of blocked addresses (may be NULL and valid)
 */
char* __convert_event_to_string( schedule_t *s, schedule_event_t *e )
{
    char *rv;

    rv = NULL;
    if( (NULL != s) && (NULL != e) ) {
        size_t count;

        count = e->block_count;

        rv = (char*) aker_malloc( sizeof(char) * (count * MAC_ADDRESS_SIZE + 1) );
        if( NULL != rv ) {
            char *p = rv;
            bool string_ok = false;
            size_t i;

            for( i = 0; i < count; i++ ) {
                if( e->block[i] < s->mac_count ) {
                    string_ok = true;
                    memcpy( p, &s->macs[e->block[i]], 17 );
                    p[17] = ' ';
                } else {
                    debug_error("__convert_event_to_string():Invalid mac index\n");
                    string_ok = false;
                    break;
                }
                p = &p[18];
            }
            *p = '\0';

            /* Don't send back an empty string, just put it out of it's
             * misery here. */
            if( false == string_ok ) {
                aker_free( rv );
                rv = NULL;
            } else {
                /* Chomp the extra ' ' and make it a '\0'. */
                p[-1] = '\0';
            }
        }
    }

    return rv;
}


/**
 *  Validates that the MAC address is in the expected format.
 *
 *  @param mac the MAC address to validate
 *  @param len the length of the mac string
 *
 *  @return 0 if valid, failure otherwise
 */
int __validate_mac( const char *mac, size_t len )
{
    int mask = -1;

    if( (NULL != mac) && (17 == len) ) {
        mask  = ! isxdigit(mac[0]);
        mask |= ! isxdigit(mac[1]);
        mask |= mac[2] - ':';
        mask |= ! isxdigit(mac[3]);
        mask |= ! isxdigit(mac[4]);
        mask |= mac[5] - ':';
        mask |= ! isxdigit(mac[6]);
        mask |= ! isxdigit(mac[7]);
        mask |= mac[8] - ':';
        mask |= ! isxdigit(mac[9]);
        mask |= ! isxdigit(mac[10]);
        mask |= mac[11] - ':';
        mask |= ! isxdigit(mac[12]);
        mask |= ! isxdigit(mac[13]);
        mask |= mac[14] - ':';
        mask |= ! isxdigit(mac[15]);
        mask |= ! isxdigit(mac[16]);
    }

    return mask;
}
