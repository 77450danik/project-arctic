/*
 * Programs and Features of the Control Panel (programscpl.dll)
 *
 * Windows' Programs and Features {7b81be6a-ce2b-4676-a29e-eb907a5126c5}: a
 * page of the Control Panel inside Explorer (page.c), not appwiz.cpl's
 * dialog; appwiz.cpl opens it (Wine patch), and stays itself for what else
 * it does (Wine Gecko and Mono). The applet is not listed beside it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "programscpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0x7b81be6a, 0xce2b, 0x4676, { 0xa2, 0x9e, 0xeb, 0x90, 0x7a, 0x51, 0x26, 0xc5 } };
const WCHAR cp_class_name[] = L"Programs";

const struct cp_page cp_pages[] =
{
    { IDS_NAME, L"pageProgramsAndFeatures", programs_build, programs_command },
    { IDS_UPDATES, L"pageInstalledUpdates", updates_build, programs_command },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* the item in a new Explorer window: 0 the programs, 1 the installed updates */
BOOL WINAPI ArcticProgramsOpen( UINT page )
{
    return cp_open( page < PAGE_COUNT ? page : 0, 0 );
}

/* rundll32 programscpl.dll,ShowPrograms */
void WINAPI ShowPrograms( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    cp_open( 0, 0 );
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
