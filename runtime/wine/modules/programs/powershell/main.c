/*
 * powershell.exe: PowerShell 7 in place of Windows PowerShell
 *
 * Programs and agents call powershell.exe (System32 and
 * System32\WindowsPowerShell\v1.0), as on Windows; Wine's own is a stub that
 * runs nothing. This one runs C:\Program Files\PowerShell\7\pwsh.exe with the
 * same arguments, in the same console and with the same standard handles,
 * and gives back its exit code (docs/linux-gate.md, stage 7).
 *
 * The one difference of the command lines: Windows PowerShell takes its
 * first argument that is not a parameter as a command, PowerShell 7 as a
 * script file. "powershell Get-Date" stays a command here.
 *
 * Copyright 2026 Project Arctic
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* the parameters of powershell.exe that take a value, by the shortest
 * prefix PowerShell accepts for each */
static const struct { const WCHAR *name; size_t shortest; } with_value[] = {
    { L"executionpolicy", 2 }, { L"ex", 2 }, { L"ep", 2 },
    { L"inputformat", 2 }, { L"if", 2 },
    { L"outputformat", 1 }, { L"of", 2 },
    { L"windowstyle", 1 },
    { L"configurationname", 4 },
    { L"psconsolefile", 2 },
    { L"version", 1 },
    { L"encodedarguments", 8 }, { L"ea", 2 },
    { L"settingsfile", 2 },
    { L"workingdirectory", 2 }, { L"wd", 2 },
};

/* PowerShell's parameters are matched by any prefix of their name */
static BOOL is_param( const WCHAR *arg, const WCHAR *name, size_t shortest )
{
    size_t len = wcslen( arg );

    return len >= shortest && len <= wcslen( name ) && !_wcsnicmp( arg, name, len );
}

/* the parameter's name, without its "-" or "/", or NULL for an argument */
static const WCHAR *param_name( const WCHAR *arg )
{
    if ((arg[0] == '-' || arg[0] == '/') && arg[1]) return arg + (arg[1] == '-' ? 2 : 1);
    return NULL;
}

/* one argument, quoted as CommandLineToArgvW reads it back */
static void append_arg( WCHAR **line, size_t *len, size_t *size, const WCHAR *arg )
{
    size_t need = *len + wcslen( arg ) * 2 + 4;
    BOOL quote = !*arg || wcspbrk( arg, L" \t\"" );
    const WCHAR *p;

    if (need > *size)
    {
        *size = need * 2;
        *line = realloc( *line, *size * sizeof(WCHAR) );
    }
    if (*len) (*line)[(*len)++] = ' ';
    if (!quote)
    {
        wcscpy( *line + *len, arg );
        *len += wcslen( arg );
        return;
    }
    (*line)[(*len)++] = '"';
    for (p = arg; *p; p++)
    {
        size_t slashes = 0;

        while (*p == '\\') { slashes++; p++; }
        if (!*p)
        {
            while (slashes--) { (*line)[(*len)++] = '\\'; (*line)[(*len)++] = '\\'; }
            break;
        }
        if (*p == '"')
        {
            slashes = slashes * 2 + 1;
        }
        while (slashes--) (*line)[(*len)++] = '\\';
        (*line)[(*len)++] = *p;
    }
    (*line)[(*len)++] = '"';
    (*line)[*len] = 0;
}

static BOOL find_pwsh( WCHAR *path, DWORD size )
{
    WCHAR dir[MAX_PATH];

    if (!GetEnvironmentVariableW( L"ProgramW6432", dir, ARRAY_SIZE(dir) ) &&
        !GetEnvironmentVariableW( L"ProgramFiles", dir, ARRAY_SIZE(dir) ))
        wcscpy( dir, L"C:\\Program Files" );
    swprintf( path, size, L"%s\\PowerShell\\7\\pwsh.exe", dir );
    if (GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES) return TRUE;
    return SearchPathW( NULL, L"pwsh.exe", NULL, size, path, NULL ) != 0;
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    WCHAR pwsh[MAX_PATH], *line = NULL;
    size_t len = 0, size = 0;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    int i;

    if (!find_pwsh( pwsh, ARRAY_SIZE(pwsh) ))
    {
        fwprintf( stderr, L"powershell.exe: PowerShell 7 is not installed (C:\\Program Files\\PowerShell\\7\\pwsh.exe).\n" );
        return 1;
    }
    append_arg( &line, &len, &size, pwsh );

    for (i = 1; i < argc; i++)
    {
        const WCHAR *name = param_name( argv[i] );
        BOOL value = FALSE;
        size_t k;

        if (!name)
        {
            /* the command, and everything after it is its text */
            append_arg( &line, &len, &size, L"-Command" );
            break;
        }
        if (is_param( name, L"command", 1 ) || is_param( name, L"file", 1 ) ||
            is_param( name, L"encodedcommand", 1 ) || is_param( name, L"ec", 2 ))
            break;
        /* Windows PowerShell 2.0 does not come back: the version is dropped */
        if (is_param( name, L"version", 1 ))
        {
            if (i + 1 < argc) i++;
            continue;
        }
        for (k = 0; k < ARRAY_SIZE(with_value); k++)
            if (is_param( name, with_value[k].name, with_value[k].shortest )) value = TRUE;
        append_arg( &line, &len, &size, argv[i] );
        if (value && i + 1 < argc) append_arg( &line, &len, &size, argv[++i] );
    }
    for (; i < argc; i++) append_arg( &line, &len, &size, argv[i] );

    /* the same console and handles: the child answers Ctrl+C itself */
    GetStartupInfoW( &si );
    si.lpReserved2 = NULL;
    si.cbReserved2 = 0;
    SetConsoleCtrlHandler( NULL, TRUE );
    if (!CreateProcessW( pwsh, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
    {
        fwprintf( stderr, L"powershell.exe: %s: error %lu\n", pwsh, GetLastError() );
        return 1;
    }
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    free( line );
    return code;
}
