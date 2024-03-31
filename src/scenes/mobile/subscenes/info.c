/*
 * Open Surge Engine
 * info.c - engine information subscene for mobile devices
 * Copyright 2008-2024 Alexandre Martins <alemartf(at)gmail.com>
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

#include <stdio.h>
#include "info.h"
#include "../util/touch.h"
#include "../../../core/video.h"
#include "../../../core/image.h"
#include "../../../core/color.h"
#include "../../../core/input.h"
#include "../../../core/font.h"
#include "../../../core/asset.h"
#include "../../../core/global.h"
#include "../../../util/util.h"
#include "../../../util/stringutil.h"

#if defined(__ANDROID__)
#include <allegro5/allegro_android.h>
#endif

typedef struct mobile_subscene_info_t mobile_subscene_info_t;
struct mobile_subscene_info_t {
    mobile_subscene_t super;
    font_t* font;
    input_t* mouse;
    v2d_t touch_previous;
    v2d_t smooth_scroll;
    int max_scroll;
    float scroll;
};

static void init(mobile_subscene_t*);
static void release(mobile_subscene_t*);
static void update(mobile_subscene_t*,v2d_t);
static void render(mobile_subscene_t*,v2d_t);
static const mobile_subscene_t super = { .init = init, .release = release, .update = update, .render = render };

#define FONT_NAME           "BoxyBold"
#define BACKGROUND_COLOR    "303030" /* RGB hex code */
#define PADDING             4

static void set_info_text(font_t* font);

static float SMOOTH_SCROLL_COEFFICIENT = 0.97f;
static void on_touch_start(v2d_t touch_start, void* subscene_ptr);
static void on_touch_move(v2d_t touch_start, v2d_t touch_current, void* subscene_ptr);





/*
 * mobile_subscene_info()
 * Returns a new instance of the info subscene
 */
mobile_subscene_t* mobile_subscene_info()
{
    mobile_subscene_info_t* subscene = mallocx(sizeof *subscene);

    subscene->super = super;
    subscene->font = NULL;
    subscene->mouse = NULL;
    
    return (mobile_subscene_t*)subscene;
}






/*
 * init()
 * Initializes the subscene
 */
void init(mobile_subscene_t* subscene_ptr)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;

    /* create a mouse input */
    input_t* mouse = input_create_mouse();
    subscene->mouse = mouse;

    /* create font */
    font_t* font = font_create(FONT_NAME);
    font_set_position(font, v2d_new(VIDEO_SCREEN_W / 2, PADDING));
    font_set_width(font, min(426, VIDEO_SCREEN_W - 2 * PADDING));
    font_set_align(font, FONTALIGN_CENTER);
    set_info_text(font);
    subscene->font = font;

    /* touch scroll */
    subscene->touch_previous = v2d_new(0.0f, 0.0f);
    subscene->smooth_scroll = v2d_new(0.0f, 0.0f);

    /* scrolling */
    int text_height = font_get_textsize(font).y + PADDING;
    subscene->max_scroll = max(0, text_height - VIDEO_SCREEN_H);
    subscene->scroll = 0.0f;
}

/*
 * release()
 * Releases the subscene
 */
void release(mobile_subscene_t* subscene_ptr)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;

    font_destroy(subscene->font);
    input_destroy(subscene->mouse);

    free(subscene);
}

/*
 * update()
 * Updates the subscene
 */
void update(mobile_subscene_t* subscene_ptr, v2d_t subscene_offset)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;

    /* pause the scroll? */
    bool pause_scroll = (v2d_magnitude(subscene_offset) > 0.0f);
    if(pause_scroll) {
        subscene->smooth_scroll = v2d_new(0.0f, 0.0f);
        return;
    }

    /* faster scroll? */
    handle_touch_input_ex(subscene->mouse, subscene, on_touch_start, NULL, on_touch_move);

    v2d_t new_position = v2d_add(
        font_get_position(subscene->font),
        subscene->smooth_scroll
    );

    new_position.y = clip(new_position.y, -subscene->max_scroll, 0.0f);

    font_set_position(subscene->font, new_position);

    subscene->smooth_scroll = v2d_lerp(
        v2d_new(0.0f, 0.0f),
        subscene->smooth_scroll,
        SMOOTH_SCROLL_COEFFICIENT
    );
}

/*
 * render()
 * Renders the subscene
 */
void render(mobile_subscene_t* subscene_ptr, v2d_t subscene_offset)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;

    /* render background */
    int x = subscene_offset.x;
    int y = subscene_offset.y;

    image_rectfill(x, y, VIDEO_SCREEN_W, VIDEO_SCREEN_H, color_hex(BACKGROUND_COLOR));

    /* render font */
    v2d_t center = v2d_multiply(video_get_screen_size(), 0.5f);
    v2d_t camera = v2d_subtract(center, subscene_offset);

    font_render(subscene->font, camera);
}



/*
 * private
 */

void set_info_text(font_t* font)
{
    char path[2][1024];
    bool multiple_datadirs;
    extern const char LICENSE_TEXT[];

    asset_shared_datadir(path[0], sizeof(path[0]));
    asset_user_datadir(path[1], sizeof(path[1]));
    multiple_datadirs = (0 != strcmp(path[0], path[1]));

    #define SEPARATOR       "    "
    #define NOWRAP_SPACE    "<color=" BACKGROUND_COLOR ">_</color>"
    #define HIGHLIGHT_COLOR "ffee11"

    font_set_text(font,

        "%.48s\n"
        "is created with an open source game engine:\n"
        "\n"
        "%s\n"
        "\n"
        "%s\n"
        "\n"
        "<color=" HIGHLIGHT_COLOR ">Engine"             NOWRAP_SPACE "version:</color>" NOWRAP_SPACE "%s" SEPARATOR
        "<color=" HIGHLIGHT_COLOR ">SurgeScript"        NOWRAP_SPACE "version:</color>" NOWRAP_SPACE "%s" SEPARATOR
        "<color=" HIGHLIGHT_COLOR ">Allegro"            NOWRAP_SPACE "version:</color>" NOWRAP_SPACE "%s" SEPARATOR
        "<color=" HIGHLIGHT_COLOR ">Build"              NOWRAP_SPACE "date:</color>"    NOWRAP_SPACE "%s" SEPARATOR
#if defined(__ANDROID__)
        "<color=" HIGHLIGHT_COLOR ">Platform:</color>"  NOWRAP_SPACE "Android"  NOWRAP_SPACE "%s" SEPARATOR
#else
        "<color=" HIGHLIGHT_COLOR ">Platform:</color>"  NOWRAP_SPACE "%s" SEPARATOR
#endif
        "<color=" HIGHLIGHT_COLOR ">Data"               NOWRAP_SPACE "%s:</color> %s\n%s",

        opensurge_game_name(),

        GAME_COPYRIGHT,
        LICENSE_TEXT,

        GAME_VERSION_STRING,
        surgescript_version_string(),
        allegro_version_string(),

        GAME_BUILD_DATE,

#if defined(__ANDROID__)
        al_android_get_os_version(),
#else
        GAME_PLATFORM_NAME,
#endif

        multiple_datadirs ? "directories" : "directory",
        path[0],
        multiple_datadirs ? path[1] : ""
    );

    #undef HIGHLIGHT_COLOR
    #undef NOWRAP_SPACE
    #undef SEPARATOR
}

/* private */

/* implementation of faster scroll */
void on_touch_move(v2d_t touch_start, v2d_t touch_current, void* subscene_ptr)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;
    v2d_t delta = v2d_new(0.0f, touch_current.y - subscene->touch_previous.y);

    subscene->smooth_scroll = delta;
    subscene->touch_previous = touch_current;
}

void on_touch_start(v2d_t touch_start, void* subscene_ptr)
{
    mobile_subscene_info_t* subscene = (mobile_subscene_info_t*)subscene_ptr;

    subscene->touch_previous = touch_start;
}