/*
 * The Start menus: what the standard menu and those of Windows 10 and 11 share
 *
 * Opening places, the power menu, the context menu of a program, the
 * style chosen, and how text and glyphs go over the backdrop.
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
#include "shlobj.h"
#include "knownfolders.h"
#include "winternl.h"
#include "powrprof.h"
#include "winreg.h"

#define REG_KEY  L"Software\\Arctic\\StartMenu"

void palette_init( struct palette *pal, BOOL light )
{
    pal->light = light;
    if (light)
    {
        pal->back = RGB( 255, 255, 255 );
        pal->text = RGB( 0, 0, 0 );
        pal->dim = RGB( 0x5d, 0x5d, 0x5d );
        pal->ink = RGB( 0, 0, 0 );
        pal->field = RGB( 255, 255, 255 );
        pal->field_text = RGB( 0, 0, 0 );
    }
    else
    {
        pal->back = RGB( 0, 0, 0 );
        pal->text = RGB( 255, 255, 255 );
        pal->dim = RGB( 0xa0, 0xa0, 0xa0 );
        pal->ink = RGB( 255, 255, 255 );
        pal->field = RGB( 255, 255, 255 );
        pal->field_text = RGB( 0, 0, 0 );
    }
}

void paint_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    if (!(format & DT_WORDBREAK)) format |= DT_SINGLELINE | DT_VCENTER;
    DrawTextW( hdc, text, -1, rect, format | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

/* the keyboard's focus around something */
void frame_rect( HDC hdc, const RECT *rect, int width, COLORREF color )
{
    RECT side;

    SetRect( &side, rect->left, rect->top, rect->right, rect->top + width );
    fill_alpha( hdc, &side, color, 255 );
    SetRect( &side, rect->left, rect->bottom - width, rect->right, rect->bottom );
    fill_alpha( hdc, &side, color, 255 );
    SetRect( &side, rect->left, rect->top + width, rect->left + width, rect->bottom - width );
    fill_alpha( hdc, &side, color, 255 );
    SetRect( &side, rect->right - width, rect->top + width, rect->right, rect->bottom - width );
    fill_alpha( hdc, &side, color, 255 );
}

/* the menu's surface in a DIB, filled with what shows the backdrop */
HDC paint_begin( HWND hwnd, PAINTSTRUCT *ps, const struct palette *pal, HBITMAP *bitmap, HGDIOBJ *old )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HDC hdc = BeginPaint( hwnd, ps ), mem;
    RECT client;
    UINT32 *bits;
    size_t count;

    GetClientRect( hwnd, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    mem = CreateCompatibleDC( hdc );
    if (!(*bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( mem );
        EndPaint( hwnd, ps );
        return NULL;
    }
    *old = SelectObject( mem, *bitmap );
    /* without alpha: black, or white, is the acrylic seen through */
    count = (size_t)client.right * client.bottom;
    for (size_t i = 0; i < count; i++) bits[i] = pal->light ? 0x00ffffff : 0;
    SetBkMode( mem, TRANSPARENT );
    return mem;
}

void paint_end( HWND hwnd, PAINTSTRUCT *ps, HDC mem, HBITMAP bitmap, HGDIOBJ old )
{
    RECT client;

    GetClientRect( hwnd, &client );
    BitBlt( ps->hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
    SelectObject( mem, old );
    DeleteObject( bitmap );
    DeleteDC( mem );
    EndPaint( hwnd, ps );
}

/**********************************************************************
 *          Places
 */

void open_folder_id( int csidl )
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    LPITEMIDLIST pidl;

    if (FAILED(SHGetSpecialFolderLocation( NULL, csidl, &pidl ))) return;
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW( &info );
    CoTaskMemFree( pidl );
}

void open_path( const WCHAR *path )
{
    ShellExecuteW( NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL );
}

void open_known_folder( const KNOWNFOLDERID *id, const WCHAR *fallback )
{
    WCHAR *path, profile[MAX_PATH];

    if (SUCCEEDED(SHGetKnownFolderPath( id, 0, NULL, &path )))
    {
        open_path( path );
        CoTaskMemFree( path );
        return;
    }
    if (SUCCEEDED(SHGetFolderPathW( NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, profile )))
    {
        lstrcatW( profile, L"\\" );
        lstrcatW( profile, fallback );
        open_path( profile );
    }
}

void open_downloads(void)
{
    open_known_folder( &FOLDERID_Downloads, L"Downloads" );
}

/* the places that can stand on the Start menus of Windows 10 and 11, as
 * "Choose which folders appear on Start" has them */
static const struct
{
    UINT name;
    enum glyph glyph;
}
places[FOLDER_COUNT] =
{
    { IDS_FILE_EXPLORER_PLACE, GLYPH_FOLDER },
    { IDS_SETTINGS,            GLYPH_SETTINGS },
    { IDS_DOCUMENTS,           GLYPH_DOCUMENT },
    { IDS_DOWNLOADS,           GLYPH_DOWNLOAD },
    { IDS_MUSIC,               GLYPH_MUSIC },
    { IDS_PICTURES,            GLYPH_PICTURE },
    { IDS_VIDEOS,              GLYPH_VIDEO },
    { IDS_NETWORK,             GLYPH_NETWORK },
    { IDS_USER_FOLDER,         GLYPH_PERSON },
};

const WCHAR *folder_name( int folder )
{
    return load_string( places[folder].name );
}

enum glyph folder_glyph( int folder )
{
    return places[folder].glyph;
}

void folder_open( int folder )
{
    switch (folder)
    {
    case FOLDER_EXPLORER:  open_folder_id( CSIDL_DRIVES ); break;
    case FOLDER_SETTINGS:  open_folder_id( CSIDL_CONTROLS ); break;
    case FOLDER_DOCUMENTS: open_folder_id( CSIDL_PERSONAL ); break;
    case FOLDER_DOWNLOADS: open_downloads(); break;
    case FOLDER_MUSIC:     open_folder_id( CSIDL_MYMUSIC ); break;
    case FOLDER_PICTURES:  open_folder_id( CSIDL_MYPICTURES ); break;
    case FOLDER_VIDEOS:    open_folder_id( CSIDL_MYVIDEO ); break;
    case FOLDER_NETWORK:   open_folder_id( CSIDL_NETWORK ); break;
    case FOLDER_USER:      open_folder_id( CSIDL_PROFILE ); break;
    }
}

/**********************************************************************
 *          Power
 */

BOOL update_ready(void)
{
    WCHAR path[MAX_PATH];

    GetSystemDirectoryW( path, ARRAY_SIZE(path) );
    lstrcatW( path, L"\\Host\\Update\\ready" );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* Power Options' "Show in Power menu" (FlyoutMenuSettings, as Windows keeps them) */
static BOOL flyout_shows( const WCHAR *name, DWORD fallback )
{
    DWORD value = fallback, size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FlyoutMenuSettings", name,
                  RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

/* "Режим глибокого сну", once msconfig turned hibernation on (HibernateEnabled,
 * as Windows keeps it; docs/hibernation.md) and Power Options show it */
static BOOL hibernate_enabled(void)
{
    DWORD value = 0, size = sizeof(value);

    return !RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled",
                          RRF_RT_REG_DWORD, NULL, &value, &size ) && value && flyout_shows( L"ShowHibernateOption", 1 );
}

/* "Сон" where the machine can sleep (docs/power.md) */
static BOOL sleep_enabled(void)
{
    return IsPwrSuspendAllowed() && flyout_shows( L"ShowSleepOption", 1 );
}

static DWORD WINAPI sleep_thread( void *arg )
{
    SetSuspendState( FALSE, FALSE, FALSE );
    return 0;
}

/* The machine sleeps inside SetSuspendState and the call returns when it
 * wakes: on a thread of its own, the menu keeps answering meanwhile */
static DWORD WINAPI hibernate_thread( void *arg )
{
    if (!SetSuspendState( TRUE, FALSE, FALSE ) && GetLastError() == ERROR_BUSY)
        MessageBoxW( NULL, load_string( IDS_HIBERNATE_REFUSED ), load_string( IDS_HIBERNATE ), MB_ICONWARNING );
    return 0;
}

void power_shut_down(void)
{
    ExitWindowsEx( EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED );
}

/* after a menu of ours: a click elsewhere took the foreground, and the
 * Start menu goes as well */
static void popup_ended( HWND owner, void (*hide)(void) )
{
    if (GetForegroundWindow() != owner) hide();
}

/* Windows 10's order: Sleep, Hibernate, Shut down, Restart. The menu hides
 * (hide) before anything is done; FALSE if nothing was chosen. */
BOOL power_popup( HWND owner, POINT pt, UINT align, BOOL *in_popup, void (*hide)(void), int popup_style,
                  const struct look *look )
{
    struct popup_item items[4];
    UINT count = 0, cmd;

    memset( items, 0, sizeof(items) );
    if (sleep_enabled()) items[count++] = (struct popup_item){ 4, load_string( IDS_SLEEP ), GLYPH_SLEEP };
    if (hibernate_enabled()) items[count++] = (struct popup_item){ 3, load_string( IDS_HIBERNATE ), GLYPH_SLEEP };
    items[count++] = (struct popup_item){ 2, load_string( IDS_SHUT_DOWN ), GLYPH_POWER };
    items[count++] = (struct popup_item){ 1, load_string( update_ready() ? IDS_UPDATE_AND_RESTART : IDS_RESTART ),
                                          GLYPH_UPDATE };
    *in_popup = TRUE;
    cmd = popup_track( owner, pt, align, items, count, popup_style, look );
    *in_popup = FALSE;
    if (!cmd)
    {
        popup_ended( owner, hide );
        return FALSE;
    }
    hide();
    /* winlogon ends the session, arctic-init restarts and the next start
     * installs a waiting update */
    if (cmd == 3 || cmd == 4)
    {
        HANDLE thread = CreateThread( NULL, 0, cmd == 3 ? hibernate_thread : sleep_thread, NULL, 0, NULL );
        if (thread) CloseHandle( thread );
    }
    else if (cmd == 1) ExitWindowsEx( EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED );
    else power_shut_down();
    return TRUE;
}

/* the user's menu of Windows 10 and 11: the account's settings, sign out */
void user_popup( HWND owner, POINT pt, UINT align, BOOL *in_popup, void (*hide)(void), int popup_style,
                 const struct look *look )
{
    struct popup_item items[] =
    {
        { 1, load_string( IDS_USER_FOLDER ), GLYPH_FOLDER },
        { 2, load_string( IDS_USER_ACCOUNTS ), GLYPH_PERSON },
        { 0 },
        { 3, load_string( IDS_SIGN_OUT ), GLYPH_SIGN_OUT },
    };
    UINT cmd;

    *in_popup = TRUE;
    cmd = popup_track( owner, pt, align, items, ARRAY_SIZE(items), popup_style, look );
    *in_popup = FALSE;
    if (!cmd)
    {
        popup_ended( owner, hide );
        return;
    }
    hide();
    switch (cmd)
    {
    case 1: open_folder_id( CSIDL_PROFILE ); break;
    case 2: ShellExecuteW( NULL, NULL, L"control.exe", L"userpasswords", NULL, SW_SHOWNORMAL ); break;
    case 3: ExitWindowsEx( EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED ); break;
    }
}

/* the context menu of a program: Open, Open file location, pin or unpin,
 * and, for one of the most used, Remove from this list. What was done. */
enum item_action item_popup( HWND owner, struct item *item, POINT pt, BOOL removable, BOOL *in_popup,
                             void (*hide)(void), int popup_style, const struct look *look )
{
    struct popup_item items[6];
    UINT count = 0, cmd;
    BOOL pinned;

    if (!item || item->kind != ITEM_PROGRAM) return ACTION_NONE;
    pinned = items_is_pinned( item );
    memset( items, 0, sizeof(items) );
    items[count++] = (struct popup_item){ 1, load_string( IDS_OPEN ), GLYPH_NONE };
    if (!item->args[0]) items[count++] = (struct popup_item){ 4, load_string( IDS_OPEN_LOCATION ), GLYPH_FOLDER };
    items[count++] = (struct popup_item){ 0 };
    items[count++] = (struct popup_item){ 2, load_string( pinned ? IDS_UNPIN : IDS_PIN ), pinned ? GLYPH_UNPIN : GLYPH_PIN };
    if (removable) items[count++] = (struct popup_item){ 3, load_string( IDS_REMOVE_FROM_LIST ), GLYPH_NONE };
    *in_popup = TRUE;
    cmd = popup_track( owner, pt, TPM_LEFTALIGN | TPM_TOPALIGN, items, count, popup_style, look );
    *in_popup = FALSE;
    switch (cmd)
    {
    case 1:
        hide();
        items_launch( item );
        return ACTION_LAUNCHED;
    case 2:
        items_pin( item, !pinned );
        return ACTION_CHANGED;
    case 3:
        items_forget( item );
        return ACTION_CHANGED;
    case 4:
        hide();
        items_open_location( item );
        return ACTION_LAUNCHED;
    }
    popup_ended( owner, hide );
    return ACTION_NONE;
}

/* "Виконати...": explorer's hot key for Win+R, which starts Run on a thread of its own */
void open_run( HWND tray )
{
    if (tray) PostMessageW( tray, WM_HOTKEY, 0x1f4 /* IDHK_RUN */, 0 );
}

/**********************************************************************
 *          The style
 */

static DWORD read_dword( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

/* Style, or Classic as the taskbar's properties wrote it before there were four */
int start_style_load(void)
{
    DWORD style = read_dword( L"Style", STYLE_COUNT );

    if (style < STYLE_COUNT) return style;
    return read_dword( L"Classic", 0 ) ? STYLE_CLASSIC : STYLE_STANDARD;
}

void start_style_save( int style )
{
    DWORD value = style, classic = style == STYLE_CLASSIC;
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"Style", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    /* explorer opens its own classic menu by this one */
    RegSetValueExW( key, L"Classic", 0, REG_DWORD, (const BYTE *)&classic, sizeof(classic) );
    RegCloseKey( key );
}

/* how long ago, as Windows 11's Recommended says it */
void describe_age( const FILETIME *time, WCHAR *text, int size )
{
    FILETIME now_ft;
    ULONGLONG now, then, minutes;
    SYSTEMTIME local, utc;

    GetSystemTimeAsFileTime( &now_ft );
    now = ((ULONGLONG)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime;
    then = ((ULONGLONG)time->dwHighDateTime << 32) | time->dwLowDateTime;
    minutes = now > then ? (now - then) / 600000000ull : 0;
    if (minutes < 1) lstrcpynW( text, load_string( IDS_JUST_NOW ), size );
    else if (minutes < 60) swprintf( text, size, load_string( IDS_MINUTES_AGO ), (int)minutes );
    else if (minutes < 24 * 60) swprintf( text, size, load_string( IDS_HOURS_AGO ), (int)(minutes / 60) );
    else if (minutes < 48 * 60) lstrcpynW( text, load_string( IDS_YESTERDAY ), size );
    else
    {
        FileTimeToSystemTime( time, &utc );
        SystemTimeToTzSpecificLocalTime( NULL, &utc, &local );
        if (!GetDateFormatW( LOCALE_USER_DEFAULT, 0, &local, L"d MMMM", text, size )) text[0] = 0;
    }
}
