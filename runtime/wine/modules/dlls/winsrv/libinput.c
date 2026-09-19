/*
 * Arctic window server: host input devices, through libinput
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <libinput.h>
#include <libudev.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsrv);

static struct libinput *li;
static double motion_x, motion_y, scroll_x, scroll_y;  /* fractions not sent yet */

static int open_restricted( const char *path, int flags, void *data )
{
    int fd = open( path, flags | O_CLOEXEC );
    if (fd < 0) WARN( "cannot open %s: %s\n", path, strerror( errno ) );
    return fd < 0 ? -errno : fd;
}

static void close_restricted( int fd, void *data )
{
    close( fd );
}

static const struct libinput_interface input_interface =
{
    open_restricted,
    close_restricted,
};

static NTSTATUS rit_init( void *args )
{
    struct udev *udev;

    if (!(udev = udev_new())) return STATUS_UNSUCCESSFUL;
    li = libinput_udev_create_context( &input_interface, NULL, udev );
    udev_unref( udev );
    if (!li) return STATUS_UNSUCCESSFUL;
    if (libinput_udev_assign_seat( li, "seat0" ))
    {
        libinput_unref( li );
        li = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static void add_event( struct rit_read_params *params, UINT32 type, UINT32 code, INT32 x, INT32 y, INT32 value )
{
    struct rit_event *event = &params->events[params->count++];

    event->type = type;
    event->code = code;
    event->x = x;
    event->y = y;
    event->value = value;
}

/* whole units now, the rest later */
static INT32 take( double *acc, double value )
{
    INT32 whole;

    *acc += value;
    whole = (INT32)*acc;
    *acc -= whole;
    return whole;
}

static void device_added( struct libinput_device *device )
{
    MESSAGE( "csrss: input %s\n", libinput_device_get_name( device ) );
    /* touchpads click on tap, as in Windows */
    if (libinput_device_config_tap_get_finger_count( device ))
        libinput_device_config_tap_set_enabled( device, LIBINPUT_CONFIG_TAP_ENABLED );
}

static void translate( struct rit_read_params *params, struct libinput_event *event )
{
    struct libinput_event_keyboard *key;
    struct libinput_event_pointer *pointer;
    INT32 x, y;

    switch (libinput_event_get_type( event ))
    {
    case LIBINPUT_EVENT_DEVICE_ADDED:
        device_added( libinput_event_get_device( event ) );
        break;

    case LIBINPUT_EVENT_KEYBOARD_KEY:
    {
        BOOL pressed;
        UINT32 count;

        key = libinput_event_get_keyboard_event( event );
        pressed = libinput_event_keyboard_get_key_state( key ) == LIBINPUT_KEY_STATE_PRESSED;
        count = libinput_event_keyboard_get_seat_key_count( key );
        /* the same key on two keyboards is one key */
        if (pressed ? count == 1 : count == 0)
            add_event( params, RIT_KEY, libinput_event_keyboard_get_key( key ), 0, 0, pressed );
        break;
    }

    case LIBINPUT_EVENT_POINTER_MOTION:
        pointer = libinput_event_get_pointer_event( event );
        x = take( &motion_x, libinput_event_pointer_get_dx( pointer ) );
        y = take( &motion_y, libinput_event_pointer_get_dy( pointer ) );
        if (x || y) add_event( params, RIT_MOTION, 0, x, y, 0 );
        break;

    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        pointer = libinput_event_get_pointer_event( event );
        x = libinput_event_pointer_get_absolute_x_transformed( pointer, 65536 );
        y = libinput_event_pointer_get_absolute_y_transformed( pointer, 65536 );
        add_event( params, RIT_MOTION_ABSOLUTE, 0, min( max( x, 0 ), 65535 ), min( max( y, 0 ), 65535 ), 0 );
        break;

    case LIBINPUT_EVENT_POINTER_BUTTON:
    {
        BOOL pressed;
        UINT32 count;

        pointer = libinput_event_get_pointer_event( event );
        pressed = libinput_event_pointer_get_button_state( pointer ) == LIBINPUT_BUTTON_STATE_PRESSED;
        count = libinput_event_pointer_get_seat_button_count( pointer );
        if (pressed ? count == 1 : count == 0)
            add_event( params, RIT_BUTTON, libinput_event_pointer_get_button( pointer ), 0, 0, pressed );
        break;
    }

    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        pointer = libinput_event_get_pointer_event( event );
        if (libinput_event_pointer_has_axis( pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ))
            add_event( params, RIT_WHEEL, 0, 0, 0,
                       -(INT32)libinput_event_pointer_get_scroll_value_v120( pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) );
        if (libinput_event_pointer_has_axis( pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ))
            add_event( params, RIT_HWHEEL, 0, 0, 0,
                       (INT32)libinput_event_pointer_get_scroll_value_v120( pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) );
        break;

    /* touchpads and the like: 15 units of motion make a notch */
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
        pointer = libinput_event_get_pointer_event( event );
        if (libinput_event_pointer_has_axis( pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) &&
            (y = take( &scroll_y, -8 * libinput_event_pointer_get_scroll_value( pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) )))
            add_event( params, RIT_WHEEL, 0, 0, 0, y );
        if (libinput_event_pointer_has_axis( pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) &&
            (x = take( &scroll_x, 8 * libinput_event_pointer_get_scroll_value( pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) )))
            add_event( params, RIT_HWHEEL, 0, 0, 0, x );
        break;

    default:
        break;
    }
}

static NTSTATUS rit_read( void *args )
{
    struct rit_read_params *params = args;
    struct libinput_event *event;
    struct pollfd pfd;
    int ret;

    params->count = 0;
    if (!li) return STATUS_INVALID_DEVICE_STATE;

    for (;;)
    {
        libinput_dispatch( li );
        /* room for the most events one libinput event turns into */
        while (params->count + 2 <= params->max && (event = libinput_get_event( li )))
        {
            translate( params, event );
            libinput_event_destroy( event );
        }
        if (params->count) return STATUS_SUCCESS;

        pfd.fd = libinput_get_fd( li );
        pfd.events = POLLIN;
        ret = poll( &pfd, 1, params->timeout_ms == ~0u ? -1 : (int)params->timeout_ms );
        if (!ret) return STATUS_TIMEOUT;
        if (ret < 0 && errno != EINTR) return STATUS_UNSUCCESSFUL;
    }
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    rit_init,
    rit_read,
};
