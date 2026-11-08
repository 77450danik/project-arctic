/*
 * Date and Time of the Control Panel (timedate.cpl)
 *
 * Not Windows' dialog with its tabs but a page of the Control Panel inside
 * Explorer, as Power Options is (page.c), drawn by the shared Control Panel
 * view (cpanel.c, cpfolder.c): the time and its server, the date and time by
 * hand, the time zone. Windows' Date and Time ({E2E7934B-DCE5-43C4-9576-
 * 7FE4F75E7480}, Microsoft.DateAndTime) is this folder; "control timedate.cpl",
 * the clock's calendar and the taskbar's menu open it. The applet itself is
 * not listed beside it ("don't load").
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "timedate.h"
#include "cpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0xe2e7934b, 0xdce5, 0x43c4, { 0x95, 0x76, 0x7f, 0xe4, 0xf7, 0x5e, 0x74, 0x80 } };
const WCHAR cp_class_name[] = L"DateTime";

const struct cp_page cp_pages[] =
{
    { IDS_NAME, L"pageDateTime", datetime_build, datetime_command, datetime_timer },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* rundll32 timedate.cpl,ShowDateTime: the page in a new Explorer window */
void WINAPI ShowDateTime( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    cp_open( 0, 0 );
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
        info->idIcon = IDI_TIMEDATE;
        info->idName = IDS_NAME;
        info->idInfo = IDS_TIP;
        info->lData = 0;
        return 0;
    }
    case CPL_STARTWPARMSW:
        cp_open( 0, 0 );
        return TRUE;
    case CPL_DBLCLK:
        cp_open( 0, 0 );
        return 0;
    }
    return 0;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    return cp_class_object( clsid, riid, out );
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return cp_can_unload();
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        cp_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
