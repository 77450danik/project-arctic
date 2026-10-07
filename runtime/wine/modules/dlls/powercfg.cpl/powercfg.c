/*
 * Power Options applet (powercfg.cpl)
 *
 * As in Windows, "control powercfg.cpl" and the battery icon's menu open
 * Power Options, the Control Panel's folder of powercpl.dll, in File
 * Explorer; "powercfg.cpl,,battery" and ",,graphics" open Arctic's pages of
 * the batteries and graphics cards and of the graphics settings. The applet
 * itself is not listed in the Control Panel ("don't load"): the folder is.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "cpl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powercfg);

static HINSTANCE instance;

static void open_power_options( const WCHAR *page )
{
    HMODULE module = LoadLibraryW( L"powercpl.dll" );
    BOOL (WINAPI *open)( const WCHAR * ) = module ? (void *)GetProcAddress( module, "ArcticOpenPowerOptions" ) : NULL;

    if (!open || !open( page )) ERR( "cannot open Power Options\n" );
}

LONG CALLBACK CPlApplet( HWND hwnd, UINT msg, LPARAM lparam1, LPARAM lparam2 )
{
    switch (msg)
    {
    case CPL_INIT:
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_INQUIRE:
    {
        CPLINFO *info = (CPLINFO *)lparam2;
        info->idIcon = 1;
        info->idName = 1;
        info->idInfo = 2;
        info->lData = 0;
        return 0;
    }
    case CPL_STARTWPARMSW:
        open_power_options( (const WCHAR *)lparam2 );
        return TRUE;
    case CPL_DBLCLK:
        open_power_options( NULL );
        return 0;
    }
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        instance = inst;
        DisableThreadLibraryCalls( inst );
    }
    return TRUE;
}
