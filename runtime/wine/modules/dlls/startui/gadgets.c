/*
 * The widgets of the desktop: the day, the date and the time in large thin
 * letters; the processor, the graphics card, the memory and the disk C: as
 * the Games panel shows them; and the battery. They stay just above the
 * desktop, show a frame while the pointer is over them or a button is down
 * on them, and are dragged anywhere by it. HKCU\Software\Arctic\DesktopWidgets;
 * the Display panel's "Widgets..." (desk.cpl, ArcticDesktopWidgetsDialog)
 * turns them on and off.
 *
 * Copyright 2026 Arctic contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "dwmapi.h"
#include "shlobj.h"
#include "shellapi.h"
#include "wine/arctic_dwm.h"
#include "wine/arctic_power.h"

#define GADGETS_KEY      L"Software\\Arctic\\DesktopWidgets"
#define GADGETS_CHANGED  L"ArcticDesktopWidgets"   /* WM_SETTINGCHANGE's lparam */

enum gadget_kind { G_CLOCK, G_MONITOR, G_BATTERY, G_COUNT };

static const WCHAR *const gadget_names[G_COUNT] = { L"Clock", L"Monitor", L"Battery" };
static const DWORD gadget_default[G_COUNT] = { 1, 0, 0 };

/* the monitoring's rows, HKCU\...\MonitorItems */
enum { ITEM_CPU = 1, ITEM_GPU = 2, ITEM_RAM = 4, ITEM_DISK = 8, ITEM_ALL = 15 };

#define TIMER_TICK  1
#define TIMER_ICONS 2
#define ID_REMOVE   1
#define ID_SETTINGS 2

struct gadget
{
    HWND hwnd;
    BOOL hot, pressed, light;
    int width, height;
};

static struct gadget gadgets[G_COUNT];
static HWND manager;
static DWORD monitor_items = ITEM_ALL, clock_english;
static struct arctic_sensors sensors;
static BOOL have_sensors;
static SYSTEMTIME shown_time;
static int ticks;

static DWORD get_value( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, GADGETS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

static BOOL find_value( const WCHAR *name, DWORD *value )
{
    DWORD size = sizeof(*value);

    return !RegGetValueW( HKEY_CURRENT_USER, GADGETS_KEY, name, RRF_RT_REG_DWORD, NULL, value, &size );
}

static void set_value( const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, GADGETS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static void delete_value( const WCHAR *name )
{
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, GADGETS_KEY, 0, KEY_SET_VALUE, &key )) return;
    RegDeleteValueW( key, name );
    RegCloseKey( key );
}

/**********************************************************************
 *          The letters of the day: thin, wide, round, own drawings
 *
 * In units of the capital's height, y down; 1 x0 y0 x1 y1 is a line,
 * 2 cx cy r from to an arc (degrees, 0 to the right, 90 down), 0 the end.
 */

struct letter { WCHAR ch; float width; const float *ops; };

#define LN 1.f
#define AR 2.f

static const float l_a[] = { LN,0,1,.45f,0, LN,.45f,0,.9f,1, 0 };
static const float l_be[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.45f,.625f,.45f, AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_b[] = { LN,0,0,0,1, LN,0,0,.625f,0, AR,.625f,.225f,.225f,-90,90, LN,0,.45f,.625f,.45f,
                             AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_ge[] = { LN,0,0,0,1, LN,0,0,.9f,0, 0 };
static const float l_de[] = { LN,.25f,0,.8f,0, LN,.8f,0,.8f,1, LN,.25f,0,.1f,1, LN,-.05f,1,.95f,1, LN,-.05f,1,-.05f,1.15f,
                              LN,.95f,1,.95f,1.15f, 0 };
static const float l_d[] = { LN,0,0,0,1, LN,0,0,.4f,0, AR,.4f,.5f,.5f,-90,90, LN,.4f,1,0,1, 0 };
static const float l_e[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.5f,.7f,.5f, LN,0,1,.9f,1, 0 };
static const float l_f[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.5f,.7f,.5f, 0 };
static const float l_h[] = { LN,0,0,0,1, LN,.9f,0,.9f,1, LN,0,.5f,.9f,.5f, 0 };
static const float l_i[] = { LN,0,0,0,1, 0 };
static const float l_y_uk[] = { LN,0,0,0,1, LN,.9f,0,.9f,1, LN,.9f,0,0,1, 0 };   /* И */
static const float l_k[] = { LN,0,0,0,1, LN,0,.55f,.85f,0, LN,.3f,.38f,.9f,1, 0 };
static const float l_el[] = { LN,.3f,0,.9f,0, LN,.9f,0,.9f,1, LN,.3f,0,0,1, 0 };
static const float l_m[] = { LN,0,1,0,0, LN,0,0,.45f,.55f, LN,.45f,.55f,.9f,0, LN,.9f,0,.9f,1, 0 };
static const float l_n[] = { LN,0,1,0,0, LN,0,0,.9f,1, LN,.9f,1,.9f,0, 0 };
static const float l_o[] = { AR,.5f,.5f,.5f,0,360, 0 };
static const float l_pe[] = { LN,0,1,0,0, LN,0,0,.9f,0, LN,.9f,0,.9f,1, 0 };
static const float l_p[] = { LN,0,1,0,0, LN,0,0,.65f,0, AR,.65f,.25f,.25f,-90,90, LN,.65f,.5f,0,.5f, 0 };
static const float l_r[] = { LN,0,1,0,0, LN,0,0,.65f,0, AR,.65f,.25f,.25f,-90,90, LN,.65f,.5f,0,.5f, LN,.5f,.5f,.9f,1, 0 };
static const float l_c[] = { AR,.5f,.5f,.5f,40,320, 0 };
static const float l_s[] = { LN,.9f,0,.25f,0, AR,.25f,.25f,.25f,270,90, LN,.25f,.5f,.65f,.5f, AR,.65f,.75f,.25f,-90,90,
                             LN,.65f,1,0,1, 0 };
static const float l_t[] = { LN,0,0,.9f,0, LN,.45f,0,.45f,1, 0 };
static const float l_u_uk[] = { LN,0,0,.45f,.55f, LN,.9f,0,.25f,1, 0 };   /* У */
static const float l_u[] = { LN,0,0,0,.55f, AR,.45f,.55f,.45f,180,0, LN,.9f,.55f,.9f,0, 0 };
static const float l_w[] = { LN,0,0,.275f,1, LN,.275f,1,.55f,.35f, LN,.55f,.35f,.825f,1, LN,.825f,1,1.1f,0, 0 };
static const float l_y[] = { LN,0,0,.45f,.5f, LN,.9f,0,.45f,.5f, LN,.45f,.5f,.45f,1, 0 };
static const float l_tse[] = { LN,0,0,0,1, LN,.8f,0,.8f,1, LN,0,1,.95f,1, LN,.95f,1,.95f,1.15f, 0 };
static const float l_che[] = { LN,0,0,0,.5f, LN,0,.5f,.9f,.5f, LN,.9f,0,.9f,1, 0 };
static const float l_ya[] = { LN,.9f,0,.9f,1, LN,.9f,0,.25f,0, AR,.25f,.25f,.25f,270,90, LN,.25f,.5f,.9f,.5f, LN,.4f,.5f,0,1, 0 };
static const float l_soft[] = { LN,0,0,0,1, LN,0,.45f,.625f,.45f, AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_apos[] = { LN,0,0,0,.28f, 0 };

static const struct letter letters[] =
{
    { 'A', .9f, l_a }, { 'D', .9f, l_d }, { 'E', .9f, l_e }, { 'F', .9f, l_f }, { 'H', .9f, l_h }, { 'I', 0, l_i },
    { 'M', .9f, l_m }, { 'N', .9f, l_n }, { 'O', 1, l_o }, { 'R', .9f, l_r }, { 'S', .9f, l_s }, { 'T', .9f, l_t },
    { 'U', .9f, l_u }, { 'W', 1.1f, l_w }, { 'Y', .9f, l_y }, { 'B', .9f, l_b }, { 'C', 1, l_c }, { 'K', .9f, l_k },
    { 'P', .9f, l_p }, { '\'', 0, l_apos }, { 0x2019, 0, l_apos },
    { 0x0410, .9f, l_a }, { 0x0411, .9f, l_be }, { 0x0412, .9f, l_b }, { 0x0413, .9f, l_ge }, { 0x0414, .9f, l_de },
    { 0x0415, .9f, l_e }, { 0x0406, 0, l_i }, { 0x0418, .9f, l_y_uk }, { 0x041a, .9f, l_k }, { 0x041b, .9f, l_el },
    { 0x041c, .9f, l_m }, { 0x041d, .9f, l_h }, { 0x041e, 1, l_o }, { 0x041f, .9f, l_pe }, { 0x0420, .9f, l_p },
    { 0x0421, 1, l_c }, { 0x0422, .9f, l_t }, { 0x0423, .9f, l_u_uk }, { 0x0426, .95f, l_tse }, { 0x0427, .9f, l_che },
    { 0x042f, .9f, l_ya }, { 0x042c, .9f, l_soft },
};

static const struct letter *find_letter( WCHAR ch )
{
    for (int i = 0; i < ARRAY_SIZE(letters); i++) if (letters[i].ch == ch) return &letters[i];
    return NULL;
}

static BOOL letters_cover( const WCHAR *text )
{
    for (; *text; text++) if (*text != ' ' && !find_letter( *text )) return FALSE;
    return TRUE;
}

/* the width of a word, the letters apart by gap (in heights) */
static float word_width( const WCHAR *text, float gap )
{
    float width = 0;
    int count = 0;

    for (; *text; text++)
    {
        const struct letter *l = find_letter( *text );

        width += l ? l->width : .5f;
        count++;
    }
    return count ? width + gap * (count - 1) : 0;
}

struct mask { int width, height; BYTE *bits; };

/* a stroke of round ends into the mask, each pixel by 4x4 samples */
static void mask_segment( struct mask *m, float x0, float y0, float x1, float y1, float r )
{
    int left = max( 0, (int)floorf( min( x0, x1 ) - r - 1 ) ), right = min( m->width, (int)ceilf( max( x0, x1 ) + r + 1 ) );
    int top = max( 0, (int)floorf( min( y0, y1 ) - r - 1 ) ), bottom = min( m->height, (int)ceilf( max( y0, y1 ) + r + 1 ) );
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;

    for (int y = top; y < bottom; y++)
    {
        for (int x = left; x < right; x++)
        {
            int hits = 0;
            BYTE *p = &m->bits[y * m->width + x];

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float px_ = x + (i + .5f) / 4 - x0, py_ = y + (j + .5f) / 4 - y0;
                    float t = len2 > 0 ? (px_ * dx + py_ * dy) / len2 : 0, ex, ey;

                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    ex = px_ - t * dx;
                    ey = py_ - t * dy;
                    if (ex * ex + ey * ey <= r * r) hits++;
                }
            }
            if (hits * 255 / 16 > *p) *p = hits * 255 / 16;
        }
    }
}

static void mask_word( struct mask *m, const WCHAR *text, float x, float y, float height, float gap, float stroke )
{
    for (; *text; text++)
    {
        const struct letter *l = find_letter( *text );
        const float *p;

        if (!l) { x += (.5f + gap) * height; continue; }
        for (p = l->ops; *p; )
        {
            if (*p == LN)
            {
                mask_segment( m, x + p[1] * height, y + p[2] * height, x + p[3] * height, y + p[4] * height, stroke / 2 );
                p += 5;
            }
            else
            {
                float cx = x + p[1] * height, cy = y + p[2] * height, r = p[3] * height;
                float from = p[4] * 3.14159265f / 180, to = p[5] * 3.14159265f / 180;
                int steps = max( 6, (int)(fabsf( p[5] - p[4] ) / 5) );

                for (int i = 0; i < steps; i++)
                {
                    float a = from + (to - from) * i / steps, b = from + (to - from) * (i + 1) / steps;

                    mask_segment( m, cx + r * cosf( a ), cy + r * sinf( a ), cx + r * cosf( b ), cy + r * sinf( b ), stroke / 2 );
                }
                p += 6;
            }
        }
        x += (l->width + gap) * height;
    }
}

/* the mask in white, premultiplied, over what is drawn */
static void blend_mask( HDC hdc, const struct mask *m, int x, int y, COLORREF color )
{
    UINT32 *pixels = malloc( m->width * m->height * sizeof(*pixels) );

    if (!pixels) return;
    for (int i = 0; i < m->width * m->height; i++)
    {
        UINT32 a = m->bits[i];

        pixels[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                    (GetBValue( color ) * a / 255);
    }
    blend_bits( hdc, x, y, m->width, m->height, pixels );
    free( pixels );
}

/**********************************************************************
 *          Drawing
 */

/* see-through pixels of the key (alpha 0): a veil and a frame lighter than
 * what is behind in the dark, darker in the light, the frame of the pointer */
static void paint_frame( HDC hdc, HBITMAP bitmap, const RECT *rect, float radius, int veil, int line, BOOL light )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DIBSECTION dib;
    BYTE *bits;

    GdiFlush();
    if (GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !(bits = dib.dsBm.bmBits)) return;
    for (int y = max( rect->top, 0 ); y < min( rect->bottom, abs( dib.dsBm.bmHeight ) ); y++)
    {
        BYTE *row = bits + y * dib.dsBm.bmWidthBytes;

        for (int x = max( rect->left, 0 ); x < min( rect->right, dib.dsBm.bmWidth ); x++)
        {
            int inside = 0, edge = 0, value;

            for (int j = 0; j < 3; j++)
            {
                for (int i = 0; i < 3; i++)
                {
                    float sx = x - rect->left + (i + .5f) / 3, sy = y - rect->top + (j + .5f) / 3;
                    float dx = max( max( radius - sx, sx - (width - radius) ), 0.f );
                    float dy = max( max( radius - sy, sy - (height - radius) ), 0.f );
                    float d = sqrtf( dx * dx + dy * dy );

                    if (d > radius) continue;
                    inside++;
                    /* the frame: within a pixel of the edge */
                    if (min( min( sx, width - sx ), min( sy, height - sy ) ) < 1.2f || (dx > 0 && dy > 0 && d > radius - 1.2f))
                        edge++;
                }
            }
            if (!inside) continue;
            value = (veil * (inside - edge) + line * edge) / 9;
            if (row[x * 4 + 3]) continue;   /* drawn opaque already */
            for (int c = 0; c < 3; c++)
            {
                if (light) row[x * 4 + c] = min( row[x * 4 + c], 255 - value );
                else row[x * 4 + c] = max( row[x * 4 + c], value );
            }
        }
    }
}

/* a ring of how much, from the top round to the right, its ends round */
static void paint_ring( HDC hdc, int cx, int cy, int radius, int thickness, float fraction, COLORREF track, COLORREF fill )
{
    int size = radius * 2 + 2, left = cx - radius - 1, top = cy - radius - 1;
    float mid = radius - thickness / 2.f, half = thickness / 2.f, sweep = max( 0.f, min( fraction, 1.f ) ) * 2 * 3.14159265f;
    float end_x = cosf( sweep - 3.14159265f / 2 ) * mid, end_y = sinf( sweep - 3.14159265f / 2 ) * mid;
    UINT32 *pixels = malloc( size * size * sizeof(*pixels) );

    if (!pixels) return;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int on = 0, lit = 0;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float sx = left + x + (i + .5f) / 4 - cx, sy = top + y + (j + .5f) / 4 - cy;
                    float d = sqrtf( sx * sx + sy * sy ), angle = atan2f( sx, -sy );

                    if (angle < 0) angle += 2 * 3.14159265f;
                    if (fabsf( d - mid ) <= half)
                    {
                        on++;
                        if (angle <= sweep) lit++;
                    }
                    else if (sweep > 0 && ((sx * sx + (sy + mid) * (sy + mid) <= half * half) ||
                             ((sx - end_x) * (sx - end_x) + (sy - end_y) * (sy - end_y) <= half * half)))
                        on++, lit++;
                }
            }
            {
                UINT32 a = on * 255 / 16, la = lit * 255 / 16, ta = a - min( a, la );
                UINT32 r = (GetRValue( fill ) * la + GetRValue( track ) * ta) / 255;
                UINT32 g = (GetGValue( fill ) * la + GetGValue( track ) * ta) / 255;
                UINT32 b = (GetBValue( fill ) * la + GetBValue( track ) * ta) / 255;

                pixels[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    blend_bits( hdc, left, top, size, size, pixels );
    free( pixels );
}

/* a number with the decimal point of the user's language */
static void format_decimal( WCHAR *out, int count, double value )
{
    WCHAR point[4] = L".";

    swprintf( out, count, L"%.1f", value );
    if (GetLocaleInfoEx( LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, point, ARRAY_SIZE(point) ) && point[0] != '.')
        for (WCHAR *p = out; *p; p++) if (*p == '.') *p = point[0];
}

static void format_gb( WCHAR *out, int count, double gb )
{
    WCHAR number[32];

    format_decimal( number, ARRAY_SIZE(number), gb );
    swprintf( out, count, load_string( IDS_DW_GB ), number );
}

/**********************************************************************
 *          The clock: the day, the date, the time
 */

/* Rainmeter's Mond, which this clock is: the day in Anurati at 40 points,
 * 10 pixels before and after each letter; the date and the time in
 * Quicksand Bold at 14 points; each line 5 pixels under the one above, all
 * white, in capitals, on one middle. Anurati is free for personal use only,
 * so it is not shipped: installed (for the user or in Windows' Fonts), the
 * day is in it, else in the letters drawn above. */
#define CLOCK_PAD    px( 16 )
#define DAY_POINTS   40
#define LINE_POINTS  14
#define DAY_SPACING  px( 10 )
#define LINE_GAP     px( 5 )
#define DAY_HEIGHT   px( 40 )     /* the drawn letters' capitals */
#define DAY_GAP      .8f          /* ... apart, in heights */
#define DAY_STROKE   max( 2, DAY_HEIGHT * 7 / 100 )

static BOOL have_anurati, have_quicksand;

static int CALLBACK font_found( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found )
{
    *(BOOL *)found = TRUE;
    return 0;
}

static BOOL font_exists( const WCHAR *face )
{
    LOGFONTW lf = { 0 };
    BOOL found = FALSE;
    HDC hdc = GetDC( NULL );

    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW( lf.lfFaceName, face, ARRAY_SIZE(lf.lfFaceName) );
    EnumFontFamiliesExW( hdc, &lf, font_found, (LPARAM)&found, 0 );
    ReleaseDC( NULL, hdc );
    return found;
}

/* a font installed for the user only, or put in Windows' Fonts since this
 * process started: loaded for it */
static BOOL font_load( const WCHAR *face, const WCHAR *file )
{
    WCHAR path[MAX_PATH];
    UINT len;

    if (font_exists( face )) return TRUE;
    if ((len = GetEnvironmentVariableW( L"LOCALAPPDATA", path, MAX_PATH )) && len < MAX_PATH - 64)
    {
        swprintf( path + len, MAX_PATH - len, L"\\Microsoft\\Windows\\Fonts\\%s", file );
        AddFontResourceExW( path, FR_PRIVATE, NULL );
    }
    if ((len = GetWindowsDirectoryW( path, MAX_PATH )) && len < MAX_PATH - 64)
    {
        swprintf( path + len, MAX_PATH - len, L"\\Fonts\\%s", file );
        AddFontResourceExW( path, FR_PRIVATE, NULL );
    }
    return font_exists( face );
}

static HFONT point_font( const WCHAR *face, int points, int weight )
{
    LOGFONTW lf = { 0 };

    lf.lfHeight = -MulDiv( points, px( 96 ), 72 );
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lstrcpynW( lf.lfFaceName, face, ARRAY_SIZE(lf.lfFaceName) );
    return CreateFontIndirectW( &lf );
}

static HFONT line_font(void)
{
    return have_quicksand ? point_font( L"Quicksand", LINE_POINTS, FW_BOLD ) : point_font( L"Inter", LINE_POINTS, FW_BOLD );
}

static const WCHAR *clock_locale(void)
{
    if (clock_english) return L"en-US";
    switch (PRIMARYLANGID( GetUserDefaultUILanguage() ))
    {
    case LANG_UKRAINIAN: return L"uk-UA";
    case LANG_RUSSIAN: return L"ru-RU";
    }
    return L"en-US";
}

static void day_name( const SYSTEMTIME *st, WCHAR *out, int count )
{
    const WCHAR *locale = clock_locale();

    if (!GetDateFormatEx( locale, 0, st, L"dddd", out, count, NULL )) out[0] = 0;
    CharUpperW( out );
    if (!letters_cover( out ))
    {
        GetDateFormatEx( L"en-US", 0, st, L"dddd", out, count, NULL );
        CharUpperW( out );
    }
}

/* Anurati has the Latin capitals only */
static BOOL day_in_anurati( const WCHAR *name )
{
    if (!have_anurati) return FALSE;
    for (; *name; name++) if (*name != ' ' && (*name < 'A' || *name > 'Z')) return FALSE;
    return TRUE;
}

static void date_line( const SYSTEMTIME *st, WCHAR *out, int count )
{
    WCHAR date[96];

    if (clock_english || !wcscmp( clock_locale(), L"en-US" ))
    {
        /* Mond's "%d  %B,  %Y", and the point of the picture it is known by */
        if (!GetDateFormatEx( L"en-US", 0, st, L"dd  MMMM,  yyyy", date, ARRAY_SIZE(date), NULL )) date[0] = 0;
    }
    else if (!GetDateFormatEx( clock_locale(), 0, st, L"d MMMM, yyyy", date, ARRAY_SIZE(date), NULL )) date[0] = 0;
    CharUpperW( date );
    swprintf( out, count, L"%s.", date );
}

static void time_line( const SYSTEMTIME *st, WCHAR *out, int count )
{
    WCHAR time[64];

    if (clock_english || !wcscmp( clock_locale(), L"en-US" ))
    {
        if (!GetTimeFormatEx( L"en-US", 0, st, L"hh:mm tt", time, ARRAY_SIZE(time) )) time[0] = 0;
    }
    else if (!GetTimeFormatEx( clock_locale(), TIME_NOSECONDS, st, NULL, time, ARRAY_SIZE(time) )) time[0] = 0;
    CharUpperW( time );
    swprintf( out, count, L"- %s -", time );
}

/* the day's line: its width and height, its letters drawn when hdc is given */
static void day_line( HDC hdc, const WCHAR *name, int width, int y, SIZE *size )
{
    if (day_in_anurati( name ))
    {
        HFONT font = point_font( L"Anurati", DAY_POINTS, FW_NORMAL );
        HDC screen = hdc ? NULL : GetDC( NULL ), dc = hdc ? hdc : screen;
        HGDIOBJ old = SelectObject( dc, font );
        TEXTMETRICW tm;

        SetTextCharacterExtra( dc, DAY_SPACING * 2 );
        GetTextExtentPoint32W( dc, name, lstrlenW( name ), size );
        GetTextMetricsW( dc, &tm );
        size->cy = tm.tmHeight + tm.tmExternalLeading;
        if (hdc)
        {
            SetTextColor( hdc, RGB( 255, 255, 255 ) );
            SetBkMode( hdc, TRANSPARENT );
            TextOutW( hdc, (width - size->cx) / 2 + DAY_SPACING, y, name, lstrlenW( name ) );
        }
        SetTextCharacterExtra( dc, 0 );
        SelectObject( dc, old );
        DeleteObject( font );
        if (screen) ReleaseDC( NULL, screen );
        return;
    }
    {
        struct mask m;

        m.height = DAY_HEIGHT * 115 / 100 + DAY_STROKE + 2;
        m.width = (int)(word_width( name, DAY_GAP ) * DAY_HEIGHT) + DAY_STROKE + 2;
        size->cx = m.width;
        size->cy = m.height;
        if (!hdc || !(m.bits = calloc( m.width, m.height ))) return;
        mask_word( &m, name, DAY_STROKE / 2.f + 1, DAY_STROKE / 2.f + 1, (float)DAY_HEIGHT, DAY_GAP, (float)DAY_STROKE );
        blend_mask( hdc, &m, (width - m.width) / 2, y, RGB( 255, 255, 255 ) );
        free( m.bits );
    }
}

static int text_line( HDC hdc, const WCHAR *text, int width, int y )
{
    HFONT font = line_font();
    HDC screen = hdc ? NULL : GetDC( NULL ), dc = hdc ? hdc : screen;
    HGDIOBJ old = SelectObject( dc, font );
    TEXTMETRICW tm;
    SIZE size;

    GetTextMetricsW( dc, &tm );
    GetTextExtentPoint32W( dc, text, lstrlenW( text ), &size );
    if (hdc)
    {
        SetTextColor( hdc, RGB( 255, 255, 255 ) );
        SetBkMode( hdc, TRANSPARENT );
        TextOutW( hdc, (width - size.cx) / 2, y, text, lstrlenW( text ) );
    }
    SelectObject( dc, old );
    DeleteObject( font );
    if (screen) ReleaseDC( NULL, screen );
    return hdc ? tm.tmHeight + tm.tmExternalLeading : size.cx;
}

/* wide enough for the longest day of the language */
static void clock_size( int *width, int *height )
{
    SYSTEMTIME st, day;
    FILETIME ft;
    ULARGE_INTEGER t;
    WCHAR name[64], line[96];
    int widest = 0, day_high = 0, line_high;
    TEXTMETRICW tm;
    HDC screen;
    HFONT font;
    SIZE size;

    have_anurati = font_load( L"Anurati", L"Anurati.otf" );
    have_quicksand = font_load( L"Quicksand", L"Quicksand-Bold.otf" );
    GetLocalTime( &st );
    SystemTimeToFileTime( &st, &ft );
    for (int i = 0; i < 7; i++)
    {
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart += (ULONGLONG)i * 24 * 3600 * 10000000;
        {
            FILETIME next = { t.LowPart, t.HighPart };

            FileTimeToSystemTime( &next, &day );
        }
        day_name( &day, name, ARRAY_SIZE(name) );
        day_line( NULL, name, 0, 0, &size );
        widest = max( widest, size.cx );
        day_high = max( day_high, size.cy );
    }
    date_line( &st, line, ARRAY_SIZE(line) );
    widest = max( widest, text_line( NULL, line, 0, 0 ) + px( 80 ) );

    screen = GetDC( NULL );
    font = line_font();
    SelectObject( screen, font );
    GetTextMetricsW( screen, &tm );
    line_high = tm.tmHeight + tm.tmExternalLeading;
    SelectObject( screen, GetStockObject( SYSTEM_FONT ) );
    DeleteObject( font );
    ReleaseDC( NULL, screen );

    *width = widest + CLOCK_PAD * 2;
    *height = CLOCK_PAD + day_high + LINE_GAP + line_high + LINE_GAP + line_high + CLOCK_PAD;
}

static void paint_clock( struct gadget *g, HDC hdc, HBITMAP bitmap )
{
    WCHAR name[64], line[96];
    SIZE size;
    RECT r;
    int y = CLOCK_PAD;

    GetLocalTime( &shown_time );
    if (g->hot || g->pressed)
    {
        SetRect( &r, 0, 0, g->width, g->height );
        paint_frame( hdc, bitmap, &r, (float)px( 8 ), g->pressed ? 0x30 : 0x20, g->pressed ? 0xa0 : 0x78, FALSE );
    }
    day_name( &shown_time, name, ARRAY_SIZE(name) );
    day_line( hdc, name, g->width, y, &size );
    y += size.cy + LINE_GAP;
    date_line( &shown_time, line, ARRAY_SIZE(line) );
    y += text_line( hdc, line, g->width, y ) + LINE_GAP;
    time_line( &shown_time, line, ARRAY_SIZE(line) );
    text_line( hdc, line, g->width, y );
}

/**********************************************************************
 *          The monitoring and the battery: cards of rings
 */

#define CARD_W     px( 300 )
#define CARD_PAD   px( 14 )
#define ROW_H      px( 52 )
#define RING_R     px( 21 )

struct row
{
    float fraction;
    WCHAR center[16];
    WCHAR title[64];
    WCHAR detail[96];
    COLORREF color;
};

static int monitor_rows( struct row *rows )
{
    int count = 0;
    WCHAR a[32], b[32];

    if (monitor_items & ITEM_CPU)
    {
        struct row *r = &rows[count++];
        UINT32 load = have_sensors ? sensors.cpu_load_percent : ARCTIC_UNKNOWN;

        memset( r, 0, sizeof(*r) );
        r->fraction = load != ARCTIC_UNKNOWN ? load / 100.f : 0;
        if (load != ARCTIC_UNKNOWN) swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", load );
        lstrcpynW( r->title, load_string( IDS_DW_CPU ), ARRAY_SIZE(r->title) );
        if (have_sensors && sensors.cpu_temperature_mc != ARCTIC_UNKNOWN)
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%u \x00b0C", (sensors.cpu_temperature_mc + 500) / 1000 );
        if (have_sensors && sensors.cpu_mhz != ARCTIC_UNKNOWN && sensors.cpu_mhz)
        {
            format_decimal( a, ARRAY_SIZE(a), sensors.cpu_mhz / 1000.0 );
            swprintf( b, ARRAY_SIZE(b), load_string( IDS_DW_GHZ ), a );
            if (r->detail[0]) wcscat( r->detail, L"  \x00b7  " );
            wcscat( r->detail, b );
        }
    }
    if (monitor_items & ITEM_GPU)
    {
        struct row *r = &rows[count++];
        UINT32 load = have_sensors ? sensors.gpu_load_percent : ARCTIC_UNKNOWN;

        memset( r, 0, sizeof(*r) );
        r->fraction = load != ARCTIC_UNKNOWN ? load / 100.f : 0;
        if (load != ARCTIC_UNKNOWN) swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", load );
        lstrcpynW( r->title, have_sensors && sensors.gpu_name[0] ? sensors.gpu_name : load_string( IDS_DW_GPU ), ARRAY_SIZE(r->title) );
        if (have_sensors && sensors.gpu_temperature_mc != ARCTIC_UNKNOWN)
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%u \x00b0C", (sensors.gpu_temperature_mc + 500) / 1000 );
        if (have_sensors && sensors.gpu_mhz != ARCTIC_UNKNOWN && sensors.gpu_mhz)
        {
            swprintf( b, ARRAY_SIZE(b), load_string( IDS_DW_MHZ ), sensors.gpu_mhz );
            if (r->detail[0]) wcscat( r->detail, L"  \x00b7  " );
            wcscat( r->detail, b );
        }
    }
    if (monitor_items & ITEM_RAM)
    {
        struct row *r = &rows[count++];
        MEMORYSTATUSEX mem = { sizeof(mem) };

        memset( r, 0, sizeof(*r) );
        lstrcpynW( r->title, load_string( IDS_DW_RAM ), ARRAY_SIZE(r->title) );
        if (GlobalMemoryStatusEx( &mem ) && mem.ullTotalPhys)
        {
            double total = mem.ullTotalPhys / 1073741824.0, used = (mem.ullTotalPhys - mem.ullAvailPhys) / 1073741824.0;

            r->fraction = (float)(used / total);
            swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", (UINT)(r->fraction * 100 + .5f) );
            format_decimal( a, ARRAY_SIZE(a), used );
            format_gb( b, ARRAY_SIZE(b), total );
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%s / %s", a, b );
        }
    }
    if (monitor_items & ITEM_DISK)
    {
        struct row *r = &rows[count++];
        ULARGE_INTEGER avail, total, free_bytes;

        memset( r, 0, sizeof(*r) );
        lstrcpynW( r->title, load_string( IDS_DW_DISK ), ARRAY_SIZE(r->title) );
        if (GetDiskFreeSpaceExW( L"C:\\", &avail, &total, &free_bytes ) && total.QuadPart)
        {
            r->fraction = (float)(1.0 - (double)free_bytes.QuadPart / total.QuadPart);
            swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", (UINT)(r->fraction * 100 + .5f) );
            format_gb( a, ARRAY_SIZE(a), free_bytes.QuadPart / 1073741824.0 );
            format_gb( b, ARRAY_SIZE(b), total.QuadPart / 1073741824.0 );
            swprintf( r->detail, ARRAY_SIZE(r->detail), load_string( IDS_DW_FREE ), a, b );
        }
    }
    return count;
}

static int battery_rows( struct row *row )
{
    SYSTEM_POWER_STATUS status;

    memset( row, 0, sizeof(*row) );
    lstrcpynW( row->title, load_string( IDS_DW_BATTERY ), ARRAY_SIZE(row->title) );
    if (!GetSystemPowerStatus( &status ) || (status.BatteryFlag & 128) || status.BatteryLifePercent > 100)
    {
        lstrcpynW( row->detail, load_string( IDS_DW_NO_BATTERY ), ARRAY_SIZE(row->detail) );
        return 1;
    }
    row->fraction = status.BatteryLifePercent / 100.f;
    swprintf( row->center, ARRAY_SIZE(row->center), L"%u%%", status.BatteryLifePercent );
    if (status.BatteryFlag & 8)
    {
        lstrcpynW( row->detail, load_string( IDS_DW_CHARGING ), ARRAY_SIZE(row->detail) );
        row->color = RGB( 0x6c, 0xcb, 0x5f );
    }
    else if (status.ACLineStatus == 1)
        lstrcpynW( row->detail, load_string( IDS_DW_PLUGGED ), ARRAY_SIZE(row->detail) );
    else if (status.BatteryLifeTime != (DWORD)-1)
        swprintf( row->detail, ARRAY_SIZE(row->detail), load_string( IDS_DW_REMAINING ),
                  status.BatteryLifeTime / 3600, status.BatteryLifeTime / 60 % 60 );
    else
        lstrcpynW( row->detail, load_string( IDS_DW_ON_BATTERY ), ARRAY_SIZE(row->detail) );
    if (status.ACLineStatus != 1 && status.BatteryLifePercent <= 20) row->color = RGB( 0xf7, 0x63, 0x0c );
    return 1;
}

static int card_height( int rows )
{
    return CARD_PAD * 2 + max( rows, 1 ) * ROW_H;
}

static void paint_card( struct gadget *g, HDC hdc, HBITMAP bitmap, const struct palette *pal, const struct row *rows, int count )
{
    HFONT title = shell_font( px( 14 ), FW_SEMIBOLD ), detail_font = shell_font( px( 12 ), FW_NORMAL );
    HFONT ring_font = shell_font( px( 11 ), FW_SEMIBOLD );
    COLORREF accent = accent_color(), track = pal->light ? RGB( 0xd8, 0xd8, 0xd8 ) : RGB( 0x44, 0x44, 0x44 );
    RECT r;

    if (g->hot || g->pressed)
    {
        SetRect( &r, 0, 0, g->width, g->height );
        paint_frame( hdc, bitmap, &r, (float)px( 8 ), g->pressed ? 0x18 : 0x0c, g->pressed ? 0x90 : 0x68, pal->light );
    }
    for (int i = 0; i < count; i++)
    {
        int top = CARD_PAD + i * ROW_H, cx = CARD_PAD + RING_R, cy = top + ROW_H / 2, x = CARD_PAD + RING_R * 2 + px( 14 );

        paint_ring( hdc, cx, cy, RING_R, px( 4 ), rows[i].fraction, track, rows[i].color ? rows[i].color : accent );
        SetRect( &r, cx - RING_R, cy - RING_R, cx + RING_R, cy + RING_R );
        paint_text( hdc, rows[i].center, &r, ring_font, pal->text, DT_CENTER );
        SetRect( &r, x, cy - px( 19 ), g->width - CARD_PAD, cy );
        paint_text( hdc, rows[i].title, &r, title, pal->text, DT_LEFT | DT_END_ELLIPSIS );
        SetRect( &r, x, cy, g->width - CARD_PAD, cy + px( 18 ) );
        paint_text( hdc, rows[i].detail, &r, detail_font, pal->dim, DT_LEFT | DT_END_ELLIPSIS );
    }
    DeleteObject( title );
    DeleteObject( detail_font );
    DeleteObject( ring_font );
}

/**********************************************************************
 *          The windows
 */

static enum gadget_kind gadget_of( HWND hwnd )
{
    for (int i = 0; i < G_COUNT; i++) if (gadgets[i].hwnd == hwnd) return i;
    return G_COUNT;
}

/* its look: when it comes, and when the mode changes */
static void gadget_look( enum gadget_kind kind )
{
    struct look look = { 0 };
    DWORD corner;

    if (kind == G_CLOCK)
    {
        /* nothing behind the letters: white on what the desktop shows */
        look.effect = ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT;
        look.color = RGB( 0, 0, 0 );
        look.opacity = 0;
        look.blur = 1;
        look.text = TEXT_WHITE;
        corner = 1;   /* DWMWCP_DONOTROUND: no shadow either */
    }
    else
    {
        BOOL mode_light = system_mode_light();

        look.effect = ARCTIC_ACCENT_ENABLE_BLURBEHIND;
        look.color = mode_light ? RGB( 0xf3, 0xf3, 0xf3 ) : RGB( 0x1c, 0x1c, 0x1c );
        look.opacity = 62;
        look.blur = 70;
        look.text = mode_light ? TEXT_BLACK : TEXT_WHITE;
        corner = ARCTIC_FLYOUT_CORNER;
    }
    DwmSetWindowAttribute( gadgets[kind].hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    gadgets[kind].light = look_apply( gadgets[kind].hwnd, &look );
}

static void gadget_paint( HWND hwnd )
{
    enum gadget_kind kind = gadget_of( hwnd );
    struct gadget *g;
    struct palette pal;
    struct row rows[4];
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    HDC mem;

    if (kind == G_COUNT) return;
    g = &gadgets[kind];
    palette_init( &pal, g->light );
    if (!(mem = paint_begin( hwnd, &ps, &pal, &bitmap, &old ))) return;
    if (kind == G_CLOCK) paint_clock( g, mem, bitmap );
    else if (kind == G_MONITOR) paint_card( g, mem, bitmap, &pal, rows, monitor_rows( rows ) );
    else paint_card( g, mem, bitmap, &pal, rows, battery_rows( rows ) );
    paint_end( hwnd, &ps, mem, bitmap, old );
}

static void gadget_size( enum gadget_kind kind, int *width, int *height )
{
    struct row rows[4];

    switch (kind)
    {
    case G_CLOCK:
        clock_size( width, height );
        break;
    case G_MONITOR:
        *width = CARD_W;
        *height = card_height( monitor_rows( rows ) );
        break;
    default:
        *width = CARD_W;
        *height = card_height( 1 );
        break;
    }
}

/* where it is when it was never dragged: the clock above the middle, the
 * cards in the top right corner, the battery under the monitoring */
static void default_position( enum gadget_kind kind, int width, int *x, int *y )
{
    MONITORINFO info = { sizeof(info) };
    POINT origin = { 0, 0 };

    GetMonitorInfoW( MonitorFromPoint( origin, MONITOR_DEFAULTTOPRIMARY ), &info );
    if (kind == G_CLOCK)
    {
        *x = (info.rcWork.left + info.rcWork.right - width) / 2;
        *y = info.rcWork.top + px( 48 );
        return;
    }
    *x = info.rcWork.right - width - px( 24 );
    *y = info.rcWork.top + px( 24 );
    if (kind == G_BATTERY && gadgets[G_MONITOR].hwnd) *y += gadgets[G_MONITOR].height + px( 12 );
}

static void position_name( enum gadget_kind kind, WCHAR *x, WCHAR *y )
{
    swprintf( x, 32, L"%sX", gadget_names[kind] );
    swprintf( y, 32, L"%sY", gadget_names[kind] );
}

static void gadget_place( enum gadget_kind kind )
{
    struct gadget *g = &gadgets[kind];
    WCHAR name_x[32], name_y[32];
    DWORD saved_x, saved_y;
    int x, y;
    RECT virt, r;

    gadget_size( kind, &g->width, &g->height );
    position_name( kind, name_x, name_y );
    if (find_value( name_x, &saved_x ) && find_value( name_y, &saved_y ))
    {
        x = (int)saved_x;
        y = (int)saved_y;
        if (kind == G_CLOCK) x -= g->width / 2;   /* the clock keeps its middle: the days are not as long */
    }
    else default_position( kind, g->width, &x, &y );

    /* not lost off the screens */
    SetRect( &virt, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
             GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ),
             GetSystemMetrics( SM_YVIRTUALSCREEN ) + GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    SetRect( &r, x, y, x + g->width, y + g->height );
    if (r.right < virt.left + px( 40 ) || r.left > virt.right - px( 40 ) || r.top < virt.top || r.top > virt.bottom - px( 40 ))
        default_position( kind, g->width, &x, &y );
    SetWindowPos( g->hwnd, HWND_BOTTOM, x, y, g->width, g->height, SWP_NOACTIVATE );
}

static void gadget_save( enum gadget_kind kind )
{
    WCHAR name_x[32], name_y[32];
    RECT r;

    if (!GetWindowRect( gadgets[kind].hwnd, &r )) return;
    position_name( kind, name_x, name_y );
    set_value( name_x, kind == G_CLOCK ? (r.left + r.right) / 2 : r.left );
    set_value( name_y, r.top );
}

static void gadgets_reload(void);

static void gadget_menu( HWND hwnd, POINT pt )
{
    enum gadget_kind kind = gadget_of( hwnd );
    HMENU menu = CreatePopupMenu();
    UINT cmd;

    AppendMenuW( menu, MF_STRING, ID_SETTINGS, load_string( IDS_DW_SETTINGS ) );
    AppendMenuW( menu, MF_STRING, ID_REMOVE, load_string( IDS_DW_REMOVE ) );
    SetForegroundWindow( hwnd );
    cmd = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL );
    DestroyMenu( menu );
    if (cmd == ID_REMOVE && kind < G_COUNT)
    {
        set_value( gadget_names[kind], 0 );
        gadgets_reload();
    }
    else if (cmd == ID_SETTINGS) ArcticDesktopWidgetsDialog( NULL );
}

static LRESULT CALLBACK gadget_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    enum gadget_kind kind = gadget_of( hwnd );
    struct gadget *g = kind < G_COUNT ? &gadgets[kind] : NULL;

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_WINDOWPOSCHANGING:
    {
        /* above the desktop, under every program: win32u keeps a popup over its owner */
        WINDOWPOS *wp = (WINDOWPOS *)lparam;

        if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = HWND_BOTTOM;
        break;
    }

    case WM_MOUSEMOVE:
        if (g && !g->hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            g->hot = TRUE;
            TrackMouseEvent( &tme );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSELEAVE:
        if (g && g->hot && !g->pressed)
        {
            g->hot = FALSE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (!g) break;
        g->pressed = TRUE;
        RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
        /* dragged by its frame: the system moves it, from where the button went down */
        SendMessageW( hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, GetMessagePos() );
        g->pressed = FALSE;
        {
            POINT pt;
            RECT r;

            GetCursorPos( &pt );
            GetWindowRect( hwnd, &r );
            g->hot = PtInRect( &r, pt );
        }
        gadget_save( kind );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_RBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

        ClientToScreen( hwnd, &pt );
        gadget_menu( hwnd, pt );
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        gadget_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void gadget_create( enum gadget_kind kind )
{
    static const WCHAR class_name[] = L"ArcticDesktopWidget";
    static BOOL registered;
    HWND owner = GetShellWindow();
    DWORD transition = ARCTIC_TRANSITION_FADE;

    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.lpfnWndProc = gadget_proc;
        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        cls.lpszClassName = class_name;
        registered = RegisterClassW( &cls ) != 0;
    }
    if (!owner) owner = FindWindowW( L"Progman", NULL );
    gadgets[kind].hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"", WS_POPUP, 0, 0, 1, 1, owner,
                                          NULL, startui_instance, NULL );
    if (!gadgets[kind].hwnd) return;
    DwmSetWindowAttribute( gadgets[kind].hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    gadget_look( kind );
    gadget_place( kind );
    ShowWindow( gadgets[kind].hwnd, SW_SHOWNOACTIVATE );
}

static void read_sensors(void)
{
    have_sensors = ArcticSensors( &sensors );
    ArcticSensors( NULL );   /* NVIDIA's library goes: an open one keeps the card awake */
}

static void gadgets_reload(void)
{
    monitor_items = get_value( L"MonitorItems", ITEM_ALL ) & ITEM_ALL;
    if (!monitor_items) monitor_items = ITEM_ALL;
    clock_english = get_value( L"ClockEnglish", 1 );
    if (get_value( gadget_names[G_MONITOR], gadget_default[G_MONITOR] )) read_sensors();
    for (int i = 0; i < G_COUNT; i++)
    {
        BOOL on = get_value( gadget_names[i], gadget_default[i] ) != 0;

        if (!on && gadgets[i].hwnd)
        {
            DestroyWindow( gadgets[i].hwnd );
            memset( &gadgets[i], 0, sizeof(gadgets[i]) );
        }
        else if (on && !gadgets[i].hwnd) gadget_create( i );
        else if (on)
        {
            gadget_place( i );
            InvalidateRect( gadgets[i].hwnd, NULL, FALSE );
        }
    }
}

static BOOL CALLBACK repaint_window( HWND hwnd, LPARAM pid )
{
    DWORD owner;

    GetWindowThreadProcessId( hwnd, &owner );
    if (owner == (DWORD)pid) RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
    return TRUE;
}

/* another icon theme: shell32 notices it at its next look-up (half a second
 * after the last at most) and makes the icons it keeps again; this process's
 * windows (the desktop, the taskbar, File Explorer) paint them */
static void icons_changed(void)
{
    SHFILEINFOW info;

    SHGetFileInfoW( L"C:\\", 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON );
    SHChangeNotify( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );
    EnumWindows( repaint_window, GetCurrentProcessId() );
}

static LRESULT CALLBACK manager_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_SETTINGCHANGE:
        if (lparam && !lstrcmpiW( (const WCHAR *)lparam, GADGETS_CHANGED )) gadgets_reload();
        else if (lparam && !lstrcmpiW( (const WCHAR *)lparam, ICONS_CHANGED )) SetTimer( hwnd, TIMER_ICONS, 700, NULL );
        else for (int i = 0; i < G_COUNT; i++)
        {
            /* the mode, the accent: the cards follow */
            if (!gadgets[i].hwnd) continue;
            gadget_look( i );
            InvalidateRect( gadgets[i].hwnd, NULL, FALSE );
        }
        return 0;

    case WM_DISPLAYCHANGE:
        for (int i = 0; i < G_COUNT; i++) if (gadgets[i].hwnd) gadget_place( i );
        return 0;

    case WM_POWERBROADCAST:
        if (gadgets[G_BATTERY].hwnd) InvalidateRect( gadgets[G_BATTERY].hwnd, NULL, FALSE );
        return TRUE;

    case WM_TIMER:
    {
        SYSTEMTIME now;

        if (wparam == TIMER_ICONS)
        {
            KillTimer( hwnd, TIMER_ICONS );
            icons_changed();
            return 0;
        }
        ticks++;
        GetLocalTime( &now );
        if (gadgets[G_CLOCK].hwnd && (now.wMinute != shown_time.wMinute || now.wDay != shown_time.wDay))
            InvalidateRect( gadgets[G_CLOCK].hwnd, NULL, FALSE );
        if (gadgets[G_MONITOR].hwnd && !(ticks % 2))
        {
            read_sensors();
            InvalidateRect( gadgets[G_MONITOR].hwnd, NULL, FALSE );
        }
        if (gadgets[G_BATTERY].hwnd && !(ticks % 10)) InvalidateRect( gadgets[G_BATTERY].hwnd, NULL, FALSE );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static DWORD WINAPI gadgets_thread( void *arg )
{
    WNDCLASSW cls = { 0 };
    MSG msg;

    /* the desktop's window first: the widgets stay over it */
    for (int i = 0; i < 100 && !GetShellWindow() && !FindWindowW( L"Progman", NULL ); i++) Sleep( 100 );
    cls.lpfnWndProc = manager_proc;
    cls.hInstance = startui_instance;
    cls.lpszClassName = L"ArcticDesktopWidgets";
    RegisterClassW( &cls );
    /* a hidden window of its own: a broadcast reaches it */
    manager = CreateWindowExW( WS_EX_TOOLWINDOW, cls.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, startui_instance, NULL );
    if (!manager) return 0;
    gadgets_reload();
    SetTimer( manager, TIMER_TICK, 1000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

/* explorer's taskbar came: the widgets of the desktop start, once */
void gadgets_start(void)
{
    static LONG started;
    HANDLE thread;

    if (InterlockedExchange( &started, 1 )) return;
    if ((thread = CreateThread( NULL, 0, gadgets_thread, NULL, 0, NULL ))) CloseHandle( thread );
}

/**********************************************************************
 *          The settings: Display's "Widgets..."
 */

enum
{
    IDC_DW_CLOCK = 100, IDC_DW_LANGUAGE, IDC_DW_MONITOR, IDC_DW_CPU, IDC_DW_GPU, IDC_DW_RAM, IDC_DW_DISK,
    IDC_DW_BATTERY, IDC_DW_RESET,
};

struct dialog
{
    HWND hwnd;
    BOOL done, ok, reset;
};

static void dialog_enable( HWND hwnd )
{
    BOOL monitor = IsDlgButtonChecked( hwnd, IDC_DW_MONITOR ) == BST_CHECKED;

    EnableWindow( GetDlgItem( hwnd, IDC_DW_LANGUAGE ), IsDlgButtonChecked( hwnd, IDC_DW_CLOCK ) == BST_CHECKED );
    for (int id = IDC_DW_CPU; id <= IDC_DW_DISK; id++) EnableWindow( GetDlgItem( hwnd, id ), monitor );
}

static LRESULT CALLBACK dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct dialog *d = (struct dialog *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        return 0;

    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case IDOK: d->ok = TRUE; d->done = TRUE; return 0;
        case IDCANCEL: d->done = TRUE; return 0;
        case IDC_DW_RESET: d->reset = TRUE; EnableWindow( (HWND)lparam, FALSE ); return 0;
        case IDC_DW_CLOCK: case IDC_DW_MONITOR: dialog_enable( hwnd ); return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wparam, GetSysColor( COLOR_BTNFACE ) );
        SetTextColor( (HDC)wparam, GetSysColor( COLOR_BTNTEXT ) );
        return (LRESULT)GetSysColorBrush( COLOR_BTNFACE );

    case WM_CLOSE:
        if (d) d->done = TRUE;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static HWND dialog_control( HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int id, int x, int y, int w, int h,
                            HFONT font )
{
    HWND control = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, px( x ), px( y ), px( w ), px( h ), parent,
                                    (HMENU)(INT_PTR)id, startui_instance, NULL );

    SendMessageW( control, WM_SETFONT, (WPARAM)font, FALSE );
    return control;
}

void WINAPI ArcticDesktopWidgetsDialog( HWND parent )
{
    static const WCHAR class_name[] = L"ArcticDesktopWidgetsDialog";
    static const struct { int id; DWORD bit; } items[] =
    {
        { IDC_DW_CPU, ITEM_CPU }, { IDC_DW_GPU, ITEM_GPU }, { IDC_DW_RAM, ITEM_RAM }, { IDC_DW_DISK, ITEM_DISK },
    };
    static const UINT item_names[] = { IDS_DW_CPU, IDS_DW_GPU, IDS_DW_RAM, IDS_DW_DISK };
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    struct dialog d = { 0 };
    WNDCLASSW cls = { 0 };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, items_on = get_value( L"MonitorItems", ITEM_ALL );
    RECT r = { 0, 0, px( 440 ), px( 236 ) }, owner_rect;
    HWND combo, focus;
    HFONT font;
    MSG msg;

    /* a page of a property sheet: the sheet waits */
    if (parent) parent = GetAncestor( parent, GA_ROOT );
    cls.lpfnWndProc = dialog_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    cls.hbrBackground = GetSysColorBrush( COLOR_BTNFACE );
    cls.lpszClassName = class_name;
    RegisterClassW( &cls );
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    font = CreateFontIndirectW( &metrics.lfMessageFont );

    AdjustWindowRectEx( &r, style, FALSE, WS_EX_DLGMODALFRAME );
    if (!parent || !GetWindowRect( parent, &owner_rect ))
        SetRect( &owner_rect, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    d.hwnd = CreateWindowExW( WS_EX_DLGMODALFRAME, class_name, load_string( IDS_DW_TITLE ), style,
                              (owner_rect.left + owner_rect.right - (r.right - r.left)) / 2,
                              (owner_rect.top + owner_rect.bottom - (r.bottom - r.top)) / 2,
                              r.right - r.left, r.bottom - r.top, parent, NULL, startui_instance, &d );
    if (!d.hwnd)
    {
        DeleteObject( font );
        return;
    }

    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_CLOCK ), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DW_CLOCK, 16, 16, 200, 24, font );
    combo = dialog_control( d.hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_DW_LANGUAGE, 240, 15, 184, 200, font );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_DW_LANG_SYSTEM ) );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)L"English" );
    SendMessageW( combo, CB_SETCURSEL, get_value( L"ClockEnglish", 1 ) ? 1 : 0, 0 );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_MONITOR ), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DW_MONITOR, 16, 52, 200, 24, font );
    for (int i = 0; i < ARRAY_SIZE(items); i++)
    {
        HWND check = dialog_control( d.hwnd, L"BUTTON", load_string( item_names[i] ), BS_AUTOCHECKBOX | WS_TABSTOP, items[i].id,
                                     40 + (i % 2) * 180, 82 + (i / 2) * 28, 170, 24, font );

        SendMessageW( check, BM_SETCHECK, (items_on & items[i].bit) ? BST_CHECKED : BST_UNCHECKED, 0 );
    }
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_BATTERY ), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DW_BATTERY, 16, 148, 200, 24, font );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_RESET ), BS_PUSHBUTTON | WS_TABSTOP, IDC_DW_RESET, 16, 196, 180, 26, font );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_OK ), BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 236, 196, 90, 26, font );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 334, 196, 90, 26, font );
    CheckDlgButton( d.hwnd, IDC_DW_CLOCK, get_value( gadget_names[G_CLOCK], gadget_default[G_CLOCK] ) ? BST_CHECKED : BST_UNCHECKED );
    CheckDlgButton( d.hwnd, IDC_DW_MONITOR, get_value( gadget_names[G_MONITOR], gadget_default[G_MONITOR] ) ? BST_CHECKED : BST_UNCHECKED );
    CheckDlgButton( d.hwnd, IDC_DW_BATTERY, get_value( gadget_names[G_BATTERY], gadget_default[G_BATTERY] ) ? BST_CHECKED : BST_UNCHECKED );
    dialog_enable( d.hwnd );

    focus = GetFocus();
    if (parent) EnableWindow( parent, FALSE );
    ShowWindow( d.hwnd, SW_SHOW );
    SetFocus( GetDlgItem( d.hwnd, IDC_DW_CLOCK ) );
    while (!d.done && GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (IsDialogMessageW( d.hwnd, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if (d.ok)
    {
        DWORD bits = 0;

        set_value( gadget_names[G_CLOCK], IsDlgButtonChecked( d.hwnd, IDC_DW_CLOCK ) == BST_CHECKED );
        set_value( gadget_names[G_MONITOR], IsDlgButtonChecked( d.hwnd, IDC_DW_MONITOR ) == BST_CHECKED );
        set_value( gadget_names[G_BATTERY], IsDlgButtonChecked( d.hwnd, IDC_DW_BATTERY ) == BST_CHECKED );
        for (int i = 0; i < ARRAY_SIZE(items); i++)
            if (IsDlgButtonChecked( d.hwnd, items[i].id ) == BST_CHECKED) bits |= items[i].bit;
        set_value( L"MonitorItems", bits ? bits : ITEM_ALL );
        set_value( L"ClockEnglish", SendMessageW( combo, CB_GETCURSEL, 0, 0 ) == 1 );
        if (d.reset)
        {
            WCHAR name_x[32], name_y[32];

            for (int i = 0; i < G_COUNT; i++)
            {
                position_name( i, name_x, name_y );
                delete_value( name_x );
                delete_value( name_y );
            }
        }
        SendMessageTimeoutW( HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)GADGETS_CHANGED, SMTO_ABORTIFHUNG, 2000, NULL );
    }
    if (parent)
    {
        EnableWindow( parent, TRUE );
        SetActiveWindow( parent );
    }
    DestroyWindow( d.hwnd );
    DeleteObject( font );
    if (focus && IsWindow( focus )) SetFocus( focus );
}
