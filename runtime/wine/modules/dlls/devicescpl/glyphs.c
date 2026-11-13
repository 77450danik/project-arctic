/*
 * Devices and Printers: the pictures of devices
 *
 * Windows 11's Settings shows a device as a glyph of Segoe Fluent Icons in
 * the text's colour: headphones, a speaker, a phone, a laptop, a keyboard, a
 * mouse, a game controller, a printer, or Bluetooth's rune for the rest,
 * picked by the device's class (Bluetooth Assigned Numbers, "Class of
 * Device"). The font is not ours to ship: the same outlines are drawn here
 * on a 16 unit grid, a line of 1.1, sampled 4 x 4 a pixel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"

#include "devicescpl.h"

#define SAMPLES 4
#define LINE    1.1f

enum device_glyph glyph_of_class( UINT32 cod )
{
    UINT major = (cod >> 8) & 0x1f, minor = (cod >> 2) & 0x3f;

    switch (major)
    {
    case 0x01: return GLYPH_LAPTOP;
    case 0x02: return GLYPH_PHONE;
    case 0x04:   /* audio and video: loudspeakers, portable and car audio play aloud */
        return minor == 5 || minor == 7 || minor == 8 || minor == 10 ? GLYPH_SPEAKER : GLYPH_HEADPHONES;
    case 0x05:   /* peripherals: keyboards, pointing devices, game controllers */
        if ((minor & 0x0f) == 1 || (minor & 0x0f) == 2) return GLYPH_GAMEPAD;
        if (((minor >> 4) & 3) == 2) return GLYPH_MOUSE;
        if ((minor >> 4) & 3) return GLYPH_KEYBOARD;
        return GLYPH_BLUETOOTH;
    case 0x06: return minor & 0x20 ? GLYPH_PRINTER : GLYPH_BLUETOOTH;
    }
    return GLYPH_BLUETOOTH;
}

static float segment( float x, float y, float ax, float ay, float bx, float by )
{
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0, ex, ey;

    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ex = ax + t * dx - x;
    ey = ay + t * dy - y;
    return sqrtf( ex * ex + ey * ey );
}

/* the edge of a rounded rectangle */
static float box( float x, float y, float l, float t, float r, float b, float radius )
{
    float cx = (l + r) / 2, cy = (t + b) / 2;
    float qx = fabsf( x - cx ) - ((r - l) / 2 - radius), qy = fabsf( y - cy ) - ((b - t) / 2 - radius);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;

    return fabsf( sqrtf( ox * ox + oy * oy ) + (in < 0 ? in : 0) - radius );
}

static float ring( float x, float y, float cx, float cy, float r )
{
    return fabsf( sqrtf( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) - r );
}

static BOOL dot( float x, float y, float cx, float cy, float r )
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

/* the upper half of a circle, its ends round */
static float upper_arc( float x, float y, float cx, float cy, float r )
{
    if (y <= cy) return ring( x, y, cx, cy, r );
    return fminf( sqrtf( (x - cx + r) * (x - cx + r) + (y - cy) * (y - cy) ),
                  sqrtf( (x - cx - r) * (x - cx - r) + (y - cy) * (y - cy) ) );
}

static BOOL sample( enum device_glyph glyph, float x, float y )
{
    const float w = LINE / 2;

    switch (glyph)
    {
    case GLYPH_BLUETOOTH:
        return segment( x, y, 5.2f, 4.8f, 10.8f, 10.2f ) <= w || segment( x, y, 10.8f, 10.2f, 8, 13 ) <= w ||
               segment( x, y, 8, 13, 8, 3 ) <= w || segment( x, y, 8, 3, 10.8f, 5.8f ) <= w ||
               segment( x, y, 10.8f, 5.8f, 5.2f, 11.2f ) <= w;
    case GLYPH_HEADPHONES:
        return upper_arc( x, y, 8, 8.8f, 5.6f ) <= w || box( x, y, 2.4f, 8.8f, 5.6f, 14, 1.3f ) <= w ||
               box( x, y, 10.4f, 8.8f, 13.6f, 14, 1.3f ) <= w;
    case GLYPH_SPEAKER:
        return box( x, y, 4, 1.5f, 12, 14.5f, 1.6f ) <= w || ring( x, y, 8, 10, 2.3f ) <= w || dot( x, y, 8, 5, 0.9f );
    case GLYPH_PHONE:
        return box( x, y, 4.5f, 1.5f, 11.5f, 14.5f, 1.6f ) <= w || segment( x, y, 7, 12.4f, 9, 12.4f ) <= w;
    case GLYPH_LAPTOP:
        return box( x, y, 3, 3, 13, 10.6f, 1 ) <= w || segment( x, y, 1.5f, 12.8f, 14.5f, 12.8f ) <= w;
    case GLYPH_KEYBOARD:
        if (box( x, y, 1.5f, 4, 14.5f, 12, 1.3f ) <= w || segment( x, y, 5.5f, 10, 10.5f, 10 ) <= w) return TRUE;
        for (int i = 0; i < 4; i++)
            if (dot( x, y, 4.3f + i * 2.47f, 6.4f, 0.6f ) || dot( x, y, 4.3f + i * 2.47f, 8.2f, 0.6f )) return TRUE;
        return FALSE;
    case GLYPH_MOUSE:
        return box( x, y, 4.5f, 1.5f, 11.5f, 14.5f, 3.4f ) <= w || segment( x, y, 8, 1.5f, 8, 6 ) <= w;
    case GLYPH_GAMEPAD:
        return box( x, y, 1.5f, 4.5f, 14.5f, 12, 3.4f ) <= w || segment( x, y, 4.3f, 8.2f, 7, 8.2f ) <= w ||
               segment( x, y, 5.65f, 6.85f, 5.65f, 9.55f ) <= w || dot( x, y, 10.4f, 7.3f, 0.75f ) ||
               dot( x, y, 11.9f, 9, 0.75f );
    case GLYPH_PRINTER:
        return box( x, y, 2, 6, 14, 11.6f, 1.2f ) <= w || segment( x, y, 4.6f, 6, 4.6f, 2 ) <= w ||
               segment( x, y, 4.6f, 2, 11.4f, 2 ) <= w || segment( x, y, 11.4f, 2, 11.4f, 6 ) <= w ||
               box( x, y, 4.6f, 9.4f, 11.4f, 14.5f, 0.5f ) <= w;
    case GLYPH_ADD:
        return segment( x, y, 8, 2.5f, 8, 13.5f ) <= w || segment( x, y, 2.5f, 8, 13.5f, 8 ) <= w;
    case GLYPH_SEND:
        return segment( x, y, 2.5f, 8, 13, 8 ) <= w || segment( x, y, 9, 4, 13, 8 ) <= w ||
               segment( x, y, 13, 8, 9, 12 ) <= w;
    }
    return FALSE;
}

static float *coverage( enum device_glyph glyph, int size )
{
    float *cov = malloc( size * size * sizeof(float) ), unit = 16.0f / size;

    if (!cov) return NULL;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++)
        {
            int hits = 0;

            for (int j = 0; j < SAMPLES; j++)
                for (int i = 0; i < SAMPLES; i++)
                    if (sample( glyph, (px + (i + 0.5f) / SAMPLES) * unit, (py + (j + 0.5f) / SAMPLES) * unit )) hits++;
            cov[py * size + px] = (float)hits / (SAMPLES * SAMPLES);
        }
    return cov;
}

void draw_device_glyph( HDC hdc, enum device_glyph glyph, int x, int y, int size, COLORREF color )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    float *cov = coverage( glyph, size );
    HBITMAP bitmap;
    UINT32 *bits;
    HDC mem;

    if (!cov || !(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        free( cov );
        return;
    }
    for (int i = 0; i < size * size; i++)
    {
        UINT32 a = (UINT32)(cov[i] * 255 + 0.5f);
        bits[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                  (GetBValue( color ) * a / 255);
    }
    mem = CreateCompatibleDC( hdc );
    SelectObject( mem, bitmap );
    GdiAlphaBlend( hdc, x, y, size, size, mem, 0, 0, size, size, blend );
    DeleteDC( mem );
    DeleteObject( bitmap );
    free( cov );
}

/* the glyph inset from the icon's edges (a card's 32 holds a glyph of about 22, as Settings has it) */
HICON make_device_icon( enum device_glyph glyph, int size, int inset, COLORREF color )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    ICONINFO icon = { .fIcon = TRUE };
    UINT32 rgb = (GetRValue( color ) << 16) | (GetGValue( color ) << 8) | GetBValue( color ), *bits;
    int inner = size - 2 * inset;
    float *cov = coverage( glyph, inner );
    HICON ret = NULL;

    if (!cov) return NULL;
    if ((icon.hbmColor = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        for (int i = 0; i < size * size; i++) bits[i] = rgb;
        for (int y = 0; y < inner; y++)
            for (int x = 0; x < inner; x++)
                bits[(y + inset) * size + x + inset] = ((UINT32)(cov[y * inner + x] * 255 + 0.5f) << 24) | rgb;
        icon.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
        ret = CreateIconIndirect( &icon );
        DeleteObject( icon.hbmColor );
        DeleteObject( icon.hbmMask );
    }
    free( cov );
    return ret;
}

/* for the flyouts of the notification area (pnidui.dll): a device's picture by its class */
HICON WINAPI ArcticBluetoothDeviceIcon( UINT32 class_of_device, int size, COLORREF color )
{
    return make_device_icon( glyph_of_class( class_of_device ), size, 0, color );
}
