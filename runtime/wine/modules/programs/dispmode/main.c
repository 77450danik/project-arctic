/*
 * dispmode.exe - the display devices, their monitors and their modes
 *
 * What the Display control panel does, from a console: it lists what
 * EnumDisplayDevices and EnumDisplaySettings report and changes a mode with
 * ChangeDisplaySettingsEx. Used to test the path from a Windows program to
 * KMS without a window.
 *
 * usage: dispmode.exe [\\.\DISPLAYn] [WIDTHxHEIGHT[@HZ] | restore | windows | broadcast]
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

static const char *state_flags( DWORD flags )
{
    static char text[64];

    snprintf( text, sizeof(text), "%s%s%s", (flags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? "attached " : "",
              (flags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "primary " : "",
              (flags & DISPLAY_DEVICE_MIRRORING_DRIVER) ? "mirroring " : "" );
    return text;
}

static void print_modes( const WCHAR *device )
{
    DEVMODEW mode = { .dmSize = sizeof(mode) };
    DWORD width = 0, height = 0;

    for (DWORD i = 0; EnumDisplaySettingsExW( device, i, &mode, 0 ); i++)
    {
        if (mode.dmPelsWidth != width || mode.dmPelsHeight != height)
        {
            width = mode.dmPelsWidth;
            height = mode.dmPelsHeight;
            printf( "\n    %4lux%-4lu ", width, height );
        }
        printf( "%lu Hz  ", mode.dmDisplayFrequency );
    }
    printf( "\n" );
}

static void print_devices(void)
{
    DISPLAY_DEVICEW device = { .cb = sizeof(device) };

    for (DWORD i = 0; EnumDisplayDevicesW( NULL, i, &device, 0 ); i++)
    {
        DISPLAY_DEVICEW monitor = { .cb = sizeof(monitor) };
        DEVMODEW mode = { .dmSize = sizeof(mode) };

        printf( "%ls  %ls  %s\n", device.DeviceName, device.DeviceString, state_flags( device.StateFlags ) );
        for (DWORD k = 0; EnumDisplayDevicesW( device.DeviceName, k, &monitor, 0 ); k++)
            printf( "    monitor: %ls  %ls\n", monitor.DeviceString, monitor.DeviceID );
        if (EnumDisplaySettingsExW( device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0 ))
            printf( "    now: %lux%lu %lu Hz %lu bpp at %ld,%ld\n", mode.dmPelsWidth, mode.dmPelsHeight,
                    mode.dmDisplayFrequency, mode.dmBitsPerPel, mode.dmPosition.x, mode.dmPosition.y );
        print_modes( device.DeviceName );
    }
}

/* the toplevel windows in z-order, to see where the shell put itself after a
 * display change */
static BOOL CALLBACK print_window( HWND hwnd, LPARAM param )
{
    WCHAR class_name[64] = L"", text[64] = L"";
    RECT rect = {0};

    GetClassNameW( hwnd, class_name, ARRAYSIZE(class_name) );
    GetWindowTextW( hwnd, text, ARRAYSIZE(text) );
    GetWindowRect( hwnd, &rect );
    printf( "%p %-26ls %4ld,%-4ld %4ldx%-4ld style %08lx ex %08lx %s\"%ls\"\n", hwnd, class_name,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            (DWORD)GetWindowLongW( hwnd, GWL_STYLE ), (DWORD)GetWindowLongW( hwnd, GWL_EXSTYLE ),
            IsWindowVisible( hwnd ) ? "" : "hidden ", text );
    return TRUE;
}

/* the monitor's own name, the way the Display control panel asks for it */
static void print_monitor_names(void)
{
    UINT32 path_count = 0, mode_count = 0;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    LONG result;

    printf( "sizeof: path %u mode %u source name %u target name %u header %u\n",
            (UINT)sizeof(DISPLAYCONFIG_PATH_INFO), (UINT)sizeof(DISPLAYCONFIG_MODE_INFO),
            (UINT)sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME), (UINT)sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME),
            (UINT)sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER) );

    if ((result = GetDisplayConfigBufferSizes( QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count )))
    {
        printf( "GetDisplayConfigBufferSizes: %ld\n", result );
        return;
    }
    paths = malloc( path_count * sizeof(*paths) );
    modes = malloc( mode_count * sizeof(*modes) );
    if (!paths || !modes) return;

    if ((result = QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL )))
    {
        printf( "QueryDisplayConfig: %ld\n", result );
        return;
    }

    for (UINT32 i = 0; i < path_count; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {{ DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                                                     paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id }};
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {{ DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                                                     paths[i].targetInfo.adapterId, paths[i].targetInfo.id }};

        result = DisplayConfigGetDeviceInfo( &source.header );
        printf( "path %u: source %ls (%ld)\n", i, source.viewGdiDeviceName, result );
        result = DisplayConfigGetDeviceInfo( &target.header );
        printf( "path %u: monitor %ls (%ld)\n", i, target.monitorFriendlyDeviceName, result );
    }
    free( paths );
    free( modes );
}

static const char *result_text( LONG result )
{
    switch (result)
    {
    case DISP_CHANGE_SUCCESSFUL: return "ok";
    case DISP_CHANGE_RESTART:    return "needs a restart";
    case DISP_CHANGE_BADMODE:    return "no such mode";
    case DISP_CHANGE_NOTUPDATED: return "not written to the registry";
    case DISP_CHANGE_BADFLAGS:   return "bad flags";
    case DISP_CHANGE_BADPARAM:   return "bad parameter";
    default:                     return "failed";
    }
}

int wmain( int argc, WCHAR *argv[] )
{
    DEVMODEW mode = { .dmSize = sizeof(mode) };
    const WCHAR *device = NULL, *wanted = NULL;
    unsigned int width, height, hz = 0;
    LONG result;

    for (int i = 1; i < argc; i++)
    {
        if (!wcsncmp( argv[i], L"\\\\.\\DISPLAY", 10 )) device = argv[i];
        else wanted = argv[i];
    }

    if (!wanted)
    {
        print_devices();
        return 0;
    }

    if (!wcscmp( wanted, L"names" ))
    {
        print_monitor_names();
        return 0;
    }

    if (!wcscmp( wanted, L"windows" ))
    {
        EnumWindows( print_window, 0 );
        return 0;
    }

    if (!wcscmp( wanted, L"broadcast" ))
    {
        if (!EnumDisplaySettingsExW( NULL, ENUM_CURRENT_SETTINGS, &mode, 0 )) return 1;
        SendMessageTimeoutW( HWND_BROADCAST, WM_DISPLAYCHANGE, mode.dmBitsPerPel,
                             MAKELPARAM( mode.dmPelsWidth, mode.dmPelsHeight ), SMTO_ABORTIFHUNG, 2000, NULL );
        printf( "told every window about %lux%lu\n", mode.dmPelsWidth, mode.dmPelsHeight );
        return 0;
    }

    if (!wcscmp( wanted, L"restore" ))
    {
        result = ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
        printf( "restore: %s (%ld)\n", result_text( result ), result );
        return result != DISP_CHANGE_SUCCESSFUL;
    }

    if (swscanf( wanted, L"%ux%u@%u", &width, &height, &hz ) < 2)
    {
        printf( "usage: dispmode.exe [\\\\.\\DISPLAYn] [WIDTHxHEIGHT[@HZ] | restore]\n" );
        return 1;
    }

    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmBitsPerPel = 32;
    if (hz)
    {
        mode.dmFields |= DM_DISPLAYFREQUENCY;
        mode.dmDisplayFrequency = hz;
    }

    result = ChangeDisplaySettingsExW( device, &mode, NULL, CDS_UPDATEREGISTRY, NULL );
    printf( "%ls %ux%u@%u: %s (%ld)\n", device ? device : L"the primary monitor", width, height, hz,
            result_text( result ), result );
    if (result == DISP_CHANGE_SUCCESSFUL) print_devices();
    return result != DISP_CHANGE_SUCCESSFUL;
}
