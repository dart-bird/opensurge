/*
 * Open Surge Engine
 * timer.c - time handler
 * Copyright (C) 2010, 2012  Alexandre Martins <alemartf(at)gmail(dot)com>
 * http://opensurge2d.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "util.h"
#include "timer.h"
#include "logfile.h"

#if defined(A5BUILD)
#include <math.h>
#include <sys/time.h> /* FIXME */
#elif defined(_WIN32)
#include <allegro.h>
#include <winalleg.h>
#else
#include <allegro.h>
#include <sys/time.h>
#endif

#ifdef A5BUILD
static const float minimum_delta = 0.016f;
static const float maximum_delta = 0.033f;
static float delta_time = 0.0f;
static uint32_t start_time = 0; /* TODO: remove */

static inline float get_current_time(); /* given in seconds */
static uint32_t get_tick_count();
static inline void yield_cpu();
#else

/* constants */
#define MIN_FRAME_INTERVAL 15 /* (1/15) * 1000 ~ 67 fps max */
#define MAX_FRAME_INTERVAL 17 /* (1/17) * 1000 ~ 58 fps min */


/* internal data */
static uint32_t last_time;
static float delta;
static int must_yield_cpu;
static volatile uint32_t elapsed_time;
static uint32_t start_time;

/* platform-specific code */
static uint32_t get_tick_count(); /* tell me the time */
static void yield_cpu(); /* we don't like using 100% of the cpu */

#endif


/*
 * timer_init()
 * Initializes the Time Handler
 */
void timer_init()
{
    logfile_message("timer_init()");

#ifdef A5BUILD
    /* ignore optimize_cpu_usage (always TRUE) */
    start_time = get_tick_count();
    delta_time = 0.0f;
#else
    /* installing Allegro stuff */
    logfile_message("Installing Allegro timers...");
    if(install_timer() != 0)
        logfile_message("install_timer() failed: %s", allegro_error);

    /* initializing... */
    start_time = get_tick_count();
    delta = 0.0f;

    /* done! */
    last_time = timer_get_ticks();
#endif
}


/*
 * timer_update()
 * Updates the Time Handler. This routine
 * must be called at every cycle of
 * the main loop
 */
void timer_update()
{
#ifdef A5BUILD
    static float old_time = INFINITY;
    float current_time = 0.0f;

    /* compute delta time */
    do {
        current_time = get_current_time();
        delta_time = current_time - old_time; 
        if(current_time < old_time) { /* if time wrap */
            old_time = current_time;
            delta_time = 0.0f;
        }
        if(delta_time < minimum_delta)
            yield_cpu();
    } while(delta_time < minimum_delta);

    if(delta_time > maximum_delta)
        delta_time = maximum_delta;

    old_time = current_time;
#else
    uint32_t current_time, delta_time; /* both in milliseconds */

    /* time control */
    for(delta_time = 0 ;;) {
        current_time = timer_get_ticks();
        delta_time = (current_time > last_time) ? (current_time - last_time) : 0;
        last_time = (current_time >= last_time) ? last_time : current_time;

        if(delta_time < MIN_FRAME_INTERVAL) {
            /* we don't want the cpu usage at 100%. */
            /* will the OS make our process active again on time? */
            yield_cpu();
        }
        else
            break;
    }
    delta_time = min(delta_time, MAX_FRAME_INTERVAL);
    delta = (float)delta_time * 0.001f;

    /* done! */
    last_time = timer_get_ticks();
#endif
}


/*
 * timer_release()
 * Releases the Time Handler
 */
void timer_release()
{
    logfile_message("timer_release()");
}


/*
 * timer_get_delta()
 * Returns the time interval, in seconds,
 * between the last two cycles of the
 * main loop
 */
float timer_get_delta()
{
#ifdef A5BUILD
    return delta_time;
#else
    return delta;
#endif
}


/*
 * timer_get_ticks()
 * Elapsed milliseconds since
 * the application has started
 */
uint32_t timer_get_ticks()
{
#ifdef A5BUILD
    return 1000 * get_current_time();
#else
    uint32_t ticks = get_tick_count();
    if(ticks < start_time)
        start_time = ticks;
    return ticks - start_time;
#endif
}


/* -------- Utilities -------- */
#if defined(A5BUILD)

float get_current_time()
{
    /* FIXME FIXME FIXME */
    return 0.001f * (get_tick_count() - start_time);
}

uint32_t get_tick_count()
{
    /* TODO: remove me */
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec * 1000) + (now.tv_usec / 1000);
}

void yield_cpu()
{
    /* TODO */
}

#elif !defined(_WIN32)

uint32_t get_tick_count()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec * 1000) + (now.tv_usec / 1000);
}


void yield_cpu()
{
    rest(1); /* don't use rest(0) */
}

#else

uint32_t get_tick_count()
{
    return GetTickCount();
}

void yield_cpu()
{
    Sleep(1);
}

#endif
