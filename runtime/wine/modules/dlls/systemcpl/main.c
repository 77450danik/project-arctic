/*
 * System of the Control Panel (systemcpl.dll)
 *
 * Windows 7's System page ({BB06C0E4-D293-4f75-8A90-CB05B6477EEE}): what
 * the computer is, as a page of the Control Panel inside Explorer
 * (page_system.c), drawn by the shared Control Panel view (cpanel.c,
 * cpfolder.c). Win+Pause and "Властивості" of This PC open it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "systemcpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0xbb06c0e4, 0xd293, 0x4f75, { 0x8a, 0x90, 0xcb, 0x05, 0xb6, 0x47, 0x7e, 0xee } };
const WCHAR cp_class_name[] = L"System";

const struct cp_page cp_pages[] =
{
    { IDS_SYSTEM, L"pageSystemHub", system_build },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* rundll32 systemcpl.dll,ShowSystem: the page in a new Explorer window */
void WINAPI ShowSystem( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
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
