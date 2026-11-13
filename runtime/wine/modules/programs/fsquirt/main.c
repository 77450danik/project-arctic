/*
 * fsquirt.exe: Bluetooth File Transfer
 *
 * As Windows': what Send to → "Пристрій Bluetooth" runs with the files
 * chosen, and what runs alone to choose them. The window is
 * devicescpl.dll's (Devices and Printers), the sending winebth.sys's.
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
#include "shellapi.h"

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE previous, WCHAR *cmdline, int show )
{
    HMODULE module = LoadLibraryW( L"devicescpl.dll" );
    void (WINAPI *send_files)( HWND, int, WCHAR ** ) =
        module ? (void *)GetProcAddress( module, "ArcticSendFiles" ) : NULL;
    WCHAR **argv;
    int argc;

    if (!send_files) return 1;
    argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    /* the files Send to gave: every argument but the program's own name */
    send_files( NULL, argv && argc > 1 ? argc - 1 : 0, argv ? argv + 1 : NULL );
    LocalFree( argv );
    return 0;
}
