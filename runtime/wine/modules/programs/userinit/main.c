/*
 * Userinit Logon Application
 *
 * Runs once per logon: starts the shell named in Winlogon\Shell (the
 * user's value first, then the machine's) in the user's profile, and ends.
 *
 * Before it, the folders of the developer tools Arctic ships as Windows
 * programs (docs/linux-gate.md, stage 7) go on the system PATH when they are
 * on the disk and not on it yet, as their installers do: an image brings
 * them, and so does an update of an installed system, which runs no
 * installer. A PATH the user changed keeps everything else.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(userinit);

static const WCHAR winlogon_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
static const WCHAR environment_key[] = L"System\\CurrentControlSet\\Control\\Session Manager\\Environment";

/* under Program Files */
static const WCHAR *const tool_dirs[] = { L"PowerShell\\7", L"Git\\cmd", L"GitHub CLI" };

/* whether the list holds the folder, its variables expanded, with or
 * without a closing backslash */
static BOOL path_has( const WCHAR *list, const WCHAR *dir )
{
    size_t dir_len = wcslen( dir );
    WCHAR *copy = _wcsdup( list ), *next, *item;
    BOOL found = FALSE;

    for (item = wcstok_s( copy, L";", &next ); item && !found; item = wcstok_s( NULL, L";", &next ))
    {
        WCHAR expanded[MAX_PATH * 2];
        size_t len;

        if (!ExpandEnvironmentStringsW( item, expanded, ARRAY_SIZE(expanded) )) continue;
        len = wcslen( expanded );
        while (len && expanded[len - 1] == '\\') len--;
        found = len == dir_len && !_wcsnicmp( expanded, dir, len );
    }
    free( copy );
    return found;
}

static void add_tool_paths(void)
{
    WCHAR program_files[MAX_PATH], dir[MAX_PATH], *path, *process_path;
    DWORD size = 0, type, len;
    BOOL changed = FALSE;
    HKEY key;

    if (!GetEnvironmentVariableW( L"ProgramW6432", program_files, MAX_PATH ) &&
        !GetEnvironmentVariableW( L"ProgramFiles", program_files, MAX_PATH ))
        return;
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, environment_key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key )) return;
    RegQueryValueExW( key, L"Path", NULL, &type, NULL, &size );
    size += (ARRAY_SIZE(tool_dirs) * (MAX_PATH + 1) + 1) * sizeof(WCHAR);
    path = calloc( 1, size );
    if (RegQueryValueExW( key, L"Path", NULL, &type, (BYTE *)path, &size )) path[0] = 0;

    len = GetEnvironmentVariableW( L"PATH", NULL, 0 );
    process_path = calloc( len + ARRAY_SIZE(tool_dirs) * (MAX_PATH + 1) + 1, sizeof(WCHAR) );
    GetEnvironmentVariableW( L"PATH", process_path, len );

    for (unsigned int i = 0; i < ARRAY_SIZE(tool_dirs); i++)
    {
        swprintf( dir, ARRAY_SIZE(dir), L"%s\\%s", program_files, tool_dirs[i] );
        if (GetFileAttributesW( dir ) == INVALID_FILE_ATTRIBUTES) continue;
        if (!path_has( path, dir ))
        {
            if (*path && path[wcslen( path ) - 1] != ';') wcscat( path, L";" );
            wcscat( path, dir );
            changed = TRUE;
            TRACE( "%s goes on the system PATH\n", debugstr_w(dir) );
        }
        /* the shell and everything it starts get it now, not at the next logon */
        if (!path_has( process_path, dir ))
        {
            if (*process_path && process_path[wcslen( process_path ) - 1] != ';') wcscat( process_path, L";" );
            wcscat( process_path, dir );
        }
    }
    if (changed && RegSetValueExW( key, L"Path", 0, REG_EXPAND_SZ, (BYTE *)path, (wcslen( path ) + 1) * sizeof(WCHAR) ))
        ERR( "the system PATH could not be written\n" );
    SetEnvironmentVariableW( L"PATH", process_path );
    RegCloseKey( key );
    free( process_path );
    free( path );
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WCHAR shell[MAX_PATH] = L"explorer.exe", profile[MAX_PATH];
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD size = sizeof(shell);

    add_tool_paths();

    if (RegGetValueW( HKEY_CURRENT_USER, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size ))
    {
        size = sizeof(shell);
        RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size );
    }
    if (!GetEnvironmentVariableW( L"USERPROFILE", profile, MAX_PATH )) GetWindowsDirectoryW( profile, MAX_PATH );

    if (!CreateProcessW( NULL, shell, NULL, NULL, FALSE, 0, NULL, profile, &si, &pi ))
    {
        ERR( "cannot start the shell %s: %lu\n", debugstr_w(shell), GetLastError() );
        return 1;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return 0;
}
