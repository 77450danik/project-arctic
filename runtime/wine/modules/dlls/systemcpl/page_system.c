/*
 * System of the Control Panel: "Перегляд загальних відомостей про комп’ютер"
 *
 * Windows 7's page (systemcpl.dll's UIFILE 1001): the edition with its logo
 * on the right; the system: rating, processor, memory, system type, pen and
 * touch; the computer's name, full name, description and workgroup with
 * "Змінити настройки" on the right; activation. Sections are 20 in from
 * their header, a label takes 150 and its value stands beside it, 7 between
 * rows. The task pane: Device Manager and System Properties' Advanced page
 * (sysdm.cpl, ReactOS's with the pages of Windows 7), with the shield.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "systemcpl.h"
#include "shlwapi.h"

#define INDENT       px(20)
#define LABEL_WIDTH  px(150)
#define ROW_GAP      px(7)

static void add_row( struct view *view, const WCHAR *label, const WCHAR *value )
{
    view_pair( view, label, value, INDENT, LABEL_WIDTH );
    view_space( view, ROW_GAP );
}

static BOOL reg_string( HKEY root, const WCHAR *path, const WCHAR *name, WCHAR *value, DWORD count )
{
    DWORD size = count * sizeof(WCHAR);

    if (RegGetValueW( root, path, name, RRF_RT_REG_SZ, NULL, value, &size )) return FALSE;
    return value[0] != 0;
}

/**********************************************************************
 *          What the page says
 */

static void processor_text( WCHAR *text, size_t count )
{
    static const WCHAR cpu_key[] = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    WCHAR name[128] = L"", speed[64] = L"", number[32], *start;
    DWORD mhz = 0, size = sizeof(mhz), packages = 0, length = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = NULL;
    NUMBERFMTW format = { 2, 1, 3, (WCHAR *)L",", (WCHAR *)L" ", 1 };

    if (!reg_string( HKEY_LOCAL_MACHINE, cpu_key, L"ProcessorNameString", name, ARRAY_SIZE(name) ))
        lstrcpynW( name, load_string( IDS_UNAVAILABLE ), ARRAY_SIZE(name) );
    for (start = name; *start == ' '; start++);
    memmove( name, start, (wcslen( start ) + 1) * sizeof(WCHAR) );

    if (!RegGetValueW( HKEY_LOCAL_MACHINE, cpu_key, L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &size ) && mhz)
    {
        WCHAR raw[32];
        swprintf( raw, ARRAY_SIZE(raw), L"%u.%02u", mhz / 1000, (mhz % 1000 + 5) / 10 % 100 );
        if (!GetNumberFormatEx( LOCALE_NAME_USER_DEFAULT, 0, raw, &format, number, ARRAY_SIZE(number) ))
            lstrcpynW( number, raw, ARRAY_SIZE(number) );
        swprintf( speed, ARRAY_SIZE(speed), load_string( IDS_GHZ ), number );
    }

    /* "(2 процесори)" only on a machine with more than one package */
    GetLogicalProcessorInformation( NULL, &length );
    if (length && (info = malloc( length )) && GetLogicalProcessorInformation( info, &length ))
        for (DWORD i = 0; i < length / sizeof(*info); i++)
            if (info[i].Relationship == RelationProcessorPackage) packages++;
    free( info );

    if (packages > 1)
    {
        WCHAR count_text[16];
        swprintf( count_text, ARRAY_SIZE(count_text), L"%u", packages );
        lstrcpynW( text, format_string( IDS_PROCESSORS_SPEED, name, speed, count_text ), count );
    }
    else if (speed[0]) lstrcpynW( text, format_string( IDS_PROCESSOR_SPEED, name, speed ), count );
    else lstrcpynW( text, name, count );
}

static void memory_text( WCHAR *text, size_t count )
{
    MEMORYSTATUSEX status = { sizeof(status) };
    ULONGLONG total, step, installed;
    WCHAR installed_text[32], total_text[32];

    GlobalMemoryStatusEx( &status );
    total = status.ullTotalPhys;
    /* what is installed is what the modules hold, the kernel keeps some of it
     * for itself: whole gigabytes (a quarter of one on small machines) */
    step = total >= ((ULONGLONG)2 << 30) ? (ULONGLONG)1 << 30 : (ULONGLONG)256 << 20;
    installed = (total + step - 1) / step * step;
    StrFormatByteSizeW( installed, installed_text, ARRAY_SIZE(installed_text) );
    StrFormatByteSizeW( status.ullTotalPhys, total_text, ARRAY_SIZE(total_text) );
    if (installed - status.ullTotalPhys < step / 64) lstrcpynW( text, installed_text, count );
    else swprintf( text, count, load_string( IDS_MEMORY_AVAILABLE ), installed_text, total_text );
}

static UINT system_type(void)
{
    SYSTEM_INFO info;
    BOOL wow64 = FALSE;

    GetNativeSystemInfo( &info );
    IsWow64Process( GetCurrentProcess(), &wow64 );
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: return sizeof(void *) == 8 || wow64 ? IDS_TYPE_64_X64 : IDS_TYPE_32_X64;
    case PROCESSOR_ARCHITECTURE_ARM64: return IDS_TYPE_64_ARM;
    case PROCESSOR_ARCHITECTURE_ARM: return IDS_TYPE_32_ARM;
    default: return IDS_TYPE_32_X86;
    }
}

static const WCHAR *pen_touch_text(void)
{
    int digitizer = GetSystemMetrics( SM_DIGITIZER ), touches = GetSystemMetrics( SM_MAXIMUMTOUCHES );
    BOOL pen = digitizer & NID_INTEGRATED_PEN, touch = digitizer & (NID_INTEGRATED_TOUCH | NID_MULTI_INPUT);
    WCHAR points[16];

    swprintf( points, ARRAY_SIZE(points), L"%d", touches );
    if (!(digitizer & NID_READY) || (!pen && !touch)) return load_string( IDS_NO_PEN_TOUCH );
    if (pen && touch) return touches > 1 ? format_string( IDS_PEN_TOUCH_POINTS, points ) : load_string( IDS_PEN_SINGLE_TOUCH );
    if (pen) return load_string( IDS_PEN );
    return touches > 1 ? format_string( IDS_TOUCH_POINTS, points ) : load_string( IDS_SINGLE_TOUCH );
}

/**********************************************************************
 *          The logo
 */

static HBITMAP logo_bitmap( SIZE *size )
{
    static HBITMAP logo;
    static SIZE logo_size;

    if (!logo)
    {
        UINT dpi = GetDpiForSystem(), id = dpi >= 192 ? IDB_LOGO_200 : dpi >= 144 ? IDB_LOGO_150 :
                                          dpi >= 120 ? IDB_LOGO_125 : IDB_LOGO_100;
        BITMAP info;

        logo = LoadImageW( cp_instance, MAKEINTRESOURCEW( id ), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION );
        if (logo && GetObjectW( logo, sizeof(info), &info ))
        {
            logo_size.cx = info.bmWidth;
            logo_size.cy = info.bmHeight;
        }
    }
    *size = logo_size;
    return logo;
}

/* on the right of the edition's lines, its top with theirs */
static void paint_logo( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HBITMAP logo;
    HDC mem;
    SIZE size;

    if (!(logo = logo_bitmap( &size )) || !(mem = CreateCompatibleDC( hdc ))) return;
    SelectObject( mem, logo );
    GdiAlphaBlend( hdc, rect->right - size.cx, rect->top - px( 6 ), size.cx, size.cy, mem, 0, 0, size.cx, size.cy, blend );
    DeleteDC( mem );
}

/**********************************************************************
 *          The links
 */

static void open_properties( struct view *view, UINT_PTR page )
{
    WCHAR args[64];

    swprintf( args, ARRAY_SIZE(args), L"sysdm.cpl,,%u", (UINT)page );
    cp_run( L"control.exe", args );
}

static void open_device_manager( struct view *view, UINT_PTR param )
{
    cp_run( L"rundll32.exe", L"devmgr.dll,DeviceManager_ExecuteW" );
}

/**********************************************************************
 *          The page
 */

void system_build( struct view *view )
{
    WCHAR text[512], value[256], version[64] = L"", build[32] = L"", date[32] = L"", year[8] = L"2026";
    DWORD size;
    SIZE logo_size;

    view_nav_home( view );
    view_nav_link( view, load_string( IDS_DEVICE_MANAGER ), open_device_manager, 0 );
    /* Windows 7 has "Настройки віддаленого підключення" and "Захист системи"
     * here too: Arctic has neither a remote desktop server nor restore points */
    view_nav_icon_link( view, cp_shield_icon(), load_string( IDS_ADVANCED_SETTINGS ), open_properties, 2 );

    view_text( view, STYLE_TITLE, load_string( IDS_TITLE ) );
    view_space( view, px( 12 ) );

    /* Випуск: the name, the copyright, the build */
    view_group( view, load_string( IDS_EDITION ) );
    view_paint_area( view, 0, paint_logo, 0 );
    reg_string( HKEY_LOCAL_MACHINE, L"Software\\Arctic", L"Version", version, ARRAY_SIZE(version) );
    reg_string( HKEY_LOCAL_MACHINE, L"Software\\Arctic", L"Build", build, ARRAY_SIZE(build) );
    if (reg_string( HKEY_LOCAL_MACHINE, L"Software\\Arctic", L"BuildDate", date, ARRAY_SIZE(date) ))
        lstrcpynW( year, date, 5 );
    swprintf( text, ARRAY_SIZE(text), L"Arctic %s", version[0] ? version : L"pre-alpha" );
    view_text_at( view, STYLE_BODY, text, INDENT );
    view_space( view, px( 8 ) );
    view_text_at( view, STYLE_BODY, format_string( IDS_COPYRIGHT, year ), INDENT );
    view_space( view, px( 8 ) );
    if (build[0])
    {
        view_text_at( view, STYLE_BODY, format_string( IDS_BUILD, build, date ), INDENT );
        view_space( view, px( 8 ) );
    }
    /* the logo is taller than the lines beside it */
    logo_bitmap( &logo_size );
    view_space( view, max( 0, logo_size.cy - px( 70 ) ) + px( 10 ) );

    /* Система */
    view_group( view, load_string( IDS_SYSTEM_GROUP ) );
    add_row( view, load_string( IDS_RATING ), load_string( IDS_RATING_UNAVAILABLE ) );
    processor_text( text, ARRAY_SIZE(text) );
    add_row( view, load_string( IDS_PROCESSOR ), text );
    memory_text( text, ARRAY_SIZE(text) );
    add_row( view, load_string( IDS_MEMORY ), text );
    add_row( view, load_string( IDS_SYSTEM_TYPE ), load_string( system_type() ) );
    add_row( view, load_string( IDS_PEN_TOUCH ), pen_touch_text() );
    view_space( view, px( 10 ) );

    /* Ім’я комп’ютера, домен і робоча група */
    view_group( view, load_string( IDS_NAME_GROUP ) );
    view_icon_link( view, cp_shield_icon(), load_string( IDS_CHANGE_SETTINGS ), open_properties, 0, 0, TRUE );
    size = ARRAY_SIZE(value);
    if (!GetComputerNameExW( ComputerNameDnsHostname, value, &size )) lstrcpyW( value, load_string( IDS_UNAVAILABLE ) );
    add_row( view, load_string( IDS_COMPUTER_NAME ), value );
    size = ARRAY_SIZE(value);
    if (!GetComputerNameExW( ComputerNameDnsFullyQualified, value, &size )) lstrcpyW( value, load_string( IDS_UNAVAILABLE ) );
    add_row( view, load_string( IDS_FULL_NAME ), value );
    if (!reg_string( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                     L"srvcomment", value, ARRAY_SIZE(value) ))
        value[0] = 0;
    add_row( view, load_string( IDS_DESCRIPTION ), value );
    add_row( view, load_string( IDS_WORKGROUP_LABEL ), load_string( IDS_WORKGROUP_DEFAULT ) );
    view_space( view, px( 10 ) );

    /* Активація */
    view_group( view, load_string( IDS_ACTIVATION ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_ACTIVATION_NOT_NEEDED ), INDENT );
    view_space( view, px( 8 ) );
    if (!reg_string( HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", L"ProductId", value,
                     ARRAY_SIZE(value) ))
        lstrcpyW( value, load_string( IDS_UNAVAILABLE ) );
    swprintf( text, ARRAY_SIZE(text), L"%s %s", load_string( IDS_PRODUCT_ID ), value );
    view_text_at( view, STYLE_BODY, text, INDENT );
}
