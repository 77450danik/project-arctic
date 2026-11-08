/*
 * What the taskbar's clock opens: the notification center and the calendar
 * of Windows 11
 *
 * As Windows 11 (22H2) lays them out, two panels 360 wide at 100%, their
 * corners rounded by 8, 12 from the taskbar and from the screen's right edge,
 * 12 between them:
 *
 * - the notification center: "Сповіщення" with "Очистити все" and the bell
 *   of Do not disturb on its right; the notifications the shell showed,
 *   the newest first, each on a card with its program's icon, name and time,
 *   its title and text. A click on a card is the program's (as a click on
 *   the toast), its X takes it away. With none: "Немає нових сповіщень";
 * - the calendar: today's date, which opens the Date and Time page, and the
 *   chevron that folds the month away; the month with its arrows, the days
 *   of the week from the locale's first one, six weeks of days, today on a
 *   disc of the accent colour. The wheel turns the months, a click on the
 *   month's name comes back to today's.
 *
 * explorer hands every notification it shows over (ArcticNotificationAdd),
 * and does not show them while Do not disturb is on (ArcticNotificationsQuiet),
 * but they still come here. They are kept in
 * HKCU\Software\Arctic\Notifications (History), what a click does only for
 * as long as the program that sent it runs. Both panels take the taskbar's
 * look (the page Оформлення), as the other flyouts.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <stdio.h>

#include "startui.h"
#include "shellapi.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define WIDTH         px(360)
#define MARGIN        px(12)     /* from the taskbar, the screen's edge and between the panels */
#define PAD           px(16)
#define HEADER        px(48)
#define BUTTON        px(32)
#define CELL          px(40)
#define WEEK_ROW      px(32)
#define CARD_INSET    px(12)
#define CARD_GAP      px(8)
#define CARD_PAD      px(12)
#define SMALL_ICON    px(16)
#define GLYPH         px(16)
#define EMPTY_HEIGHT  px(100)
#define TEXT_LINES    4

#define HOVER_ALPHA   0x14
#define PRESSED_ALPHA 0x0c
#define CARD_ALPHA    0x10
#define LINE_ALPHA    0x24

#define MAX_NOTES     50
#define MAX_TARGETS   (MAX_NOTES * 2 + 64)

#define TIMER_TICK    1

#define NOTES_KEY     L"Software\\Arctic\\Notifications"
#define CLOCK_KEY     L"Software\\Arctic\\Clock"

enum { CENTER, CALENDAR, PANEL_COUNT };

enum kind { K_NONE, K_CLEAR, K_BELL, K_CARD, K_CARD_CLOSE, K_DATE, K_COLLAPSE, K_MONTH, K_PREV, K_NEXT, K_DAY };

struct target
{
    enum kind kind;
    int index;
    RECT rect;
};

struct panel
{
    HWND hwnd;
    int height;
    int content;              /* the notification center: the height of all its cards */
    int scroll;
    struct target targets[MAX_TARGETS];
    int target_count;
    struct target hot, pressed;
};

struct note
{
    FILETIME time;
    WCHAR app[64];
    WCHAR icon[MAX_PATH];     /* the file its program's icon comes from */
    WCHAR title[64];
    WCHAR text[256];
    /* what a click on it posts, while the program that sent it runs */
    HWND reply;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    HICON hicon;
    HBITMAP bitmap;           /* the icon, premultiplied, once drawn */
    int height;               /* of its card, once laid out */
};

/* as it is kept in the registry */
struct stored_note
{
    FILETIME time;
    WCHAR app[64];
    WCHAR icon[MAX_PATH];
    WCHAR title[64];
    WCHAR text[256];
};

static struct panel panels[PANEL_COUNT];
static HWND tray;
static RECT clock_rect;
static DWORD hidden_at;
static struct palette pal;
static struct look look;
static HFONT font_title, font_body, font_small;
static BOOL created, collapsed;

static struct note notes[MAX_NOTES];
static int note_count;
static BOOL notes_loaded;

static SYSTEMTIME today;
static int view_year, view_month;          /* the month the calendar shows */
static int sel_year, sel_month, sel_day;   /* the day clicked, 0 for none */

/**********************************************************************
 *          The notifications kept
 */

static DWORD read_dword( const WCHAR *key, const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

static void write_dword( const WCHAR *key_name, const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, key_name, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static void notes_load(void)
{
    struct stored_note *stored;
    DWORD size = 0;

    if (notes_loaded) return;
    notes_loaded = TRUE;
    if (RegGetValueW( HKEY_CURRENT_USER, NOTES_KEY, L"History", RRF_RT_REG_BINARY, NULL, NULL, &size ) || !size)
        return;
    if (!(stored = malloc( size ))) return;
    if (!RegGetValueW( HKEY_CURRENT_USER, NOTES_KEY, L"History", RRF_RT_REG_BINARY, NULL, stored, &size ))
    {
        for (DWORD i = 0; i < size / sizeof(*stored) && note_count < MAX_NOTES; i++)
        {
            struct note *note = &notes[note_count++];

            memset( note, 0, sizeof(*note) );
            note->time = stored[i].time;
            lstrcpynW( note->app, stored[i].app, ARRAY_SIZE(note->app) );
            lstrcpynW( note->icon, stored[i].icon, ARRAY_SIZE(note->icon) );
            lstrcpynW( note->title, stored[i].title, ARRAY_SIZE(note->title) );
            lstrcpynW( note->text, stored[i].text, ARRAY_SIZE(note->text) );
        }
    }
    free( stored );
}

static void notes_save(void)
{
    struct stored_note *stored = calloc( max( note_count, 1 ), sizeof(*stored) );
    HKEY key;

    if (!stored) return;
    for (int i = 0; i < note_count; i++)
    {
        stored[i].time = notes[i].time;
        lstrcpynW( stored[i].app, notes[i].app, ARRAY_SIZE(stored[i].app) );
        lstrcpynW( stored[i].icon, notes[i].icon, ARRAY_SIZE(stored[i].icon) );
        lstrcpynW( stored[i].title, notes[i].title, ARRAY_SIZE(stored[i].title) );
        lstrcpynW( stored[i].text, notes[i].text, ARRAY_SIZE(stored[i].text) );
    }
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, NOTES_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"History", 0, REG_BINARY, (const BYTE *)stored, note_count * sizeof(*stored) );
        RegCloseKey( key );
    }
    free( stored );
}

static void note_free( struct note *note )
{
    if (note->bitmap) DeleteObject( note->bitmap );
    if (note->hicon) DestroyIcon( note->hicon );
    note->bitmap = NULL;
    note->hicon = NULL;
}

static void note_remove( int index )
{
    if (index < 0 || index >= note_count) return;
    note_free( &notes[index] );
    memmove( &notes[index], &notes[index + 1], (note_count - index - 1) * sizeof(*notes) );
    note_count--;
    notes_save();
}

static HBITMAP note_icon( struct note *note )
{
    HICON icon = NULL;

    if (note->bitmap) return note->bitmap;
    if (note->icon[0]) ExtractIconExW( note->icon, 0, NULL, &icon, 1 );
    if (icon)
    {
        note->bitmap = icon_bitmap( icon, SMALL_ICON );
        DestroyIcon( icon );
    }
    else if (note->hicon) note->bitmap = icon_bitmap( note->hicon, SMALL_ICON );
    return note->bitmap;
}

/* how long ago, or the time of the day, as Windows 11 tells it on a card */
static void note_time( const struct note *note, WCHAR *text, int size )
{
    SYSTEMTIME utc, local, now;
    FILETIME now_ft;
    ULONGLONG then, minutes;

    GetSystemTimeAsFileTime( &now_ft );
    then = ((ULONGLONG)note->time.dwHighDateTime << 32) | note->time.dwLowDateTime;
    minutes = (((ULONGLONG)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime) > then ?
              ((((ULONGLONG)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime) - then) / 600000000ull : 0;
    if (minutes < 1)
    {
        lstrcpynW( text, load_string( IDS_JUST_NOW ), size );
        return;
    }
    FileTimeToSystemTime( &note->time, &utc );
    SystemTimeToTzSpecificLocalTime( NULL, &utc, &local );
    GetLocalTime( &now );
    if (local.wYear == now.wYear && local.wMonth == now.wMonth && local.wDay == now.wDay)
    {
        if (!GetTimeFormatW( LOCALE_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, text, size )) text[0] = 0;
    }
    else describe_age( &note->time, text, size );
}

/**********************************************************************
 *          The calendar's days
 */

static BOOL leap( int year )
{
    return (year % 4 == 0 && year % 100) || year % 400 == 0;
}

static int month_days( int year, int month )
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && leap( year ) ? 29 : days[month - 1];
}

/* 0 for Sunday, as SYSTEMTIME has it */
static int week_day( int year, int month, int day )
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year--;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

/* the locale's first day of the week, 0 for Sunday */
static int first_week_day(void)
{
    WCHAR value[4];

    if (!GetLocaleInfoEx( LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, value, ARRAY_SIZE(value) )) return 1;
    return (value[0] - '0' + 1) % 7;   /* 0 there is Monday */
}

/* the date in cell n (0-41) of the month shown */
static void cell_date( int n, int *year, int *month, int *day )
{
    int offset = (week_day( view_year, view_month, 1 ) - first_week_day() + 7) % 7;
    int d = n - offset + 1, y = view_year, m = view_month;

    while (d < 1)
    {
        if (--m < 1) { m = 12; y--; }
        d += month_days( y, m );
    }
    while (d > month_days( y, m ))
    {
        d -= month_days( y, m );
        if (++m > 12) { m = 1; y++; }
    }
    *year = y;
    *month = m;
    *day = d;
}

static void turn_month( int delta )
{
    view_month += delta;
    while (view_month < 1) { view_month += 12; view_year--; }
    while (view_month > 12) { view_month -= 12; view_year++; }
}

/**********************************************************************
 *          Layout
 */

static int text_height( HDC hdc, const WCHAR *text, HFONT font, int width, int max_lines )
{
    HGDIOBJ old = SelectObject( hdc, font );
    RECT rect = { 0, 0, width, 0 };
    TEXTMETRICW metrics;
    int height;

    GetTextMetricsW( hdc, &metrics );
    DrawTextW( hdc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL );
    height = rect.bottom;
    if (max_lines) height = min( height, metrics.tmHeight * max_lines );
    SelectObject( hdc, old );
    return height;
}

static int line_height( HDC hdc, HFONT font )
{
    HGDIOBJ old = SelectObject( hdc, font );
    TEXTMETRICW metrics;

    GetTextMetricsW( hdc, &metrics );
    SelectObject( hdc, old );
    return metrics.tmHeight;
}

static int card_text_width(void)
{
    return WIDTH - 2 * CARD_INSET - 2 * CARD_PAD;
}

/* the cards' heights and the center's whole height */
static void layout_center(void)
{
    HDC hdc = GetDC( NULL );
    int y = HEADER;

    for (int i = 0; i < note_count; i++)
    {
        struct note *note = &notes[i];

        note->height = CARD_PAD + max( SMALL_ICON, line_height( hdc, font_small ) ) + px( 8 );
        if (note->title[0]) note->height += text_height( hdc, note->title, font_title, card_text_width(), 2 ) + px( 2 );
        if (note->text[0]) note->height += text_height( hdc, note->text, font_body, card_text_width(), TEXT_LINES );
        note->height += CARD_PAD;
        y += note->height + CARD_GAP;
    }
    if (!note_count) y += EMPTY_HEIGHT;
    panels[CENTER].content = y + px( 4 );
    ReleaseDC( NULL, hdc );
}

static int calendar_height(void)
{
    if (collapsed) return HEADER;
    return HEADER + 1 + HEADER + WEEK_ROW + 6 * CELL + px( 12 );
}

/* over the taskbar on the clock's monitor, against the screen's right edge */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, avail, cal = calendar_height(), center;
    BOOL bottom;

    GetMonitorInfoW( MonitorFromRect( &clock_rect, MONITOR_DEFAULTTONEAREST ), &info );
    bottom = (clock_rect.top + clock_rect.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2;
    x = info.rcWork.right - WIDTH - MARGIN;
    layout_center();
    avail = info.rcWork.bottom - info.rcWork.top - cal - 3 * MARGIN;
    center = max( HEADER + px( 40 ), min( panels[CENTER].content, avail ) );
    panels[CENTER].height = center;
    panels[CALENDAR].height = cal;
    panels[CENTER].scroll = max( 0, min( panels[CENTER].scroll, panels[CENTER].content - center ) );
    if (bottom)
    {
        SetWindowPos( panels[CALENDAR].hwnd, HWND_TOPMOST, x, info.rcWork.bottom - MARGIN - cal, WIDTH, cal,
                      SWP_NOACTIVATE );
        SetWindowPos( panels[CENTER].hwnd, HWND_TOPMOST, x, info.rcWork.bottom - 2 * MARGIN - cal - center, WIDTH,
                      center, SWP_NOACTIVATE );
    }
    else
    {
        SetWindowPos( panels[CALENDAR].hwnd, HWND_TOPMOST, x, info.rcWork.top + MARGIN, WIDTH, cal, SWP_NOACTIVATE );
        SetWindowPos( panels[CENTER].hwnd, HWND_TOPMOST, x, info.rcWork.top + 2 * MARGIN + cal, WIDTH, center,
                      SWP_NOACTIVATE );
    }
}

/**********************************************************************
 *          Painting
 */

static void add_target( struct panel *panel, enum kind kind, int index, const RECT *rect )
{
    if (panel->target_count >= MAX_TARGETS) return;
    panel->targets[panel->target_count].kind = kind;
    panel->targets[panel->target_count].index = index;
    panel->targets[panel->target_count].rect = *rect;
    panel->target_count++;
}

static BOOL same( const struct target *a, enum kind kind, int index )
{
    return a->kind == kind && a->index == index;
}

/* the hover and the press of something round or square */
static void draw_state( HDC hdc, const struct panel *panel, enum kind kind, int index, const RECT *rect, float radius )
{
    if (same( &panel->pressed, kind, index ) && same( &panel->hot, kind, index ))
        fill_round_rect( hdc, rect, radius, pal.ink, PRESSED_ALPHA );
    else if (same( &panel->hot, kind, index ))
        fill_round_rect( hdc, rect, radius, pal.ink, HOVER_ALPHA );
}

/* GDI text over something opaque (today's disc): GDI leaves its alpha 0,
 * which dwm.exe would key by brightness, so the alpha under it goes back */
void text_over_opaque( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    DIBSECTION dib;
    HGDIOBJ bitmap = GetCurrentObject( hdc, OBJ_BITMAP );
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    BYTE *saved, *bits;

    GdiFlush();
    if (!bitmap || GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !dib.dsBm.bmBits ||
        dib.dsBm.bmBitsPixel != 32 || width <= 0 || height <= 0 || !(saved = malloc( width * height )))
    {
        paint_text( hdc, text, rect, font, color, format );
        return;
    }
    bits = dib.dsBm.bmBits;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            int px_ = rect->left + x, py = rect->top + y;
            saved[y * width + x] = (px_ >= 0 && py >= 0 && px_ < dib.dsBm.bmWidth && py < dib.dsBm.bmHeight) ?
                                   bits[py * dib.dsBm.bmWidthBytes + px_ * 4 + 3] : 0;
        }
    paint_text( hdc, text, rect, font, color, format );
    GdiFlush();
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            int px_ = rect->left + x, py = rect->top + y;
            if (px_ >= 0 && py >= 0 && px_ < dib.dsBm.bmWidth && py < dib.dsBm.bmHeight && saved[y * width + x])
                bits[py * dib.dsBm.bmWidthBytes + px_ * 4 + 3] = saved[y * width + x];
        }
    free( saved );
}

/* the ring around the day clicked: what it covers of each pixel, in the
 * accent colour; inside and outside it the backdrop, as the rest */
static void paint_ring( HDC hdc, const RECT *rect, int width, COLORREF color )
{
    int size = rect->right - rect->left;
    float r = size / 2.0f, inner = r - width;
    UINT32 *pixels;

    if (size <= 0 || rect->bottom - rect->top != size || !(pixels = malloc( size * size * sizeof(*pixels) ))) return;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int hits = 0;
            UINT32 a;

            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                {
                    float dx = x + (i + 0.5f) / 4 - r, dy = y + (j + 0.5f) / 4 - r, d = dx * dx + dy * dy;
                    if (d <= r * r && d >= inner * inner) hits++;
                }
            a = hits * 255 / 16;
            pixels[y * size + x] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                   ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    blend_bits( hdc, rect->left, rect->top, size, size, pixels );
    free( pixels );
}

static BOOL quiet(void)
{
    return read_dword( NOTES_KEY, L"DoNotDisturb", 0 ) != 0;
}

static void paint_center( struct panel *panel, HDC hdc, const RECT *client )
{
    RECT rect, area;
    int y, right = client->right - PAD / 2;
    HRGN clip;

    panel->target_count = 0;

    /* "Сповіщення", Do not disturb and Clear all */
    SetRect( &rect, right - BUTTON, (HEADER - BUTTON) / 2, right, (HEADER + BUTTON) / 2 );
    draw_state( hdc, panel, K_BELL, 0, &rect, px( 4 ) );
    draw_glyph( hdc, quiet() ? GLYPH_BELL_OFF : GLYPH_BELL, rect.left + (BUTTON - GLYPH) / 2,
                rect.top + (BUTTON - GLYPH) / 2, GLYPH, pal.ink, 255 );
    add_target( panel, K_BELL, 0, &rect );
    right = rect.left - px( 4 );
    if (note_count)
    {
        HDC screen = GetDC( NULL );
        HGDIOBJ old = SelectObject( screen, font_small );
        SIZE size;

        GetTextExtentPoint32W( screen, load_string( IDS_CLEAR_ALL ), lstrlenW( load_string( IDS_CLEAR_ALL ) ), &size );
        SelectObject( screen, old );
        ReleaseDC( NULL, screen );
        SetRect( &rect, right - size.cx - px( 16 ), (HEADER - BUTTON) / 2, right, (HEADER + BUTTON) / 2 );
        draw_state( hdc, panel, K_CLEAR, 0, &rect, px( 4 ) );
        paint_text( hdc, load_string( IDS_CLEAR_ALL ), &rect, font_small, pal.text, DT_CENTER );
        add_target( panel, K_CLEAR, 0, &rect );
        right = rect.left - px( 4 );
    }
    SetRect( &rect, PAD, 0, right, HEADER );
    paint_text( hdc, load_string( IDS_NOTIFICATIONS ), &rect, font_title, pal.text, DT_LEFT );

    if (!note_count)
    {
        SetRect( &rect, PAD, HEADER, client->right - PAD, HEADER + EMPTY_HEIGHT );
        paint_text( hdc, load_string( IDS_NO_NOTIFICATIONS ), &rect, font_body, pal.dim, DT_CENTER );
        return;
    }

    /* the cards, under the header as the wheel moves them */
    SetRect( &area, 0, HEADER, client->right, client->bottom );
    clip = CreateRectRgnIndirect( &area );
    SelectClipRgn( hdc, clip );
    y = HEADER - panel->scroll;
    for (int i = 0; i < note_count; i++)
    {
        struct note *note = &notes[i];
        RECT card, line, close;
        WCHAR when[64];
        int text_left, line_h;

        SetRect( &card, CARD_INSET, y, client->right - CARD_INSET, y + note->height );
        y += note->height + CARD_GAP;
        if (card.bottom <= HEADER || card.top >= client->bottom) continue;

        fill_round_rect( hdc, &card, px( 4 ), pal.ink, CARD_ALPHA );
        if (same( &panel->hot, K_CARD, i ) || same( &panel->hot, K_CARD_CLOSE, i ))
            fill_round_rect( hdc, &card, px( 4 ), pal.ink, same( &panel->pressed, K_CARD, i ) ? PRESSED_ALPHA : 0x08 );
        IntersectRect( &rect, &card, &area );
        add_target( panel, K_CARD, i, &rect );

        /* the program: its icon, its name, when */
        text_left = card.left + CARD_PAD;
        line_h = max( SMALL_ICON, px( 16 ) );
        draw_icon_bitmap( hdc, note_icon( note ), text_left, card.top + CARD_PAD, SMALL_ICON );
        note_time( note, when, ARRAY_SIZE(when) );
        SetRect( &close, card.right - px( 8 ) - px( 24 ), card.top + px( 6 ), card.right - px( 8 ), card.top + px( 30 ) );
        if (same( &panel->hot, K_CARD, i ) || same( &panel->hot, K_CARD_CLOSE, i ))
        {
            draw_state( hdc, panel, K_CARD_CLOSE, i, &close, px( 4 ) );
            draw_glyph( hdc, GLYPH_CLOSE, close.left + px( 4 ), close.top + px( 4 ), GLYPH, pal.ink, 255 );
            IntersectRect( &rect, &close, &area );
            add_target( panel, K_CARD_CLOSE, i, &rect );
            SetRect( &line, text_left + SMALL_ICON + px( 8 ), card.top + CARD_PAD, close.left - px( 4 ),
                     card.top + CARD_PAD + line_h );
            paint_text( hdc, note->app, &line, font_small, pal.dim, DT_LEFT );
        }
        else
        {
            SetRect( &line, text_left + SMALL_ICON + px( 8 ), card.top + CARD_PAD, card.right - CARD_PAD,
                     card.top + CARD_PAD + line_h );
            paint_text( hdc, when, &line, font_small, pal.dim, DT_RIGHT );
            line.right -= px( 60 );
            paint_text( hdc, note->app, &line, font_small, pal.dim, DT_LEFT );
        }

        /* its title and its text */
        SetRect( &line, text_left, card.top + CARD_PAD + line_h + px( 8 ), card.right - CARD_PAD, card.bottom );
        if (note->title[0])
        {
            HDC screen = GetDC( NULL );
            int h = text_height( screen, note->title, font_title, card_text_width(), 2 );

            ReleaseDC( NULL, screen );
            line.bottom = line.top + h;
            paint_text( hdc, note->title, &line, font_title, pal.text, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL );
            line.top = line.bottom + px( 2 );
        }
        line.bottom = card.bottom - CARD_PAD;
        if (note->text[0])
            paint_text( hdc, note->text, &line, font_body, pal.dim, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
}

static void paint_calendar( struct panel *panel, HDC hdc, const RECT *client )
{
    COLORREF accent = accent_color(), on_accent;
    WCHAR text[128];
    RECT rect;
    int x0 = (client->right - 7 * CELL) / 2, y, first = first_week_day();

    panel->target_count = 0;
    on_accent = (GetRValue( accent ) * 299 + GetGValue( accent ) * 587 + GetBValue( accent ) * 114) / 1000 > 150 ?
                RGB( 0, 0, 0 ) : RGB( 255, 255, 255 );

    /* today, and the chevron that folds the month */
    SetRect( &rect, client->right - PAD / 2 - BUTTON, (HEADER - BUTTON) / 2, client->right - PAD / 2,
             (HEADER + BUTTON) / 2 );
    draw_state( hdc, panel, K_COLLAPSE, 0, &rect, px( 4 ) );
    draw_glyph( hdc, collapsed ? GLYPH_CHEVRON_UP : GLYPH_CHEVRON_DOWN, rect.left + (BUTTON - GLYPH) / 2,
                rect.top + (BUTTON - GLYPH) / 2, GLYPH, pal.ink, 255 );
    add_target( panel, K_COLLAPSE, 0, &rect );
    if (!GetDateFormatW( LOCALE_USER_DEFAULT, 0, &today, L"dddd, d MMMM", text, ARRAY_SIZE(text) )) text[0] = 0;
    SetRect( &rect, PAD / 2, (HEADER - BUTTON) / 2, rect.left - px( 4 ), (HEADER + BUTTON) / 2 );
    draw_state( hdc, panel, K_DATE, 0, &rect, px( 4 ) );
    add_target( panel, K_DATE, 0, &rect );
    rect.left += PAD / 2;
    paint_text( hdc, text, &rect, font_title, pal.text, DT_LEFT );
    if (collapsed) return;

    SetRect( &rect, 0, HEADER, client->right, HEADER + 1 );
    fill_alpha( hdc, &rect, pal.ink, LINE_ALPHA );

    /* the month and its arrows */
    y = HEADER + 1;
    SetRect( &rect, client->right - PAD / 2 - BUTTON, y + (HEADER - BUTTON) / 2, client->right - PAD / 2,
             y + (HEADER + BUTTON) / 2 );
    draw_state( hdc, panel, K_NEXT, 0, &rect, px( 4 ) );
    draw_glyph( hdc, GLYPH_CHEVRON_DOWN, rect.left + (BUTTON - GLYPH) / 2, rect.top + (BUTTON - GLYPH) / 2, GLYPH,
                pal.ink, 255 );
    add_target( panel, K_NEXT, 0, &rect );
    OffsetRect( &rect, -BUTTON - px( 4 ), 0 );
    draw_state( hdc, panel, K_PREV, 0, &rect, px( 4 ) );
    draw_glyph( hdc, GLYPH_CHEVRON_UP, rect.left + (BUTTON - GLYPH) / 2, rect.top + (BUTTON - GLYPH) / 2, GLYPH,
                pal.ink, 255 );
    add_target( panel, K_PREV, 0, &rect );
    {
        SYSTEMTIME month = { view_year, view_month, 0, 1 };

        if (!GetDateFormatW( LOCALE_USER_DEFAULT, 0, &month, L"MMMM yyyy", text, ARRAY_SIZE(text) )) text[0] = 0;
    }
    SetRect( &rect, PAD / 2, y + (HEADER - BUTTON) / 2, rect.left - px( 4 ), y + (HEADER + BUTTON) / 2 );
    draw_state( hdc, panel, K_MONTH, 0, &rect, px( 4 ) );
    add_target( panel, K_MONTH, 0, &rect );
    rect.left += PAD / 2;
    paint_text( hdc, text, &rect, font_title, pal.text, DT_LEFT );

    /* the days of the week, from the locale's first */
    y += HEADER;
    for (int i = 0; i < 7; i++)
    {
        int wday = (first + i) % 7;   /* 0 Sunday */
        LCTYPE type = LOCALE_SSHORTESTDAYNAME1 + (wday + 6) % 7;   /* 1 is Monday */

        if (!GetLocaleInfoEx( LOCALE_NAME_USER_DEFAULT, type, text, ARRAY_SIZE(text) )) text[0] = 0;
        SetRect( &rect, x0 + i * CELL, y, x0 + (i + 1) * CELL, y + WEEK_ROW );
        paint_text( hdc, text, &rect, font_small, pal.text, DT_CENTER );
    }

    /* six weeks */
    y += WEEK_ROW;
    for (int n = 0; n < 42; n++)
    {
        int year, month, day, inset = px( 2 );
        BOOL is_today, in_month, selected;
        RECT disc;

        cell_date( n, &year, &month, &day );
        is_today = year == today.wYear && month == today.wMonth && day == today.wDay;
        in_month = month == view_month;
        selected = year == sel_year && month == sel_month && day == sel_day;
        SetRect( &rect, x0 + n % 7 * CELL, y + n / 7 * CELL, x0 + (n % 7 + 1) * CELL, y + (n / 7 + 1) * CELL );
        disc = rect;
        InflateRect( &disc, -inset, -inset );
        add_target( panel, K_DAY, n, &rect );
        swprintf( text, ARRAY_SIZE(text), L"%d", day );
        if (is_today)
        {
            fill_round_rect( hdc, &disc, (disc.right - disc.left) / 2.0f, accent, 255 );
            if (same( &panel->hot, K_DAY, n ))
                fill_round_rect( hdc, &disc, (disc.right - disc.left) / 2.0f, on_accent, 0x20 );
            if (selected)
            {
                RECT inner = disc;
                InflateRect( &inner, -px( 2 ), -px( 2 ) );
                fill_round_rect( hdc, &inner, (inner.right - inner.left) / 2.0f, pal.light ? RGB( 255, 255, 255 ) :
                                 RGB( 0, 0, 0 ), 0x60 );
            }
            text_over_opaque( hdc, text, &rect, font_body, on_accent, DT_CENTER );
            continue;
        }
        if (selected) paint_ring( hdc, &disc, px( 2 ), accent );
        if (same( &panel->hot, K_DAY, n )) draw_state( hdc, panel, K_DAY, n, &disc, (disc.right - disc.left) / 2.0f );
        paint_text( hdc, text, &rect, font_body, in_month ? pal.text : pal.dim, DT_CENTER );
    }
}

static void on_paint( int which )
{
    struct panel *panel = &panels[which];
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client;
    HDC mem;

    if (!(mem = paint_begin( panel->hwnd, &ps, &pal, &bitmap, &old ))) return;
    GetClientRect( panel->hwnd, &client );
    if (which == CENTER) paint_center( panel, mem, &client );
    else paint_calendar( panel, mem, &client );
    paint_end( panel->hwnd, &ps, mem, bitmap, old );
}

/**********************************************************************
 *          What a click does
 */

static struct target hit_test( struct panel *panel, POINT pt )
{
    struct target none = { K_NONE, -1 };

    /* the X of a card before the card */
    for (int i = panel->target_count - 1; i >= 0; i--)
        if (PtInRect( &panel->targets[i].rect, pt ) && panel->targets[i].kind == K_CARD_CLOSE) return panel->targets[i];
    for (int i = 0; i < panel->target_count; i++)
        if (PtInRect( &panel->targets[i].rect, pt )) return panel->targets[i];
    return none;
}

static void refresh(void)
{
    if (!clock_visible()) return;
    place();
    for (int i = 0; i < PANEL_COUNT; i++) InvalidateRect( panels[i].hwnd, NULL, FALSE );
}

static void open_date_time(void)
{
    clock_hide();
    ShellExecuteW( NULL, NULL, L"control.exe", L"timedate.cpl", NULL, SW_SHOWNORMAL );
}

static void activate_note( int index )
{
    struct note *note = &notes[index];
    DWORD pid = 0;

    if (note->reply && IsWindow( note->reply ))
    {
        if (GetWindowThreadProcessId( note->reply, &pid )) AllowSetForegroundWindow( pid );
        PostMessageW( note->reply, note->message, note->wparam, note->lparam );
    }
    note_remove( index );
    clock_hide();
}

static void activate( int which, struct target t )
{
    switch (t.kind)
    {
    case K_CLEAR:
        while (note_count) note_free( &notes[--note_count] );
        notes_save();
        panels[CENTER].scroll = 0;
        refresh();
        break;
    case K_BELL:
        write_dword( NOTES_KEY, L"DoNotDisturb", !quiet() );
        InvalidateRect( panels[CENTER].hwnd, NULL, FALSE );
        break;
    case K_CARD:
        activate_note( t.index );
        break;
    case K_CARD_CLOSE:
        note_remove( t.index );
        refresh();
        break;
    case K_DATE:
        open_date_time();
        break;
    case K_COLLAPSE:
        collapsed = !collapsed;
        write_dword( CLOCK_KEY, L"CalendarCollapsed", collapsed );
        refresh();
        break;
    case K_MONTH:
        view_year = today.wYear;
        view_month = today.wMonth;
        InvalidateRect( panels[CALENDAR].hwnd, NULL, FALSE );
        break;
    case K_PREV:
    case K_NEXT:
        turn_month( t.kind == K_PREV ? -1 : 1 );
        InvalidateRect( panels[CALENDAR].hwnd, NULL, FALSE );
        break;
    case K_DAY:
    {
        int year, month, day;

        cell_date( t.index, &year, &month, &day );
        if (year == sel_year && month == sel_month && day == sel_day) sel_year = sel_month = sel_day = 0;
        else
        {
            sel_year = year;
            sel_month = month;
            sel_day = day;
        }
        if (month != view_month)
        {
            view_year = year;
            view_month = month;
        }
        InvalidateRect( panels[CALENDAR].hwnd, NULL, FALSE );
        break;
    }
    default:
        break;
    }
}

static int panel_index( HWND hwnd )
{
    for (int i = 0; i < PANEL_COUNT; i++) if (panels[i].hwnd == hwnd) return i;
    return -1;
}

static LRESULT WINAPI panel_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    int which = panel_index( hwnd );
    struct panel *panel = which >= 0 ? &panels[which] : NULL;
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    struct target t;

    if (!panel) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (panel->hot.kind == K_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent( &tme );
        }
        t = hit_test( panel, pt );
        if (t.kind != panel->hot.kind || t.index != panel->hot.index)
        {
            panel->hot = t;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSELEAVE:
        panel->hot.kind = panel->pressed.kind = K_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        panel->pressed = hit_test( panel, pt );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        t = hit_test( panel, pt );
        if (t.kind != K_NONE && t.kind == panel->pressed.kind && t.index == panel->pressed.index)
        {
            panel->pressed.kind = K_NONE;
            activate( which, t );
        }
        panel->pressed.kind = K_NONE;
        if (clock_visible()) InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_MOUSEWHEEL:
        if (which == CALENDAR)
        {
            if (!collapsed)
            {
                turn_month( (short)HIWORD( wparam ) > 0 ? -1 : 1 );
                InvalidateRect( hwnd, NULL, FALSE );
            }
        }
        else
        {
            int scroll = panel->scroll - (short)HIWORD( wparam ) * px( 48 ) / WHEEL_DELTA;
            panel->scroll = max( 0, min( scroll, panel->content - panel->height ) );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) clock_hide();
        else if (which == CALENDAR && (wparam == VK_PRIOR || wparam == VK_NEXT))
        {
            turn_month( wparam == VK_PRIOR ? -1 : 1 );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_TICK)
        {
            SYSTEMTIME now;

            GetLocalTime( &now );
            if (now.wDay != today.wDay || now.wMonth != today.wMonth || now.wYear != today.wYear)
            {
                today = now;
                InvalidateRect( panels[CALENDAR].hwnd, NULL, FALSE );
            }
            InvalidateRect( panels[CENTER].hwnd, NULL, FALSE );   /* "how long ago" */
        }
        return 0;

    case WM_ACTIVATE:
        /* from one panel to the other they stay; anywhere else they go */
        if (LOWORD( wparam ) == WA_INACTIVE && panel_index( (HWND)lparam ) < 0) clock_hide();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint( which );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_panels(void)
{
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP, corner = ARCTIC_FLYOUT_CORNER;
    WNDCLASSW cls = { 0 };

    if (created) return panels[CENTER].hwnd && panels[CALENDAR].hwnd;
    created = TRUE;
    cls.lpfnWndProc = panel_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticClockFlyout";
    RegisterClassW( &cls );
    for (int i = 0; i < PANEL_COUNT; i++)
    {
        panels[i].hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                                          0, 0, WIDTH, px( 200 ), NULL, NULL, startui_instance, NULL );
        if (!panels[i].hwnd) return FALSE;
        DwmSetWindowAttribute( panels[i].hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
        DwmSetWindowAttribute( panels[i].hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    }
    font_title = shell_font( px( 14 ), FW_SEMIBOLD );
    font_body = shell_font( px( 14 ), FW_NORMAL );
    font_small = shell_font( px( 12 ), FW_NORMAL );
    return TRUE;
}

/**********************************************************************
 *          Showing them
 */

BOOL clock_visible(void)
{
    return created && panels[CALENDAR].hwnd && IsWindowVisible( panels[CALENDAR].hwnd );
}

void clock_hide(void)
{
    if (!clock_visible()) return;
    KillTimer( panels[CALENDAR].hwnd, TIMER_TICK );
    for (int i = 0; i < PANEL_COUNT; i++)
    {
        ShowWindow( panels[i].hwnd, SW_HIDE );
        panels[i].hot.kind = panels[i].pressed.kind = K_NONE;
    }
    hidden_at = GetTickCount();
}

void clock_toggle( HWND tray_window, const RECT *clock, BOOL click )
{
    if (!create_panels()) return;
    if (clock_visible())
    {
        clock_hide();
        return;
    }
    /* the click on the clock that took the focus away and closed them */
    if (click && GetTickCount() - hidden_at < 300) return;

    tray = tray_window;
    if (clock) clock_rect = *clock;
    else SetRect( &clock_rect, GetSystemMetrics( SM_CXSCREEN ) - 1, GetSystemMetrics( SM_CYSCREEN ) - 1,
                  GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    menu_hide();
    menu10_hide();
    menu11_hide();
    widgets_hide();
    notes_load();
    GetLocalTime( &today );
    view_year = today.wYear;
    view_month = today.wMonth;
    sel_year = sel_month = sel_day = 0;
    collapsed = read_dword( CLOCK_KEY, L"CalendarCollapsed", 0 ) != 0;
    panels[CENTER].scroll = 0;
    place();
    look_load_taskbar( &look );
    for (int i = 0; i < PANEL_COUNT; i++)
    {
        palette_init( &pal, look_apply( panels[i].hwnd, &look ) );
        InvalidateRect( panels[i].hwnd, NULL, FALSE );
    }
    ShowWindow( panels[CENTER].hwnd, SW_SHOWNOACTIVATE );
    ShowWindow( panels[CALENDAR].hwnd, SW_SHOW );
    SetForegroundWindow( panels[CALENDAR].hwnd );
    SetFocus( panels[CALENDAR].hwnd );
    SetTimer( panels[CALENDAR].hwnd, TIMER_TICK, 30000, NULL );
}

/**********************************************************************
 *          What explorer calls
 */

/* the clock was clicked: both panels come, or go */
void WINAPI ArcticClockFlyoutToggle( HWND tray_window, const RECT *clock )
{
    clock_toggle( tray_window, clock, TRUE );
}

BOOL WINAPI ArcticClockFlyoutVisible(void)
{
    return clock_visible();
}

void WINAPI ArcticClockFlyoutHide(void)
{
    clock_hide();
}

/* a notification the shell showed, or held back for Do not disturb: what
 * a click on its card posts to the program, while the program runs */
void WINAPI ArcticNotificationAdd( const WCHAR *app, const WCHAR *icon_path, HICON icon, const WCHAR *title,
                                   const WCHAR *text, HWND reply, UINT message, WPARAM wparam, LPARAM lparam )
{
    struct note *note;

    if ((!title || !title[0]) && (!text || !text[0])) return;
    notes_load();
    if (note_count == MAX_NOTES) note_free( &notes[--note_count] );
    memmove( &notes[1], &notes[0], note_count * sizeof(*notes) );
    note_count++;
    note = &notes[0];
    memset( note, 0, sizeof(*note) );
    GetSystemTimeAsFileTime( &note->time );
    lstrcpynW( note->app, app ? app : L"", ARRAY_SIZE(note->app) );
    lstrcpynW( note->icon, icon_path ? icon_path : L"", ARRAY_SIZE(note->icon) );
    lstrcpynW( note->title, title ? title : L"", ARRAY_SIZE(note->title) );
    lstrcpynW( note->text, text ? text : L"", ARRAY_SIZE(note->text) );
    note->reply = reply;
    note->message = message;
    note->wparam = wparam;
    note->lparam = lparam;
    if (icon) note->hicon = CopyIcon( icon );
    notes_save();
    refresh();
}

/* Do not disturb: the shell keeps its notifications here and shows none */
BOOL WINAPI ArcticNotificationsQuiet(void)
{
    return quiet();
}
