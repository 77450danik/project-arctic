/*
 * Programs and Features of the Control Panel: the programs
 *
 * The Uninstall keys as Wine's appwiz.cpl read them: HKLM's, its 32-bit view
 * on a 64-bit system, HKCU's; a system component is not listed, a program of
 * Windows Installer is removed and changed by msiexec. What Windows 7 shows
 * of it besides: when it was installed, its size, its links and where it is,
 * whether it repairs; an update of another program is listed apart, as
 * Windows lists installed updates.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "programscpl.h"
#include "shellapi.h"
#include "shlwapi.h"

#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

static WCHAR *reg_string( HKEY key, const WCHAR *name )
{
    DWORD type, size = 0;
    WCHAR *value;

    if (RegQueryValueExW( key, name, NULL, &type, NULL, &size ) || (type != REG_SZ && type != REG_EXPAND_SZ) || !size)
        return NULL;
    if (!(value = calloc( 1, size + sizeof(WCHAR) ))) return NULL;
    RegQueryValueExW( key, name, NULL, NULL, (BYTE *)value, &size );
    if (!value[0])
    {
        free( value );
        return NULL;
    }
    if (type == REG_EXPAND_SZ)
    {
        WCHAR expanded[MAX_PATH * 2];

        if (ExpandEnvironmentStringsW( value, expanded, ARRAY_SIZE(expanded) ))
        {
            free( value );
            value = wcsdup( expanded );
        }
    }
    return value;
}

/* a DWORD, or a string some installers write instead ("1") */
static BOOL reg_flag( HKEY key, const WCHAR *name )
{
    DWORD type, value = 0, size = sizeof(value);
    WCHAR text[16];

    if (!RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size ) && type == REG_DWORD) return value != 0;
    size = sizeof(text);
    if (!RegQueryValueExW( key, name, NULL, &type, (BYTE *)text, &size ) && type == REG_SZ) return text[0] == '1';
    return FALSE;
}

static DWORD reg_dword( HKEY key, const WCHAR *name )
{
    DWORD type, value = 0, size = sizeof(value);

    if (RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size ) || type != REG_DWORD) return 0;
    return value;
}

static WCHAR *command( const WCHAR *format, const WCHAR *code )
{
    WCHAR *text = malloc( (wcslen( format ) + wcslen( code ) + 1) * sizeof(WCHAR) );

    if (text) swprintf( text, wcslen( format ) + wcslen( code ) + 1, format, code );
    return text;
}

static void read_root( struct apps *apps, HKEY root, REGSAM view, UINT *capacity )
{
    WCHAR name[256];
    HKEY uninstall;
    DWORD length;

    if (RegOpenKeyExW( root, UNINSTALL_KEY, 0, KEY_READ | view, &uninstall )) return;
    for (DWORD i = 0; length = ARRAY_SIZE(name), !RegEnumKeyExW( uninstall, i, name, &length, NULL, NULL, NULL, NULL ); i++)
    {
        struct app *app;
        WCHAR *display, *release;
        BOOL msi;
        HKEY key;

        if (RegOpenKeyExW( uninstall, name, 0, KEY_READ, &key )) continue;
        if (reg_dword( key, L"SystemComponent" ) == 1 || !(display = reg_string( key, L"DisplayName" )))
        {
            RegCloseKey( key );
            continue;
        }
        if (apps->count == *capacity)
        {
            struct app *list = realloc( apps->list, (*capacity = *capacity ? *capacity * 2 : 64) * sizeof(*list) );
            if (!list)
            {
                free( display );
                RegCloseKey( key );
                break;
            }
            apps->list = list;
        }
        app = &apps->list[apps->count];
        memset( app, 0, sizeof(*app) );
        app->name = display;
        msi = reg_dword( key, L"WindowsInstaller" ) == 1;
        if (msi) app->uninstall = command( L"msiexec /x%s", name );
        else app->uninstall = reg_string( key, L"UninstallString" );
        if (reg_flag( key, L"NoRemove" ))
        {
            free( app->uninstall );
            app->uninstall = NULL;
        }
        if (!reg_flag( key, L"NoModify" ))
            app->modify = msi ? command( L"msiexec /i%s", name ) : reg_string( key, L"ModifyPath" );
        if (msi && !reg_flag( key, L"NoRepair" )) app->repair = command( L"msiexec /f%s", name );
        /* nothing to remove, change or repair it with: Windows does not list it */
        if (!app->uninstall && !app->modify && !app->repair)
        {
            free( app->name );
            RegCloseKey( key );
            continue;
        }

        if ((app->icon = reg_string( key, L"DisplayIcon" )))
        {
            WCHAR *comma = wcsrchr( app->icon, ',' );

            if (comma)
            {
                *comma = 0;
                app->icon_index = wcstol( comma + 1, NULL, 10 );
            }
            PathUnquoteSpacesW( app->icon );
        }
        app->publisher = reg_string( key, L"Publisher" );
        app->version = reg_string( key, L"DisplayVersion" );
        app->contact = reg_string( key, L"Contact" );
        app->help_link = reg_string( key, L"HelpLink" );
        app->phone = reg_string( key, L"HelpTelephone" );
        app->readme = reg_string( key, L"Readme" );
        app->update_link = reg_string( key, L"URLUpdateInfo" );
        app->about_link = reg_string( key, L"URLInfoAbout" );
        app->comments = reg_string( key, L"Comments" );
        app->location = reg_string( key, L"InstallLocation" );
        app->size_kb = reg_dword( key, L"EstimatedSize" );
        {
            WCHAR *date = reg_string( key, L"InstallDate" );
            int year, month, day;

            if (date && swscanf( date, L"%4d%2d%2d", &year, &month, &day ) == 3 && year > 1980 && month >= 1 &&
                month <= 12 && day >= 1 && day <= 31)
            {
                app->installed.wYear = year;
                app->installed.wMonth = month;
                app->installed.wDay = day;
            }
            free( date );
        }
        /* an update of another program, as Windows lists them apart */
        release = reg_string( key, L"ReleaseType" );
        app->update = RegQueryValueExW( key, L"ParentKeyName", NULL, NULL, NULL, NULL ) == ERROR_SUCCESS ||
                      (release && (wcsstr( release, L"Update" ) || wcsstr( release, L"Hotfix" )));
        free( release );
        app->root = root;
        app->view = view;
        lstrcpynW( app->key, name, ARRAY_SIZE(app->key) );
        apps->count++;
        RegCloseKey( key );
    }
    RegCloseKey( uninstall );
}

void apps_free( struct apps *apps )
{
    for (UINT i = 0; i < apps->count; i++)
    {
        struct app *app = &apps->list[i];

        free( app->name );
        free( app->publisher );
        free( app->version );
        free( app->contact );
        free( app->help_link );
        free( app->phone );
        free( app->readme );
        free( app->update_link );
        free( app->about_link );
        free( app->comments );
        free( app->location );
        free( app->uninstall );
        free( app->modify );
        free( app->repair );
        free( app->icon );
    }
    free( apps->list );
    apps->list = NULL;
    apps->count = 0;
}

void apps_load( struct apps *apps )
{
    UINT capacity = 0;
    BOOL wow64 = FALSE;

    apps_free( apps );
    read_root( apps, HKEY_LOCAL_MACHINE, sizeof(void *) > sizeof(int) ? KEY_WOW64_64KEY : 0, &capacity );
    /* the 32-bit programs of a 64-bit system have keys of their own */
    if (sizeof(void *) > sizeof(int) || (IsWow64Process( GetCurrentProcess(), &wow64 ) && wow64))
        read_root( apps, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, &capacity );
    read_root( apps, HKEY_CURRENT_USER, 0, &capacity );
}

/* the command line as Windows runs it: a program that asks to be run as an
 * administrator goes through UAC */
HANDLE app_run( HWND owner, struct app *app, const WCHAR *line )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR *copy;
    BOOL ok;

    if (!line || !(copy = wcsdup( line ))) return NULL;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    ok = CreateProcessW( NULL, copy, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi );
    free( copy );
    if (ok)
    {
        CloseHandle( pi.hThread );
        return pi.hProcess;
    }
    if (GetLastError() == ERROR_ELEVATION_REQUIRED)
    {
        SHELLEXECUTEINFOW info = { sizeof(info) };
        WCHAR file[MAX_PATH * 2];
        const WCHAR *args = PathGetArgsW( line );

        lstrcpynW( file, line, min( ARRAY_SIZE(file), (size_t)(args - line) + 1 ) );
        PathRemoveBlanksW( file );
        PathUnquoteSpacesW( file );
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner;
        info.lpVerb = L"runas";
        info.lpFile = file;
        info.lpParameters = args;
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW( &info )) return info.hProcess;
        if (GetLastError() == ERROR_CANCELLED) return NULL;
    }
    /* as appwiz.cpl: a program that is gone leaves its key; the user may take it away */
    if (MessageBoxW( GetAncestor( owner, GA_ROOT ), format_string( IDS_RUN_FAILED, line ), app->name,
                     MB_YESNO | MB_ICONQUESTION ) == IDYES)
    {
        HKEY uninstall;

        if (!RegOpenKeyExW( app->root, UNINSTALL_KEY, 0, KEY_ALL_ACCESS | app->view, &uninstall ))
        {
            RegDeleteTreeW( uninstall, app->key );
            RegCloseKey( uninstall );
        }
        return INVALID_HANDLE_VALUE;
    }
    return NULL;
}
