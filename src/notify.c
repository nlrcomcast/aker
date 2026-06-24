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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ENABLE_FEATURE_TELEMETRY2_0)
   #include <telemetry_busmessage_sender.h>
#endif

#include "notify.h"
#include "schedule.h"
#include "time.h"
#include "aker_log.h"
#include "aker_mem.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* T2 event marker used for all downtime lifecycle notifications. */
#define NOTIFY_T2_MARKER     "Akerdowntime_split"

/* ISO-8601 UTC timestamp buffer size: "YYYY-MM-DDTHH:MM:SSZ" + NUL = 21. */
#define ISO8601_LEN          24

/* The four event types, iterated in lifecycle order. */
#define NOTIFY_EVENT_COUNT   4

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
typedef struct sent_entry {
    time_t           instant;
    downtime_event_t event;
} sent_entry_t;

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
static char         *g_persist_file = NULL;
static sent_entry_t *g_sent = NULL;
static size_t        g_sent_count = 0;
static size_t        g_sent_cap = 0;

static const downtime_event_t g_events[NOTIFY_EVENT_COUNT] = {
    DOWNTIME_STARTING_SOON,
    DOWNTIME_STARTED,
    DOWNTIME_ENDING_SOON,
    DOWNTIME_ENDED
};

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
static void   __load_sent( void );
static void   __persist_sent( void );
static void   __record_sent( time_t instant, downtime_event_t event );
static void   __prune_sent( time_t newest );
static size_t __append( char **buf, size_t *cap, size_t off, const char *s );

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

/* See notify.h for details. */
void notify_init( const char *persist_file )
{
    notify_destroy();

    if( NULL != persist_file ) {
        g_persist_file = strdup( persist_file );
    }

    __load_sent();
}


/* See notify.h for details. */
void notify_destroy( void )
{
    if( NULL != g_sent ) {
        aker_free( g_sent );
        g_sent = NULL;
    }
    g_sent_count = 0;
    g_sent_cap = 0;

    if( NULL != g_persist_file ) {
        free( g_persist_file );
        g_persist_file = NULL;
    }
}


/* See notify.h for details. */
const char* notify_event_name( downtime_event_t event )
{
    switch( event ) {
        case DOWNTIME_STARTING_SOON: return "DOWNTIME_STARTING_SOON";
        case DOWNTIME_STARTED:       return "DOWNTIME_STARTED";
        case DOWNTIME_ENDING_SOON:   return "DOWNTIME_ENDING_SOON";
        case DOWNTIME_ENDED:         return "DOWNTIME_ENDED";
        default:                     return "DOWNTIME_UNKNOWN";
    }
}


/* See notify.h for details. */
char* notify_build_payload( schedule_t *s, downtime_event_t event,
                            time_t current, const downtime_window_t *windows,
                            size_t count )
{
    char *buf = NULL;
    size_t cap = 0;
    size_t off = 0;
    char ts[ISO8601_LEN];
    size_t i;

    if( (NULL == s) || (NULL == windows) || (0 == count) ) {
        return NULL;
    }

    format_utc_iso8601( current, ts, sizeof(ts) );

    off = __append( &buf, &cap, off, "{\"eventType\":\"" );
    off = __append( &buf, &cap, off, notify_event_name( event ) );
    off = __append( &buf, &cap, off, "\",\"currentTime\":\"" );
    off = __append( &buf, &cap, off, ts );
    off = __append( &buf, &cap, off, "\",\"affectedMacs\":[" );

    for( i = 0; i < count; i++ ) {
        const char *mac = "";

        if( windows[i].mac_index < s->mac_count ) {
            mac = s->macs[windows[i].mac_index].mac;
        }

        if( i > 0 ) {
            off = __append( &buf, &cap, off, "," );
        }
        off = __append( &buf, &cap, off, "{\"devicemac\":\"" );
        off = __append( &buf, &cap, off, mac );

        off = __append( &buf, &cap, off, "\",\"DownStartTime\":\"" );
        format_utc_iso8601( windows[i].start, ts, sizeof(ts) );
        off = __append( &buf, &cap, off, ts );

        off = __append( &buf, &cap, off, "\",\"DownEndTime\":\"" );
        format_utc_iso8601( windows[i].end, ts, sizeof(ts) );
        off = __append( &buf, &cap, off, ts );

        off = __append( &buf, &cap, off, "\"}" );
    }

    off = __append( &buf, &cap, off, "]}" );

    if( 0 == off ) {
        if( NULL != buf ) {
            aker_free( buf );
        }
        return NULL;
    }

    return buf;
}


/* See notify.h for details. */
size_t notify_emit( schedule_t *s, time_t instant, time_t current )
{
    size_t emitted = 0;
    size_t ei;

    if( NULL == s ) {
        return 0;
    }

    for( ei = 0; ei < NOTIFY_EVENT_COUNT; ei++ ) {
        downtime_event_t event = g_events[ei];
        downtime_window_t batch[64];
        size_t n;
        char *payload;

        if( notify_already_sent( instant, event ) ) {
            continue;
        }

        n = collect_notify_batch( s, instant, event, batch,
                                  sizeof(batch)/sizeof(batch[0]) );
        if( 0 == n ) {
            continue;
        }

        payload = notify_build_payload( s, event, current, batch, n );
        if( NULL == payload ) {
            continue;
        }

#if defined(ENABLE_FEATURE_TELEMETRY2_0)
        t2_event_s( NOTIFY_T2_MARKER, payload );
        debug_info( "Aker downtime t2 event triggered: %s\n",
                    notify_event_name( event ) );
#else
        debug_info( "Aker downtime notification (T2 disabled): %s %s\n",
                    notify_event_name( event ), payload );
#endif

        aker_free( payload );

        __record_sent( instant, event );
        emitted++;
    }

    if( emitted > 0 ) {
        __persist_sent();
    }

    return emitted;
}


/* See notify.h for details. */
bool notify_already_sent( time_t instant, downtime_event_t event )
{
    size_t i;

    for( i = 0; i < g_sent_count; i++ ) {
        if( (g_sent[i].instant == instant) && (g_sent[i].event == event) ) {
            return true;
        }
    }

    return false;
}


/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

/**
 *  Appends @p s to a growable heap buffer, growing it as needed.  On allocation
 *  failure the buffer is freed and 0 is returned so callers can detect the
 *  error (a successful append always returns a non-zero new offset because the
 *  buffer always begins with a non-empty literal).
 *
 *  @param buf [in/out] the buffer pointer (may be NULL initially)
 *  @param cap [in/out] the current capacity
 *  @param off the current write offset (length so far, excluding NUL)
 *  @param s   the NUL-terminated string to append
 *
 *  @return the new offset, or 0 on allocation failure
 */
static size_t __append( char **buf, size_t *cap, size_t off, const char *s )
{
    size_t need;
    size_t slen;

    if( (NULL == buf) || (NULL == cap) || (NULL == s) ) {
        return 0;
    }

    /* A prior append failed; stay failed. */
    if( (off == 0) && (NULL == *buf) && (*cap != 0) ) {
        return 0;
    }

    slen = strlen( s );
    need = off + slen + 1;

    if( need > *cap ) {
        size_t ncap = (0 == *cap) ? 128 : *cap;
        char *nbuf;

        while( ncap < need ) {
            ncap *= 2;
        }

        nbuf = (char*) aker_malloc( ncap );
        if( NULL == nbuf ) {
            if( NULL != *buf ) {
                aker_free( *buf );
            }
            *buf = NULL;
            *cap = 1; /* mark as failed (non-zero cap, NULL buf) */
            return 0;
        }
        if( NULL != *buf ) {
            memcpy( nbuf, *buf, off );
            aker_free( *buf );
        }
        *buf = nbuf;
        *cap = ncap;
    }

    memcpy( *buf + off, s, slen );
    (*buf)[off + slen] = '\0';

    return off + slen;
}


/**
 *  Loads the persisted set of already-sent boundaries from disk, if a path is
 *  configured and the file exists.  Each line is "instant event".
 */
static void __load_sent( void )
{
    FILE *f;
    long inst;
    int ev;

    if( NULL == g_persist_file ) {
        return;
    }

    f = fopen( g_persist_file, "r" );
    if( NULL == f ) {
        return;
    }

    while( 2 == fscanf( f, "%ld %d", &inst, &ev ) ) {
        if( (ev > DOWNTIME_NOTIFY_NONE) && (ev <= DOWNTIME_ENDED) ) {
            __record_sent( (time_t) inst, (downtime_event_t) ev );
        }
    }

    fclose( f );
}


/**
 *  Persists the in-memory set of already-sent boundaries to disk.
 */
static void __persist_sent( void )
{
    FILE *f;
    size_t i;

    if( NULL == g_persist_file ) {
        return;
    }

    f = fopen( g_persist_file, "w" );
    if( NULL == f ) {
        debug_error( "notify: unable to persist sent state to %s\n",
                     g_persist_file );
        return;
    }

    for( i = 0; i < g_sent_count; i++ ) {
        fprintf( f, "%ld %d\n", (long) g_sent[i].instant,
                 (int) g_sent[i].event );
    }

    fclose( f );
}


/**
 *  Records a boundary as sent in the in-memory set, growing it as needed and
 *  pruning entries older than a week relative to the newest instant.
 *
 *  @param instant the boundary instant
 *  @param event   the boundary event type
 */
static void __record_sent( time_t instant, downtime_event_t event )
{
    if( notify_already_sent( instant, event ) ) {
        return;
    }

    if( g_sent_count == g_sent_cap ) {
        size_t ncap = (0 == g_sent_cap) ? 16 : (g_sent_cap * 2);
        sent_entry_t *n = (sent_entry_t*) aker_malloc( ncap * sizeof(sent_entry_t) );
        if( NULL == n ) {
            return;
        }
        if( NULL != g_sent ) {
            memcpy( n, g_sent, g_sent_count * sizeof(sent_entry_t) );
            aker_free( g_sent );
        }
        g_sent = n;
        g_sent_cap = ncap;
    }

    g_sent[g_sent_count].instant = instant;
    g_sent[g_sent_count].event = event;
    g_sent_count++;

    __prune_sent( instant );
}


/**
 *  Drops recorded boundaries whose instant is more than one week before
 *  @p newest, bounding the persisted set's growth.
 *
 *  @param newest the most recently recorded instant
 */
static void __prune_sent( time_t newest )
{
    time_t cutoff = newest - SECONDS_IN_A_WEEK;
    size_t r = 0, w = 0;

    for( r = 0; r < g_sent_count; r++ ) {
        if( g_sent[r].instant >= cutoff ) {
            g_sent[w++] = g_sent[r];
        }
    }

    g_sent_count = w;
}
