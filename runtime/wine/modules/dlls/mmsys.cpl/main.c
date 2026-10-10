/*
 * Sound of the Control Panel (mmsys.cpl)
 *
 * Windows 7's Sound: the playback and the recording devices (playback.c),
 * each with its properties: the name and whether it is used, the levels
 * with the balance of the channels, the default format (props.c,
 * balance.c); the sound scheme (sounds.c) and what communications do to
 * the other sounds (comm.c). The dialogs are Windows' own; what they set is
 * the Core Audio API's and mmdevapi's (devices.c). "control mmsys.cpl,,1"
 * opens on the second page, as in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "mmsys.h"
#include "cpl.h"
#include "prsht.h"

HINSTANCE mmsys_instance;

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][512];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( mmsys_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

/* every size is given at 96 DPI and drawn at the DPI of the system */
int px( int n )
{
    return MulDiv( n, GetDpiForSystem(), 96 );
}

static int CALLBACK sheet_callback( HWND sheet, UINT msg, LPARAM lp )
{
    if (msg == PSCB_INITIALIZED)
        SendMessageW( sheet, WM_SETICON, ICON_BIG,
                      (LPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_SOUND ), IMAGE_ICON,
                                          GetSystemMetrics( SM_CXICON ), GetSystemMetrics( SM_CYICON ), LR_SHARED ) );
    return 0;
}

static void show_sound( HWND owner, UINT start_page )
{
    static const struct { UINT dialog; DLGPROC proc; LPARAM param; } tabs[] =
    {
        { IDD_PLAYBACK, devices_proc, FALSE },
        { IDD_RECORDING, devices_proc, TRUE },
        { IDD_SOUNDS, sounds_proc, 0 },
        { IDD_COMMUNICATIONS, communications_proc, 0 },
    };
    PROPSHEETPAGEW pages[ARRAY_SIZE(tabs)];
    PROPSHEETHEADERW header = { sizeof(header) };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES };

    InitCommonControlsEx( &icc );
    memset( pages, 0, sizeof(pages) );
    for (UINT i = 0; i < ARRAY_SIZE(tabs); i++)
    {
        pages[i].dwSize = sizeof(pages[i]);
        pages[i].hInstance = mmsys_instance;
        pages[i].pszTemplate = MAKEINTRESOURCEW( tabs[i].dialog );
        pages[i].pfnDlgProc = tabs[i].proc;
        pages[i].lParam = tabs[i].param;
    }
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP | PSH_USEICONID | PSH_USECALLBACK;
    header.hwndParent = owner;
    header.hInstance = mmsys_instance;
    header.pszIcon = MAKEINTRESOURCEW( IDI_SOUND );
    header.pszCaption = load_string( IDS_SOUND );
    header.nPages = ARRAY_SIZE(tabs);
    header.nStartPage = start_page < ARRAY_SIZE(tabs) ? start_page : 0;
    header.ppsp = pages;
    header.pfnCallback = sheet_callback;
    PropertySheetW( &header );
}

LONG CALLBACK CPlApplet( HWND hwnd, UINT msg, LPARAM lp1, LPARAM lp2 )
{
    switch (msg)
    {
    case CPL_INIT:
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_INQUIRE:
    {
        CPLINFO *info = (CPLINFO *)lp2;

        info->idIcon = IDI_SOUND;
        info->idName = IDS_SOUND;
        info->idInfo = IDS_SOUND_TIP;
        info->lData = 0;
        return 0;
    }
    case CPL_DBLCLK:
        show_sound( hwnd, 0 );
        return 0;
    case CPL_STARTWPARMSW:
        show_sound( hwnd, lp2 ? wcstoul( (const WCHAR *)lp2, NULL, 10 ) : 0 );
        return TRUE;
    }
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        mmsys_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
