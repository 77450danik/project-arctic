/*
 * Arctic window server: calls into the unix side
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINSRV_UNIXLIB_H
#define __WINE_WINSRV_UNIXLIB_H

#include "wine/unixlib.h"

enum rit_event_type
{
    RIT_KEY,              /* code: evdev key, value: 1 pressed, 0 released */
    RIT_BUTTON,           /* code: evdev button, value: 1 pressed, 0 released */
    RIT_MOTION,           /* x, y: relative, accelerated */
    RIT_MOTION_ABSOLUTE,  /* x, y: 0..65535 across the screen */
    RIT_WHEEL,            /* value: 120 per notch, positive away from the user */
    RIT_HWHEEL,           /* value: 120 per notch, positive to the right */
};

struct rit_event
{
    UINT32 type;
    UINT32 code;
    INT32  x, y;
    INT32  value;
};

struct rit_read_params
{
    UINT32            timeout_ms;  /* ~0u: wait until there is input */
    UINT32            max;
    struct rit_event *events;
    UINT32            count;       /* out */
};

enum winsrv_funcs
{
    unix_rit_init,  /* open the input devices of the seat */
    unix_rit_read,  /* wait for input; STATUS_TIMEOUT when there was none */
    unix_funcs_count
};

#endif
