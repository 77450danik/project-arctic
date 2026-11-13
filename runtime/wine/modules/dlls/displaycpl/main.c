/*
 * Display of the Control Panel (displaycpl.dll)
 *
 * Windows' Display {C555438B-3C23-4769-A71F-B6D3D9B6053A} as a page of the
 * Control Panel inside Explorer, not desk.cpl's dialog, drawn as Windows
 * 11's Settings: Display (page_display.c), Advanced display
 * (page_advanced.c), Personalization (page_personalize.c), the desktop's
 * background (page_background.c), the desktop (page_desktop.c), the screen
 * saver (page_screensaver.c). desk.cpl opens it, by the number or the name
 * of its old tab (ArcticDisplayOpen); the applet itself is not listed beside
 * it ("don't load").
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "displaycpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0xc555438b, 0x3c23, 0x4769, { 0xa7, 0x1f, 0xb6, 0xd3, 0xd9, 0xb6, 0x05, 0x3a } };
const WCHAR cp_class_name[] = L"Display";

const struct cp_page cp_pages[] =
{
    { IDS_DISPLAY, L"pageDisplay", display_build, display_command },
    { IDS_ADVANCED, L"pageAdvancedDisplay", advanced_build, advanced_command },
    { IDS_PERSONALIZE, L"pagePersonalize", personalize_build, personalize_command },
    { IDS_BACKGROUND, L"pageBackground", background_build, background_command },
    { IDS_SCREENSAVER, L"pageScreenSaver", screensaver_build, screensaver_command },
    { IDS_DESKTOP, L"pageDesktop", desktop_build, desktop_command },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* the item in a new Explorer window: "display", "advanced", "personalize",
 * "background", "screensaver" or "desktop" */
BOOL WINAPI ArcticDisplayOpen( const WCHAR *page )
{
    static const WCHAR * const names[PAGE_COUNT] = { L"display", L"advanced", L"personalize", L"background",
                                                     L"screensaver", L"desktop" };
    UINT which = PAGE_DISPLAY;

    for (UINT i = 0; page && i < PAGE_COUNT; i++)
        if (!wcsicmp( page, names[i] )) which = i;
    return cp_open( which, 0 );
}

/* rundll32 displaycpl.dll,ShowDisplay [page] */
void WINAPI ShowDisplay( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    WCHAR page[32] = L"";

    if (cmdline) MultiByteToWideChar( CP_ACP, 0, cmdline, -1, page, ARRAY_SIZE(page) );
    ArcticDisplayOpen( page );
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
