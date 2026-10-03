/*
 * Volume icon of the notification area: what programs play
 *
 * The sessions of SystemMediaTransportControls are in the shared section of
 * wine/arctic_media.h (windows.media.mediacontrol). The one shown is the one
 * playing, else the one changed last; a row whose program is gone is freed.
 * Its card is MtcUvc's TransportControlsTemplate of Windows 10: 360 x 120,
 * the program's name, the title and the artist in a 260 wide column (Margin
 * 12,12 / 12,6 / 12,6,0,6, MediaControlTitleStyle 228 wide), its buttons
 * under them, 36 x 36 with 18 glyphs (Previous E892, Play E768, Pause E769,
 * Next E893; #19FFFFFF under the pointer, #33FFFFFF pressed, 60% when off),
 * and on the right a 96 x 96 tile in a 120 x 120 area, where Windows puts
 * the album art and Arctic the program's icon.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdlib.h>

#include "sndvolsso.h"
#include "winver.h"
#include "wine/arctic_media.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

#define BUTTON_SIZE     36
#define TEXT_COLUMN     260
#define TILE_AREA       120
#define TILE_SIZE       96

/* MediaPlaybackStatus */
#define STATUS_PLAYING  3

/* SystemMediaTransportControlsButton */
#define BUTTON_PLAY     0
#define BUTTON_PAUSE    1
#define BUTTON_NEXT     6
#define BUTTON_PREVIOUS 7

static struct arctic_media_shared *get_shared(void)
{
    static struct arctic_media_shared *shared;
    struct arctic_media_shared *view;
    HANDLE mapping;

    if (shared) return shared;
    if (!(mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*view),
                                        ARCTIC_MEDIA_SECTION )))
        return NULL;
    view = MapViewOfFile( mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*view) );
    if (!view)
    {
        CloseHandle( mapping );
        return NULL;
    }
    /* the handle stays open: in Wine the section's name goes with its
     * last handle, and other processes would make a section of their own */
    InterlockedCompareExchange( (LONG *)&view->version, ARCTIC_MEDIA_VERSION, 0 );
    if (view->version != ARCTIC_MEDIA_VERSION)
    {
        UnmapViewOfFile( view );
        CloseHandle( mapping );
        return NULL;
    }
    if (InterlockedCompareExchangePointer( (void **)&shared, view, NULL ))
    {
        UnmapViewOfFile( view );
        CloseHandle( mapping );
    }
    return shared;
}

static BOOL process_alive( DWORD pid )
{
    HANDLE process = OpenProcess( SYNCHRONIZE, FALSE, pid );
    BOOL alive = process && WaitForSingleObject( process, 0 ) == WAIT_TIMEOUT;

    if (process) CloseHandle( process );
    return alive;
}

/* a row whose program is gone is free again */
static void drop_session( struct arctic_media_shared *shared, UINT32 id )
{
    HANDLE mutex = CreateMutexW( NULL, FALSE, ARCTIC_MEDIA_MUTEX );

    if (mutex) WaitForSingleObject( mutex, INFINITE );
    for (UINT i = 0; i < ARCTIC_MEDIA_SESSIONS; i++)
        if (shared->sessions[i].id == id) shared->sessions[i].id = 0;
    InterlockedIncrement( &shared->serial );
    if (mutex)
    {
        ReleaseMutex( mutex );
        CloseHandle( mutex );
    }
}

/* "Google Chrome": the program's own description of itself, else its file's name */
static void app_name( DWORD pid, WCHAR *name, int size )
{
    static DWORD cached_pid;
    static WCHAR cached[128];
    WCHAR path[MAX_PATH], query[64], *desc, *base;
    DWORD len = ARRAY_SIZE(path), handle;
    struct { WORD lang, codepage; } *translation;
    UINT bytes;
    HANDLE process;
    void *info;

    if (pid == cached_pid && cached[0])
    {
        lstrcpynW( name, cached, size );
        return;
    }
    name[0] = 0;
    if (!(process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ))) return;
    if (!QueryFullProcessImageNameW( process, 0, path, &len )) path[0] = 0;
    CloseHandle( process );
    if (!path[0]) return;

    if ((len = GetFileVersionInfoSizeW( path, &handle )) && (info = malloc( len )))
    {
        if (GetFileVersionInfoW( path, 0, len, info ) &&
            VerQueryValueW( info, L"\\VarFileInfo\\Translation", (void **)&translation, &bytes ) && bytes >= 4)
        {
            swprintf( query, ARRAY_SIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                      translation->lang, translation->codepage );
            if (VerQueryValueW( info, query, (void **)&desc, &bytes ) && bytes > 1)
                lstrcpynW( name, desc, size );
        }
        free( info );
    }
    if (!name[0])
    {
        WCHAR *dot;
        base = wcsrchr( path, '\\' );
        lstrcpynW( name, base ? base + 1 : path, size );
        if ((dot = wcsrchr( name, '.' ))) *dot = 0;
    }
    cached_pid = pid;
    lstrcpynW( cached, name, ARRAY_SIZE(cached) );
}

static HICON session_icon( HWND hwnd )
{
    DWORD_PTR icon = 0;

    if (SendMessageTimeoutW( hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &icon ) && icon) return (HICON)icon;
    if ((icon = GetClassLongPtrW( hwnd, GCLP_HICON ))) return (HICON)icon;
    if (SendMessageTimeoutW( hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &icon ) && icon) return (HICON)icon;
    return LoadIconW( NULL, (const WCHAR *)IDI_APPLICATION );
}

/* the session to show: the one playing, else the one changed last; without
 * details, not the program's name and icon, which ask other processes */
static BOOL find_current( struct media_info *info, BOOL details )
{
    struct arctic_media_shared *shared = get_shared();
    const struct arctic_media_session *best = NULL;
    struct arctic_media_session row;

    memset( info, 0, sizeof(*info) );
    if (!shared) return FALSE;
    for (UINT i = 0; i < ARCTIC_MEDIA_SESSIONS; i++)
    {
        const struct arctic_media_session *s = &shared->sessions[i];

        if (!s->id || !s->enabled || (!s->title[0] && s->status != STATUS_PLAYING)) continue;
        if (!IsWindow( ULongToHandle( s->window ) ) || (details && !process_alive( s->process )))
        {
            if (details) drop_session( shared, s->id );
            continue;
        }
        if (!best || (s->status == STATUS_PLAYING) > (best->status == STATUS_PLAYING) ||
            ((s->status == STATUS_PLAYING) == (best->status == STATUS_PLAYING) && (int)(s->changed - best->changed) > 0))
            best = s;
    }
    if (!best) return FALSE;
    row = *best;                      /* a torn read shows for one refresh at most */
    info->present = TRUE;
    info->id = row.id;
    info->notify = ULongToHandle( row.notify );
    info->window = ULongToHandle( row.window );
    info->playing = row.status == STATUS_PLAYING;
    info->buttons = row.buttons;
    lstrcpynW( info->title, row.title, ARRAY_SIZE(info->title) );
    lstrcpynW( info->artist, row.artist, ARRAY_SIZE(info->artist) );
    if (details)
    {
        app_name( row.process, info->app, ARRAY_SIZE(info->app) );
        info->icon = session_icon( info->window );
    }
    return TRUE;
}

BOOL media_current( struct media_info *info )
{
    return find_current( info, TRUE );
}

/* for the keyboard hook, which must answer at once */
BOOL media_session( struct media_info *info )
{
    return find_current( info, FALSE );
}

static BOOL action_enabled( const struct media_info *info, int action )
{
    switch (action)
    {
    case MEDIA_PREVIOUS:  return info->notify && (info->buttons & ARCTIC_MEDIA_PREVIOUS);
    case MEDIA_NEXT:      return info->notify && (info->buttons & ARCTIC_MEDIA_NEXT);
    case MEDIA_PLAYPAUSE: return info->notify && (info->buttons & (info->playing ? ARCTIC_MEDIA_PAUSE : ARCTIC_MEDIA_PLAY));
    }
    return FALSE;
}

/* the press goes to the program, as its ButtonPressed */
BOOL media_press( const struct media_info *info, int action )
{
    static UINT msg;
    UINT button;

    if (!info->present || !action_enabled( info, action )) return FALSE;
    if (!msg) msg = RegisterWindowMessageW( ARCTIC_MEDIA_BUTTON_MSG );
    button = action == MEDIA_PREVIOUS ? BUTTON_PREVIOUS : action == MEDIA_NEXT ? BUTTON_NEXT :
             info->playing ? BUTTON_PAUSE : BUTTON_PLAY;
    TRACE( "session %u button %u\n", info->id, button );
    return PostMessageW( info->notify, msg, button, info->id );
}

static void button_rect( const RECT *card, int action, RECT *rect )
{
    int x = card->left + 12 + action * BUTTON_SIZE, y = card->bottom - BUTTON_SIZE;
    SetRect( rect, x, y, x + BUTTON_SIZE, y + BUTTON_SIZE );
}

int media_hit( const RECT *card, POINT pt, const struct media_info *info )
{
    RECT rect;

    if (!info->present) return -1;
    for (int action = MEDIA_PREVIOUS; action <= MEDIA_NEXT; action++)
    {
        button_rect( card, action, &rect );
        if (PtInRect( &rect, pt )) return action;
    }
    return -1;
}

/* the glyphs of the buttons, 18 high, drawn as Segoe MDL2's: a skip is a bar
 * and a triangle, play a triangle, pause two bars */
static float tri_cov( float x, float y, const float p[3][2] )
{
    float d1 = (x - p[1][0]) * (p[0][1] - p[1][1]) - (p[0][0] - p[1][0]) * (y - p[1][1]);
    float d2 = (x - p[2][0]) * (p[1][1] - p[2][1]) - (p[1][0] - p[2][0]) * (y - p[2][1]);
    float d3 = (x - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (y - p[0][1]);
    BOOL neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static void draw_button_glyph( HDC hdc, const RECT *rect, int action, BOOL playing, BYTE alpha )
{
    const int size = 18;
    float cov[18 * 18];
    int ox = (rect->left + rect->right - size) / 2, oy = (rect->top + rect->bottom - size) / 2;

    for (int py = 0; py < size; py++)
    {
        for (int px = 0; px < size; px++)
        {
            int hits = 0;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float x = px + (i + 0.5f) / 4, y = py + (j + 0.5f) / 4;
                    BOOL on = FALSE;

                    if (action == MEDIA_PLAYPAUSE && playing)
                        on = (x >= 4 && x <= 7.5f && y >= 3 && y <= 15) || (x >= 10.5f && x <= 14 && y >= 3 && y <= 15);
                    else if (action == MEDIA_PLAYPAUSE)
                    {
                        static const float play[3][2] = { { 5, 2.5f }, { 15.5f, 9 }, { 5, 15.5f } };
                        on = tri_cov( x, y, play );
                    }
                    else if (action == MEDIA_NEXT)
                    {
                        static const float next[3][2] = { { 3, 3 }, { 12.5f, 9 }, { 3, 15 } };
                        on = tri_cov( x, y, next ) || (x >= 13 && x <= 15 && y >= 3 && y <= 15);
                    }
                    else
                    {
                        static const float prev[3][2] = { { 15, 3 }, { 5.5f, 9 }, { 15, 15 } };
                        on = tri_cov( x, y, prev ) || (x >= 3 && x <= 5 && y >= 3 && y <= 15);
                    }
                    if (on) hits++;
                }
            }
            cov[py * size + px] = hits * (alpha / 255.f) / 16;
        }
    }
    blend_white( hdc, ox, oy, size, size, cov );
}

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, BYTE alpha )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, RGB( alpha, alpha, alpha ) );
    DrawTextW( hdc, text, -1, rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

/* the card in card, which is MEDIA_CARD_WIDTH x MEDIA_CARD_HEIGHT */
void media_paint( HDC hdc, const RECT *card, const struct media_info *info, int hot, BOOL pressed, BYTE opacity )
{
    static HFONT title_font, caption_font;
    RECT rect;

    if (!info->present) return;
    if (!title_font) title_font = shell_font( 15, FW_NORMAL );
    if (!caption_font) caption_font = shell_font( 12, FW_NORMAL );
    SetBkMode( hdc, TRANSPARENT );

    SetRect( &rect, card->left + 12, card->top + 12, card->left + 12 + 228, card->top + 32 );
    draw_text( hdc, info->app, &rect, title_font, (BYTE)(0xcc * opacity / 255) );
    SetRect( &rect, card->left + 12, card->top + 38, card->left + 12 + 228, card->top + 58 );
    draw_text( hdc, info->title, &rect, title_font, opacity );
    SetRect( &rect, card->left + 12, card->top + 64, card->left + 12 + 228, card->top + 80 );
    draw_text( hdc, info->artist, &rect, caption_font, (BYTE)(0xcc * opacity / 255) );

    for (int action = MEDIA_PREVIOUS; action <= MEDIA_NEXT; action++)
    {
        BOOL enabled = action_enabled( info, action );

        button_rect( card, action, &rect );
        if (enabled && hot == action) fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), (BYTE)((pressed ? 0x33 : 0x19) * opacity / 255) );
        draw_button_glyph( hdc, &rect, action, info->playing, (BYTE)((enabled ? 255 : 0x99) * opacity / 255) );
    }

    /* the program's icon on the tile where Windows has the album art */
    SetRect( &rect, card->right - TILE_AREA + (TILE_AREA - TILE_SIZE) / 2, card->top + (TILE_AREA - TILE_SIZE) / 2,
             card->right - TILE_AREA + (TILE_AREA + TILE_SIZE) / 2, card->top + (TILE_AREA + TILE_SIZE) / 2 );
    fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), (BYTE)(0x19 * opacity / 255) );
    if (info->icon)
        DrawIconEx( hdc, (rect.left + rect.right) / 2 - 24, (rect.top + rect.bottom) / 2 - 24, info->icon, 48, 48, 0, NULL,
                    DI_NORMAL );
}
