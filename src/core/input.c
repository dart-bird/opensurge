/*
 * Open Surge Engine
 * input.c - input management
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

#include <allegro5/allegro.h>
#include "input.h"
#include "engine.h"
#include "video.h"
#include "logfile.h"
#include "timer.h"
#include "inputmap.h"
#include "../entities/mobilegamepad.h"
#include "../util/numeric.h"
#include "../util/util.h"
#include "../util/stringutil.h"

/* <base class>: generic input */
struct input_t {
    bool enabled; /* is this input object enabled? */
    bool blocked; /* is this input object blocked for user input? */
    bool state[IB_MAX], oldstate[IB_MAX]; /* state of the buttons */
    void (*update)(input_t*); /* update method */
};

/* <derived class>: mouse input */
struct inputmouse_t {
    input_t base;
    int x, y; /* cursor position */
    int dx, dy, dz; /* delta-x, delta-y, delta-z (mouse mickeys) */
};
static void inputmouse_update(input_t* in);

/* <derived class>: computer-generated input */
struct inputcomputer_t {
    input_t base;
};
static void inputcomputer_update(input_t* in);

/* <derived class>: input with user-defined mapping */
struct inputuserdefined_t {
    input_t base;
    const inputmap_t *inputmap; /* input mapping */
};
static void inputuserdefined_update(input_t* in);

/* list of inputs */
typedef struct input_list_t input_list_t;
struct input_list_t {
    input_t *data;
    input_list_t *next;
};



/* keyboard input */
static bool a5_key[ALLEGRO_KEY_MAX] = { false };

/* mouse input */
#define LEFT_MOUSE_BUTTON   1 /* primary button, 1 << 0 */
#define RIGHT_MOUSE_BUTTON  2 /* secondary button, 1 << 1 */
#define MIDDLE_MOUSE_BUTTON 4 /* tertiary button, 1 << 2 */
static struct {
    int x, y, z; /* position of the cursor */
    int dx, dy, dz; /* deltas */
    int b; /* bit vector of active buttons */
} a5_mouse = { 0 };

/* joystick input */
#define MAX_JOYS         8 /* maximum number of joysticks */
#define MIN_BUTTONS      4 /* minimum number of buttons for a joystick to be considered a gamepad */
#define REQUIRED_AXES    2 /* required number of axes of a stick */
enum { AXIS_X = 0, AXIS_Y = 1 }; /* axes of a stick */

typedef struct joystick_input_t joystick_input_t;
struct joystick_input_t {
    float axis[REQUIRED_AXES]; /* -1.0 <= axis[i] <= 1.0 */
    uint32_t button; /* bit vector */
};

static joystick_input_t joy[MAX_JOYS];
static joystick_input_t *wanted_joy[MAX_JOYS];

static bool ignore_joystick = false;

/* dead-zone for analog input */
static const float DEADZONE_THRESHOLD = 0.2f;

/* analog sticks: sensitivity threshold */
static const float ANALOG_SENSITIVITY_THRESHOLD = 0.5f; /* a value in [0,1] */

/* analog sticks: thresholds for the (x,y) axes */
static const float ANALOG_AXIS_THRESHOLD[REQUIRED_AXES] = {

    /* Pressing up + jump won't make the player jump */
    [AXIS_X] = 0.609f,/* cos(52.5 degrees) ~ 105 degrees horizontally */
    [AXIS_Y] = 0.752f /* sin(48.75 degrees) ~ 97.5 degrees vertically */
    /*[AXIS_Y] = 0.707f*/ /* sin(45 degrees) ~ 90 degrees vertically */

};

/* the joystick pool is used to keep consistent joystick IDs across reconfigurations */
#define POOL_CAPACITY    MAX_JOYS /* how many different joysticks we can connect */
static ALLEGRO_JOYSTICK* joystick_pool[POOL_CAPACITY];
static ALLEGRO_JOYSTICK* query_joystick_pool(int id);
static void refresh_joystick_pool();
static void clear_joystick_pool();
static int joystick_pool_size();

/* private data */
static const char DEFAULT_INPUTMAP_NAME[] = "default";
static input_list_t* input_list = NULL;

/* private methods */
static void input_register(input_t *in);
static void input_unregister(input_t *in);
static void input_clear(input_t *in);
static void remap_joystick_buttons(joystick_input_t* joy);
static void log_joysticks();
static void log_joystick(ALLEGRO_JOYSTICK* joystick);

/* Allegro 5 events */
static void a5_handle_keyboard_event(const ALLEGRO_EVENT* event, void* data);
static void a5_handle_mouse_event(const ALLEGRO_EVENT* event, void* data);
static void a5_handle_joystick_event(const ALLEGRO_EVENT* event, void* data);
static void a5_handle_touch_event(const ALLEGRO_EVENT* event, void* data);
static struct {
    ALLEGRO_EVENT_SOURCE event_source;
    bool initialized;
    int tracked_touch_id;
} emulated_mouse = { .initialized = false, .tracked_touch_id = -1 };






/*
 * public API
 */



/*
 * input_init()
 * Initializes the input module
 */
void input_init()
{
    /* initialize the Allegro input system */
    logfile_message("Initializing the input system...");

    /* initialize the keyboard */
    if(!al_is_keyboard_installed()) {
        if(!al_install_keyboard())
            fatal_error("Can't initialize the keyboard");
    }
    engine_add_event_source(al_get_keyboard_event_source());
    engine_add_event_listener(ALLEGRO_EVENT_KEY_DOWN, NULL, a5_handle_keyboard_event);
    engine_add_event_listener(ALLEGRO_EVENT_KEY_UP, NULL, a5_handle_keyboard_event);

    /* initialize the mouse */
    if(!al_is_mouse_installed()) {
        if(!al_install_mouse())
            fatal_error("Can't initialize the mouse");
    }
    engine_add_event_source(al_get_mouse_event_source());
    engine_add_event_listener(ALLEGRO_EVENT_MOUSE_BUTTON_DOWN, NULL, a5_handle_mouse_event);
    engine_add_event_listener(ALLEGRO_EVENT_MOUSE_BUTTON_UP, NULL, a5_handle_mouse_event);
    engine_add_event_listener(ALLEGRO_EVENT_MOUSE_AXES, NULL, a5_handle_mouse_event);

    /* initialize the joystick */
    if(!al_is_joystick_installed()) {
        if(!al_install_joystick())
            fatal_error("Can't initialize the joystick subsystem");
    }
    engine_add_event_source(al_get_joystick_event_source());
    engine_add_event_listener(ALLEGRO_EVENT_JOYSTICK_CONFIGURATION, NULL, a5_handle_joystick_event);

    /* initialize touch input */
    if(!al_is_touch_input_installed()) {
        if(!al_install_touch_input())
            logfile_message("Can't initialize the multi-touch subsystem");
    }

    if(al_is_touch_input_installed()) {
        logfile_message("Touch input is available");

        logfile_message("Enabling mouse emulation via touch input");
        al_init_user_event_source(&emulated_mouse.event_source);
        emulated_mouse.initialized = true;

        engine_add_event_source(&emulated_mouse.event_source);
        engine_add_event_source(al_get_touch_input_event_source());
        engine_add_event_listener(ALLEGRO_EVENT_TOUCH_BEGIN, NULL, a5_handle_touch_event);
        engine_add_event_listener(ALLEGRO_EVENT_TOUCH_END, NULL, a5_handle_touch_event);
        engine_add_event_listener(ALLEGRO_EVENT_TOUCH_MOVE, NULL, a5_handle_touch_event);
        engine_add_event_listener(ALLEGRO_EVENT_TOUCH_CANCEL, NULL, a5_handle_touch_event);
    }
    else {
        logfile_message("Touch input is unavailable");
        emulated_mouse.initialized = false;
    }

    /* initialize the input list */
    input_list = NULL;

    /* initialize mouse input */
    a5_mouse.b = 0;
    a5_mouse.x = a5_mouse.y = a5_mouse.z = 0;
    a5_mouse.dx = a5_mouse.dy = a5_mouse.dz = 0;

    /* initialize keyboard input */
    for(int i = 0; i < ALLEGRO_KEY_MAX; i++)
        a5_key[i] = false;

    /* initialize joystick input */
    for(int j = 0; j < MAX_JOYS; j++)
        wanted_joy[j] = NULL;

    clear_joystick_pool();
    refresh_joystick_pool();
    log_joysticks();
    ignore_joystick = !input_is_joystick_available();

    /* loading custom input mappings */
    inputmap_init();

#if defined(__ANDROID__)
    /* this seems to be needed on Android TV */
    if(is_tv_device())
        input_reconfigure_joysticks();
#endif
}

/*
 * input_update()
 * Updates all the registered input objects
 */
void input_update()
{
    int num_joys = min(al_get_num_joysticks(), MAX_JOYS);

    /* read joystick input */
    for(int j = 0; j < num_joys; j++) {
        ALLEGRO_JOYSTICK* joystick = al_get_joystick(j);
        int num_sticks = al_get_joystick_num_sticks(joystick);
        int num_buttons = al_get_joystick_num_buttons(joystick);

        /* cap the number of buttons */
        if(num_buttons > MAX_JOYSTICK_BUTTONS)
            num_buttons = MAX_JOYSTICK_BUTTONS;

        /* read the current state */
        ALLEGRO_JOYSTICK_STATE state;
        al_get_joystick_state(joystick, &state);

        /* read buttons */
        joy[j].button = 0;
        for(int b = 0; b < num_buttons; b++)
            joy[j].button |= (state.button[b] != 0) << b;

        /* platform-specific remapping */
        remap_joystick_buttons(&joy[j]);

        /*

        In order to read directional input from the analog sticks, we use the
        following heuristic: read the first reported stick that has two axes.
        Such stick likely corresponds to the left analog stick of the connected
        gamepad - if it exists.

        If we take a look at SDL_GameControllerDB, a community sourced database
        of game controller mappings, we'll see that, in most controllers, entry
        "leftx" is mapped to "a0" (axis 0) and that entry "lefty" is mapped to
        "a1" (axis 1). Entries "rightx" and "righty" aren't mapped as uniformly.
        I infer that axis 0 and 1 likely correspond to the two axes of stick 0
        reported by the Allegro API.

        We only read one analog stick at this time. Other sticks may correspond
        to shoulder buttons acting as analog sticks with a single axis. I don't
        know if the second stick that has two axes (which may be stick 1, 2...),
        as reported by Allegro, can be reliably associated with the right analog
        stick*. Further testing is desirable. How does Allegro divide the analog
        input in sticks**? jstest only reports axes and buttons.

        https://github.com/gabomdq/SDL_GameControllerDB

        (*) Yes, it can if we use Allegro's XInput driver on Windows. Stick 0
            is the "Left Thumbstick" and stick 1 is the "Right Thumbstick". See
            src/win/wjoyxi.c in Allegro's source code.

        (**) the division is merely logical in Allegro's Windows XInput driver.
             A XINPUT_STATE structure holds a XINPUT_GAMEPAD structure. The
             latter describes the current state of the controller. The state
             of the digital buttons is described by a single bitmask, which is
             mapped to logical buttons defined in the Allegro driver. The state
             of the analog sticks is mapped to different logical sticks and axes
             that are also defined by Allegro in src/win/wjoyxi.c. Take a look
             at joyxi_convert_state() in that file.

             https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad

        */

        /* read sticks */
        joy[j].axis[AXIS_X] = 0.0f;
        joy[j].axis[AXIS_Y] = 0.0f;

        for(int stick_id = 0; stick_id < num_sticks; stick_id++) {

            /* safety check */
            /* I use <= because I think al_get_joystick_num_axes() cannot be
               fully trusted with controllers in DInput mode.
               https://www.allegro.cc/forums/thread/614996/1 */
            if(REQUIRED_AXES <= al_get_joystick_num_axes(joystick, stick_id)) {

                float x = state.stick[stick_id].axis[0];
                float y = state.stick[stick_id].axis[1];

                /* ignore what is probably the right analog stick (stick_id = 1
                   with 2 axes), but not a D-Pad that is mapped to a stick
                   (probably the first stick with stick_id >= 2 and 2 axes). */
                int flags = al_get_joystick_stick_flags(joystick, stick_id);
                if(stick_id == 1 && !(flags & ALLEGRO_JOYFLAG_DIGITAL))
                    continue;

                /* ignore the dead-zone and normalize the data back to [-1,1] */
                const float NORMALIZER = 1.0f - DEADZONE_THRESHOLD;
                if(fabs(x) >= DEADZONE_THRESHOLD)
                    joy[j].axis[AXIS_X] += (x - DEADZONE_THRESHOLD * sign(x)) / NORMALIZER;
                if(fabs(y) >= DEADZONE_THRESHOLD)
                    joy[j].axis[AXIS_Y] += (y - DEADZONE_THRESHOLD * sign(y)) / NORMALIZER;

            }

        }

        /* clamp values to [0,1] */
        joy[j].axis[AXIS_X] = clip(joy[j].axis[AXIS_X], -1.0f, 1.0f);
        joy[j].axis[AXIS_Y] = clip(joy[j].axis[AXIS_Y], -1.0f, 1.0f);
    }

    /* remap joystick IDs. The first joystick (if any) must be a valid one!
       This is especially important on Android, which reports the first
       joystick as an accelerometer. Also, we ensure that joystick IDs
       remain consistent across reconfigurations. */
    for(int j = 0; j < MAX_JOYS; j++) {
        wanted_joy[j] = NULL;

        const ALLEGRO_JOYSTICK* target = query_joystick_pool(j);
        if(target != NULL) {
            for(int k = 0; k < num_joys; k++) {
                if(al_get_joystick(k) == target) {
                    wanted_joy[j] = &joy[k];
                    break;
                }
            }
        }
    }

    /* update the input objects */
    for(input_list_t* it = input_list; it; it = it->next) {
        input_t* in = it->data;

        /* save the previous state of the buttons */
        memcpy(in->oldstate, in->state, sizeof(in->oldstate));

        /* clear the current state of the buttons */
        for(inputbutton_t button = 0; button < IB_MAX; button++)
            in->state[button] = false;

        /* accept user input */
        if(!in->blocked)
            in->update(in);
    }
}


/*
 * input_release()
 * Releases the input module
 */
void input_release()
{
    logfile_message("input_release()");

    logfile_message("Releasing registered input objects...");
    for(input_list_t *next, *it = input_list; it; it = next) {
        next = it->next;
        free(it->data);
        free(it);
    }

    inputmap_release();

    if(emulated_mouse.initialized) {
        logfile_message("Disabling mouse emulation via touch input");
        engine_remove_event_source(&emulated_mouse.event_source);
        al_destroy_user_event_source(&emulated_mouse.event_source);
        emulated_mouse.initialized = false;
    }
}


/*
 * input_button_down()
 * Checks if a given button is down
 */
bool input_button_down(const input_t *in, inputbutton_t button)
{
    return in->enabled && in->state[button];
}


/*
 * input_button_pressed()
 * Checks if a given button has just been pressed (not held down)
 */
bool input_button_pressed(const input_t *in, inputbutton_t button)
{
    return in->enabled && (in->state[button] && !in->oldstate[button]);
}


/*
 * input_button_released()
 * Checks if a given button has just been released
 */
bool input_button_released(const input_t *in, inputbutton_t button)
{
    return in->enabled && (!in->state[button] && in->oldstate[button]);
}




/* 
 * input_create_mouse()
 * Creates an input object based on the mouse
 */
input_t *input_create_mouse()
{
    inputmouse_t *me = mallocx(sizeof *me);
    input_t *in = (input_t*)me;

    in->update = inputmouse_update;
    in->enabled = true;
    in->blocked = false;
    me->dx = 0;
    me->dy = 0;
    me->x = 0;
    me->y = 0;
    input_clear(in);

    input_register(in);
    return in;
}




/*
 * input_create_computer()
 * Creates an object that receives "input" from
 * the computer
 */
input_t *input_create_computer()
{
    inputcomputer_t *me = mallocx(sizeof *me);
    input_t *in = (input_t*)me;

    in->update = inputcomputer_update;
    in->enabled = true;
    in->blocked = false;
    input_clear(in);

    input_register(in);
    return in;
}

/*
 * input_create_user()
 * Creates an user's custom input device
 */
input_t *input_create_user(const char* inputmap_name)
{
    inputuserdefined_t *me = mallocx(sizeof *me);
    input_t *in = (input_t*)me;

    in->update = inputuserdefined_update;
    in->enabled = true;
    in->blocked = false;
    input_clear(in);

    /* if there isn't such a inputmap_name, the game will exit beautifully */
    inputmap_name = inputmap_name ? inputmap_name : DEFAULT_INPUTMAP_NAME;
    me->inputmap = inputmap_get(inputmap_name);

    input_register(in);
    return in;
}


/*
 * input_destroy()
 * Destroys an input object
 */
void input_destroy(input_t *in)
{
    input_unregister(in);
    free(in);
}


/*
 * input_disable()
 * Disables an input object
 */
void input_disable(input_t *in)
{
    in->enabled = false;
}


/*
 * input_enable()
 * Enables an input object
 */
void input_enable(input_t *in)
{
    in->enabled = true;
}



/*
 * input_is_enabled()
 * Checks if an input object is enabled
 */
bool input_is_enabled(const input_t *in)
{
    return in->enabled;
}


/*
 * input_is_blocked()
 * Checks if an input object is blocked for user input
 */
bool input_is_blocked(const input_t *in)
{
    return in->blocked;
}


/*
 * input_block()
 * Blocks an input object for user input, but not necessarily for simulated input
 */
void input_block(input_t *in)
{
    in->blocked = true;
}


/*
 * input_unblock()
 * Unblocks an input object for user input
 */
void input_unblock(input_t *in)
{
    in->blocked = false;
}



/*
 * input_simulate_button_down()
 * Useful for computer-controlled input objects
 */
void input_simulate_button_down(input_t *in, inputbutton_t button)
{
    /*in->oldstate[button] = in->state[button];*/ /* this logic creates issues between frames */
    in->state[button] = true;
}



/*
 * input_simulate_button_up()
 * Useful for computer-controlled input objects
 */
void input_simulate_button_up(input_t *in, inputbutton_t button)
{
    in->state[button] = false;
}


/*
 * input_simulate_button_press()
 * Simulate that a button is first pressed;
 * useful for computer-controlled input objects
 */
void input_simulate_button_press(input_t *in, inputbutton_t button)
{
    in->oldstate[button] = false;
    in->state[button] = true;
}


/*
 * input_reset()
 * Resets the input object like if nothing is being held down
 */
void input_reset(input_t *in)
{
    for(inputbutton_t button = 0; button < IB_MAX; button++)
        input_simulate_button_up(in, button);
}


/*
 * input_copy()
 * Copy the state of the buttons of src to dest
 */
void input_copy(input_t *dest, const input_t *src)
{
    /* we just copy the buttons, not the enabled/blocked flags */
    if(src->enabled) {
        memcpy(dest->state, src->state, sizeof(src->state));
        memcpy(dest->oldstate, src->oldstate, sizeof(src->oldstate));
    }
    else {
        memset(dest->state, 0, sizeof(dest->state));
        memset(dest->oldstate, 0, sizeof(dest->oldstate));
    }
}




/*
 * input_is_joystick_available()
 * Checks if there is a plugged joystick
 */
bool input_is_joystick_available()
{
    return input_number_of_joysticks() > 0;
}

/*
 * input_is_joystick_enabled()
 * Is the joystick input enabled?
 */
bool input_is_joystick_enabled()
{
    return !input_is_joystick_ignored() && input_is_joystick_available();
}


/*
 * input_ignore_joystick()
 * Ignores the input received from joysticks
 */
void input_ignore_joystick(bool ignore)
{
    ignore_joystick = ignore;
}


/*
 * input_is_joystick_ignored()
 * Is the joystick input ignored?
 */
bool input_is_joystick_ignored()
{
    return ignore_joystick;
}


/*
 * input_number_of_joysticks()
 * Number of connected and valid joysticks
 */
int input_number_of_joysticks()
{
    int n = 0;

    for(int p = 0; p < POOL_CAPACITY && joystick_pool[p] != NULL; p++) {
        ALLEGRO_JOYSTICK* joystick = joystick_pool[p];
        if(al_get_joystick_active(joystick))
            n++;
    }

    return n;
}


/*
 * input_reconfigure_joysticks()
 * Reconfigures the joysticks. Called when hotplugging, but also may be called manually
 */
void input_reconfigure_joysticks()
{
    logfile_message("Reconfiguring joysticks...");

    al_reconfigure_joysticks();
    refresh_joystick_pool();
    log_joysticks();
}


/*
 * input_print_joysticks()
 * Prints the connected & valid joysticks to the screen
 */
void input_print_joysticks()
{
    int n = input_number_of_joysticks();

    if(n > 0)
        video_showmessage("Found %d joystick%s", n, n != 1 ? "s" : "");
    else
        video_showmessage("No joysticks have been detected");

    for(int j = 0; j < n; j++) {
        ALLEGRO_JOYSTICK* joystick = query_joystick_pool(j);
        if(joystick != NULL)
            video_showmessage("%s", al_get_joystick_name(joystick));
    }
}


/*
 * input_get_xy()
 * Gets the xy coordinates (this will only work for a mouse device)
 */
v2d_t input_get_xy(const inputmouse_t *in)
{
    return v2d_new(in->x, in->y);
}

/*
 * input_change_mapping()
 * Changes the input mapping of an user-defined input device
 */
void input_change_mapping(inputuserdefined_t *in, const char* inputmap_name)
{
    if(str_icmp(inputmap_name, input_get_mapping_name(in)) != 0) {
        input_clear((input_t*)in);
        in->inputmap = inputmap_get(inputmap_name ? inputmap_name : DEFAULT_INPUTMAP_NAME);
        ((input_t*)in)->update((input_t*)in);
    }
}

/*
 * input_get_mapping_name()
 * Returns the mapping name associated to this user-defined input device
 */
const char* input_get_mapping_name(inputuserdefined_t *in)
{
    return in->inputmap->name;
}









/*
 * private stuff
 */


/* registers an input device */
void input_register(input_t *in)
{
    input_list_t *node = mallocx(sizeof *node);

    node->data = in;
    node->next = input_list;
    input_list = node;
}

/* unregisters the given input device */
void input_unregister(input_t *in)
{
    input_list_t *node, *next;

    if(!input_list)
        return;

    if(input_list->data == in) {
        next = input_list->next;
        free(input_list);
        input_list = next;
    }
    else {
        node = input_list;
        while(node->next && node->next->data != in)
            node = node->next;
        if(node->next) {
            next = node->next->next; 
            free(node->next);
            node->next = next;
        }
    }
}

/* clears all the input buttons */
void input_clear(input_t *in)
{
    for(inputbutton_t button = 0; button < IB_MAX; button++)
        in->state[button] = in->oldstate[button] = false;
}

/* update specific input devices */
void inputmouse_update(input_t* in)
{
    inputmouse_t *mouse = (inputmouse_t*)in;

    mouse->x = a5_mouse.x;
    mouse->y = a5_mouse.y;
    mouse->dx = a5_mouse.dx;
    mouse->dy = a5_mouse.dy;
    mouse->dz = a5_mouse.dz;

    in->state[IB_UP] = (mouse->dz > 0);
    in->state[IB_DOWN] = (mouse->dz < 0);
    in->state[IB_LEFT] = false;
    in->state[IB_RIGHT] = false;
    in->state[IB_FIRE1] = (a5_mouse.b & LEFT_MOUSE_BUTTON);
    in->state[IB_FIRE2] = (a5_mouse.b & RIGHT_MOUSE_BUTTON);
    in->state[IB_FIRE3] = (a5_mouse.b & MIDDLE_MOUSE_BUTTON);
    in->state[IB_FIRE4] = false;
    in->state[IB_FIRE5] = false;
    in->state[IB_FIRE6] = false;
    in->state[IB_FIRE7] = false;
    in->state[IB_FIRE8] = false;
}

void inputcomputer_update(input_t* in)
{
    ;
}

void inputuserdefined_update(input_t* in)
{
    inputuserdefined_t *me = (inputuserdefined_t*)in;
    const inputmap_t *im = me->inputmap;

    /* read keyboard input */
    if(im->keyboard.enabled) {
        for(inputbutton_t button = 0; button < IB_MAX; button++)
            in->state[button] = (im->keyboard.scancode[button] > 0) && a5_key[im->keyboard.scancode[button]];
    }

    /* read joystick input */
    if(im->joystick.enabled && input_is_joystick_enabled()) {
        int joy_id = im->joystick.number - 1;
        if(joy_id < MAX_JOYS && wanted_joy[joy_id] != NULL) {
            v2d_t axis = v2d_new(wanted_joy[joy_id]->axis[AXIS_X], wanted_joy[joy_id]->axis[AXIS_Y]);
            v2d_t abs_axis = v2d_new(fabsf(axis.x), fabsf(axis.y));
            float norm_inf = max(abs_axis.x, abs_axis.y);

            if(norm_inf >= ANALOG_SENSITIVITY_THRESHOLD) {
                v2d_t normalized_axis = v2d_normalize(axis);

                in->state[IB_UP] = in->state[IB_UP] || (normalized_axis.y <= -ANALOG_AXIS_THRESHOLD[AXIS_Y]);
                in->state[IB_DOWN] = in->state[IB_DOWN] || (normalized_axis.y >= ANALOG_AXIS_THRESHOLD[AXIS_Y]);
                in->state[IB_LEFT] = in->state[IB_LEFT] || (normalized_axis.x <= -ANALOG_AXIS_THRESHOLD[AXIS_X]);
                in->state[IB_RIGHT] = in->state[IB_RIGHT] || (normalized_axis.x >= ANALOG_AXIS_THRESHOLD[AXIS_X]);

                /*video_showmessage("%f,%f => %f", normalized_axis.x, normalized_axis.y, RAD2DEG * atan2f(normalized_axis.y, normalized_axis.x));*/
            }

            for(inputbutton_t button = 0; button < IB_MAX; button++) {
                uint32_t button_mask = im->joystick.button_mask[(int)button];
                in->state[button] = in->state[button] || ((wanted_joy[joy_id]->button & button_mask) != 0);
            }
        }
    }

    /* read the mobile gamepad as the first joystick (always enabled) */
    if(im->joystick.enabled && im->joystick.number == 1) {
        mobilegamepad_state_t mobile;
        mobilegamepad_get_state(&mobile);

        in->state[IB_UP] = in->state[IB_UP] || ((mobile.dpad & MOBILEGAMEPAD_DPAD_UP) != 0);
        in->state[IB_DOWN] = in->state[IB_DOWN] || ((mobile.dpad & MOBILEGAMEPAD_DPAD_DOWN) != 0);
        in->state[IB_LEFT] = in->state[IB_LEFT] || ((mobile.dpad & MOBILEGAMEPAD_DPAD_LEFT) != 0);
        in->state[IB_RIGHT] = in->state[IB_RIGHT] || ((mobile.dpad & MOBILEGAMEPAD_DPAD_RIGHT) != 0);

        in->state[IB_FIRE1] = in->state[IB_FIRE1] || ((mobile.buttons & MOBILEGAMEPAD_BUTTON_ACTION) != 0);
        in->state[IB_FIRE4] = in->state[IB_FIRE4] || ((mobile.buttons & MOBILEGAMEPAD_BUTTON_BACK) != 0);
    }
}

/* remap joystick buttons according to the underlying platform.
   We want to maintain consistency across platforms */
void remap_joystick_buttons(joystick_input_t* joy)
{
    /* Allegro's numbers for XINPUT button input
       from: src/win/wjoyxi.c (Allegro's source code) */
    enum {
        XINPUT_A = 0,
        XINPUT_B = 1,
        XINPUT_X = 2,
        XINPUT_Y = 3,
        XINPUT_RB = 4,
        XINPUT_LB = 5,
        XINPUT_RT = 6,
        XINPUT_LT = 7,
        XINPUT_BACK = 8,
        XINPUT_START = 9,
        XINPUT_DPAD_R = 10,
        XINPUT_DPAD_L = 11,
        XINPUT_DPAD_D = 12,
        XINPUT_DPAD_U = 13
    };

#if defined(__ANDROID__)
    /*

    Remap Allegro's JS_* button constants to Allegro's buttons of the Windows XInput driver.

    The following JS_* constants are defined in the source code of MODIFIED Allegro 5.2.9 at:
    android/gradle_project/allegro/src/main/java/org/liballeg/android/AllegroActivity.java

    My joystick-related modifications to Allegro 5.2.9:
    1) https://patch-diff.githubusercontent.com/raw/liballeg/allegro5/pull/1483.patch
    2) https://patch-diff.githubusercontent.com/raw/liballeg/allegro5/pull/1507.patch (apply with fuzz=3)

    */
    enum {
        JS_A = 0,
        JS_B = 1,
        JS_X = 2,
        JS_Y = 3,
        JS_L1 = 4,
        JS_R1 = 5,
        JS_DPAD_L = 6,
        JS_DPAD_R = 7,
        JS_DPAD_U = 8,
        JS_DPAD_D = 9,
        JS_START = 10,
        JS_SELECT = 11,
        JS_MODE = 12,
        JS_THUMBL = 13,
        JS_THUMBR = 14,
        JS_L2 = 15,
        JS_R2 = 16,
        JS_C = 17,
        JS_Z = 18,
        JS_DPAD_CENTER = 19
    };

    const int remap[] = {
        [JS_A] = XINPUT_A, /* BUTTON_A := primary action button */
        [JS_B] = XINPUT_B,
        [JS_X] = XINPUT_X,
        [JS_Y] = XINPUT_Y,
        [JS_L1] = XINPUT_LB,
        [JS_R1] = XINPUT_RB,
        [JS_DPAD_L] = XINPUT_DPAD_L,
        [JS_DPAD_R] = XINPUT_DPAD_R,
        [JS_DPAD_U] = XINPUT_DPAD_U,
        [JS_DPAD_D] = XINPUT_DPAD_D,
        [JS_START] = XINPUT_START,
        [JS_SELECT] = XINPUT_BACK,
        [JS_MODE] = -1, /* unused */
        [JS_THUMBL] = XINPUT_RT,
        [JS_THUMBR] = XINPUT_LT,
        [JS_L2] = -1, /* unused */
        [JS_R2] = -1, /* unused */
        [JS_C] = -1, /* unused */
        [JS_Z] = -1, /* unused */
        [JS_DPAD_CENTER] = XINPUT_A
    };

    const int n = sizeof(remap) / sizeof(int);

    /* store the state of the buttons */
    uint32_t buttons = joy->button;

    /* clear the state of the buttons */
    joy->button = 0;

    /* remap buttons */
    for(int js = 0; js < n; js++) {
        if((buttons & (1 << js)) != 0) {
            if(remap[js] >= 0)
                joy->button |= (1 << remap[js]);
        }
    }

    /*

    See also: recommended game actions for gamepad buttons
    https://developer.android.com/develop/ui/views/touch-and-input/game-controllers/controller-input#button
    https://developer.android.com/training/tv/start/controllers#tv-ui-events

    */

#else

    /* do nothing */
    (void)XINPUT_A;
    (void)XINPUT_B;
    (void)XINPUT_X;
    (void)XINPUT_Y;
    (void)XINPUT_RB;
    (void)XINPUT_LB;
    (void)XINPUT_RT;
    (void)XINPUT_LT;
    (void)XINPUT_BACK;
    (void)XINPUT_START;
    (void)XINPUT_DPAD_R;
    (void)XINPUT_DPAD_L;
    (void)XINPUT_DPAD_D;
    (void)XINPUT_DPAD_U;
    (void)joy;

#endif
}

/* clear the joystick pool */
void clear_joystick_pool()
{
    for(int p = 0; p < POOL_CAPACITY; p++)
        joystick_pool[p] = NULL;
}

/* the current size of the joystick pool */
int joystick_pool_size()
{
    int pool_size = 0;

    while(pool_size < POOL_CAPACITY && joystick_pool[pool_size] != NULL)
        pool_size++;

    return pool_size;
}

/* refresh the joystick pool. Call after al_reconfigure_joysticks() */
void refresh_joystick_pool()
{
    /* compute the current size of the pool */
    int pool_size = joystick_pool_size();

    /* the pool is full (really?!). We're not accepting new joysticks */
    if(pool_size >= POOL_CAPACITY)
        return;

    /* we'll try to insert new joysticks into the pool */
    int num_joys = min(al_get_num_joysticks(), MAX_JOYS);
    for(int j = 0; j < num_joys; j++) {
        ALLEGRO_JOYSTICK* joystick = al_get_joystick(j);
        int num_buttons = al_get_joystick_num_buttons(joystick);
        int num_sticks = al_get_joystick_num_sticks(joystick);
        int num_axes = num_sticks > 0 ? al_get_joystick_num_axes(joystick, 0) : 0;
        bool in_the_pool = false;

        /* filter out devices that are not gamepads
           (an accelerometer is reported as the first joystick on Android) */
        if(num_buttons < MIN_BUTTONS || num_axes < REQUIRED_AXES)
            continue;

        /* check if the joystick is in the pool */
        for(int p = 0; p < pool_size && !in_the_pool; p++)
            in_the_pool = (joystick == joystick_pool[p]);

        /* add the joystick to the pool. Give it a new, fixed ID */
        if(!in_the_pool && pool_size < POOL_CAPACITY) {
            int new_id = pool_size++;
            joystick_pool[new_id] = joystick;
        }
    }

    /*

    From the Allegro manual:
    https://liballeg.org/a5docs/trunk/joystick.html#al_reconfigure_joysticks

    After a call to al_reconfigure_joysticks(), "the number returned by
    al_get_num_joysticks may be different, and the handles returned by
    al_get_joystick may be different or be ordered differently."

    "All ALLEGRO_JOYSTICK handles remain valid, but handles for disconnected
    devices become inactive: their states will no longer update, and
    al_get_joystick will not return the handle. Handles for devices which
    **remain connected** (emphasis added) will continue to represent the same
    devices. Previously inactive handles may become active again, being reused
    to represent newly connected devices."

    */
}

/* get a joystick from the joystick pool. This ensures consistent IDs after reconfigurations */
ALLEGRO_JOYSTICK* query_joystick_pool(int id)
{
    /* validate ID */
    if(id < 0 || id >= POOL_CAPACITY)
        return NULL;

    /* select by ID */
    ALLEGRO_JOYSTICK* candidate = joystick_pool[id];

    /* no joystick with the given ID exists in the pool */
    if(candidate == NULL)
        return NULL;

    /* the candidate was disconnected */
    if(!al_get_joystick_active(candidate))
        return NULL;

    /* success! return the candidate */
    return candidate;
}

/* handle a keyboard event */
void a5_handle_keyboard_event(const ALLEGRO_EVENT* event, void* data)
{
    switch(event->type) {

        case ALLEGRO_EVENT_KEY_DOWN:
            a5_key[event->keyboard.keycode] = true;
            break;

        case ALLEGRO_EVENT_KEY_UP:
            a5_key[event->keyboard.keycode] = false;
            break;

    }

    (void)data;
}

/* handle a mouse event */
void a5_handle_mouse_event(const ALLEGRO_EVENT* event, void* data)
{
    #define update_mouse_position() do { \
        a5_mouse.dx = event->mouse.x - a5_mouse.x; \
        a5_mouse.dy = event->mouse.y - a5_mouse.y; \
        a5_mouse.dz = event->mouse.z - a5_mouse.z; \
        a5_mouse.x = event->mouse.x; \
        a5_mouse.y = event->mouse.y; \
        a5_mouse.z = event->mouse.z; \
    } while(0)

    switch(event->type) {

        case ALLEGRO_EVENT_MOUSE_BUTTON_DOWN:
            a5_mouse.b |= 1 << (event->mouse.button - 1);
            update_mouse_position();
            break;

        case ALLEGRO_EVENT_MOUSE_BUTTON_UP:
            a5_mouse.b &= ~(1 << (event->mouse.button - 1));
            update_mouse_position();
            break;

        case ALLEGRO_EVENT_MOUSE_AXES:
            /* we track the position of the mouse using events and not
               ALLEGRO_MOUSE_STATE because we emit mouse events in order
               to emulate mouse input using touch input */
            update_mouse_position();
            break;

    }

    (void)data;
    #undef update_mouse_position
}

/* handle a joystick event */
void a5_handle_joystick_event(const ALLEGRO_EVENT* event, void* data)
{
    switch(event->type) {
        /*
         * Joystick input based on ALLEGRO_JOYSTICK_STATE
         * seems to work better according to several users
         * 
         * tested with Allegro 5.2.5 on Windows using
         * DirectInput devices
         */
        /*
        case ALLEGRO_EVENT_JOYSTICK_AXIS:
            break;

        case ALLEGRO_EVENT_JOYSTICK_BUTTON_DOWN:
            break;

        case ALLEGRO_EVENT_JOYSTICK_BUTTON_UP:
            break;
        */

        /* hot plugging */
        case ALLEGRO_EVENT_JOYSTICK_CONFIGURATION: {
            input_reconfigure_joysticks();
            input_print_joysticks();

            if(input_number_of_joysticks() > 0) {
                /* the user probably wants the joystick input to be enabled
                   (automatic joystick detection) */
                input_ignore_joystick(false);
            }

            break;
        }
    }

    (void)data;
}

/* emulate mouse input using touch input */
void a5_handle_touch_event(const ALLEGRO_EVENT* event, void* data)
{
    ALLEGRO_EVENT my_event;
    static int num_touches = 0;
    const unsigned int left_mouse_button = 1u;

    /* validate */
    if(!emulated_mouse.initialized)
        return;

    /* fill mouse event */
    memset(&my_event, 0, sizeof(my_event));
    switch(event->type) {
        case ALLEGRO_EVENT_TOUCH_BEGIN:
            num_touches++;
            my_event.type = ALLEGRO_EVENT_MOUSE_BUTTON_DOWN;
            my_event.mouse.x = event->touch.x;
            my_event.mouse.y = event->touch.y;
            my_event.mouse.button = left_mouse_button;
            my_event.mouse.pressure = 1.0f;
            break;

        case ALLEGRO_EVENT_TOUCH_MOVE:
            if(event->touch.id != emulated_mouse.tracked_touch_id)
                return; /* skip */

            my_event.type = ALLEGRO_EVENT_MOUSE_AXES;
            my_event.mouse.x = event->touch.x;
            my_event.mouse.y = event->touch.y;
            my_event.mouse.dx = event->touch.dx;
            my_event.mouse.dy = event->touch.dy;
            break;

        case ALLEGRO_EVENT_TOUCH_END:
        case ALLEGRO_EVENT_TOUCH_CANCEL:
            num_touches = max(0, num_touches - 1);
            if(!(event->touch.id == emulated_mouse.tracked_touch_id || num_touches == 0))
                return; /* skip */

            my_event.type = ALLEGRO_EVENT_MOUSE_BUTTON_UP;
            my_event.mouse.x = event->touch.x;
            my_event.mouse.y = event->touch.y;
            my_event.mouse.button = left_mouse_button;
            my_event.mouse.pressure = 1.0f;
            break;

        default:
            return; /* skip */
    }

    /* emit mouse event */
    bool is_valid_touch = (event->touch.id >= 0);
    /*bool is_target_touch = (event->touch.primary);*/
    bool is_target_touch = true; /* do not assume that touch.id is monotonically increasing */

    if(is_valid_touch && is_target_touch) {
        ALLEGRO_EVENT_SOURCE* my_event_source = &emulated_mouse.event_source;
        al_emit_user_event(my_event_source, &my_event, NULL);

        /*
            According to the Allegro docs, al_emit_user_event():

            "Events are copied in and out of event queues, so after this function
            returns the memory pointed to by event may be freed or reused."
        */

        /* track the touch ID */
        if(event->type == ALLEGRO_EVENT_TOUCH_BEGIN)
            emulated_mouse.tracked_touch_id = event->touch.id;
        else if(event->type == ALLEGRO_EVENT_TOUCH_END || event->type == ALLEGRO_EVENT_TOUCH_CANCEL)
            emulated_mouse.tracked_touch_id = -1;
    }
}

/* log joysticks */
void log_joysticks()
{
    int num_joysticks = al_get_num_joysticks();

    if(num_joysticks == 0) {
        logfile_message("No joysticks have been found");
        return;
    }

    logfile_message("Found %d joystick%s", num_joysticks, num_joysticks == 1 ? "" : "s");
    for(int j = 0; j < num_joysticks; j++) {
        ALLEGRO_JOYSTICK* joystick = al_get_joystick(j);
        logfile_message("[Joystick j=%d]", j);
        log_joystick(joystick);
    }
}

/* log joystick info */
void log_joystick(ALLEGRO_JOYSTICK* joystick)
{
    const char* joy_flag[4] = { "", "digital", "analog", "" };

    logfile_message("- name: \"%s\"", al_get_joystick_name(joystick));
    logfile_message("-- %d sticks, %d buttons", al_get_joystick_num_sticks(joystick), al_get_joystick_num_buttons(joystick));

    for(int s = 0; s < al_get_joystick_num_sticks(joystick); s++) {
        logfile_message("-- stick %d (\"%s\")", s, al_get_joystick_stick_name(joystick, s));
        logfile_message("--- flags: 0x%X %s", al_get_joystick_stick_flags(joystick, s), joy_flag[al_get_joystick_stick_flags(joystick, s) & 0x3]);
        logfile_message("--- number of axes: %d", al_get_joystick_num_axes(joystick, s));

        for(int a = 0; a < al_get_joystick_num_axes(joystick, s); a++)
            logfile_message("---- axis %d (\"%s\")", a, al_get_joystick_axis_name(joystick, s, a));
    }

    for(int b = 0; b < al_get_joystick_num_buttons(joystick); b++)
        logfile_message("-- button %d (\"%s\")", b, al_get_joystick_button_name(joystick, b));
}
