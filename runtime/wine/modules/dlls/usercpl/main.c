/*
 * User Accounts of the Control Panel (usercpl.dll)
 *
 * Windows 7's User Accounts page ({60632754-C523-4B62-B45C-4172DA012619}):
 * the account of the person using the computer, and the way to the settings
 * of User Account Control (page_users.c), drawn by the shared Control Panel
 * view (cpanel.c, cpfolder.c of systemcpl). What Arctic's User Account
 * Control guards today is the Linux subsystem: docs/linux-gate.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "usercpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0x60632754, 0xc523, 0x4b62, { 0xb4, 0x5c, 0x41, 0x72, 0xda, 0x01, 0x26, 0x19 } };
const WCHAR cp_class_name[] = L"UserAccounts";

const struct cp_page cp_pages[] =
{
    { IDS_USER_ACCOUNTS, L"pageUserAccounts", users_build },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* rundll32 usercpl.dll,ShowUserAccounts: the page in a new Explorer window */
void WINAPI ShowUserAccounts( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
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
