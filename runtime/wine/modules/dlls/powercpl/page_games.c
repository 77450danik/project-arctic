/*
 * Games (Arctic's, docs/games.md): "Ігри" and "Продуктивність"
 *
 * A Control Panel item of its own ({5C3B7E41-...}, games.reg) drawn as Power
 * Options is: what MSI Afterburner, RivaTuner and Windows' Game Mode give,
 * for whatever this machine has.
 *
 * "Ігри": game mode (the host gives a game that fills a monitor the
 * priority), the frame rate limit (DXVK's, C:\ProgramData\Arctic\dxvk.conf),
 * the overlay (gameosd.exe: what it shows, where), the keys that show it and
 * open this page, and the frame log dwm writes only while asked to. Every
 * change counts at once.
 *
 * "Продуктивність": only what the host found this machine allows
 * (tuning-info.ini): Intel's voltage offsets, the processor's power limits
 * and top frequency, the graphics card's power limit, clock offsets and top
 * clock; and the temperatures, clocks and power now. The host applies it
 * (powrprof's "games" request) and holds it back after a start it did not
 * survive.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "powercpl.h"
#include "winnls.h"
#include "commctrl.h"
#include "shellapi.h"
#include "wine/arctic_games.h"

#define ID_GAME_MODE       100
#define ID_MAX_FPS         101
#define ID_OVERLAY         102
#define ID_OVERLAY_ALWAYS  103
#define ID_CORNER          104
#define ID_SCALE           105
#define ID_ITEM_FIRST      110   /* + bit */
#define ID_HOTKEY_OVERLAY  130
#define ID_HOTKEY_PANEL    131
#define ID_FRAME_LOG       132

#define ID_CORE_OFFSET     200
#define ID_CACHE_OFFSET    201
#define ID_PL1             202
#define ID_PL2             203
#define ID_CPU_MAX         204
#define ID_GPU_POWER       205
#define ID_GPU_CORE        206
#define ID_GPU_MEM         207
#define ID_GPU_MAX         208
#define ID_APPLY_AT_START  209
#define ID_APPLY           210
#define ID_RESET           211
#define ID_TURBO           212

#define LABEL_WIDTH        px(250)
#define COMBO_WIDTH        px(200)

/**********************************************************************
 *          Ігри
 */

static const int frame_rates[] = { 0, 30, 45, 60, 75, 90, 100, 120, 144, 165, 180, 240 };
static const int scales[] = { 100, 125, 150, 200 };
static const struct { int bit; UINT name; } items[] =
{
    { OSD_FPS, IDS_OSD_FPS }, { OSD_FRAME_TIME, IDS_OSD_FRAME_TIME }, { OSD_CPU_LOAD, IDS_OSD_CPU_LOAD },
    { OSD_CPU_MHZ, IDS_OSD_CPU_MHZ }, { OSD_CPU_TEMP, IDS_OSD_CPU_TEMP }, { OSD_CPU_POWER, IDS_OSD_CPU_POWER }, { OSD_CPU_VOLTAGE, IDS_OSD_CPU_VOLTAGE },
    { OSD_RAM, IDS_OSD_RAM }, { OSD_GPU_LOAD, IDS_OSD_GPU_LOAD }, { OSD_GPU_MHZ, IDS_OSD_GPU_MHZ },
    { OSD_GPU_TEMP, IDS_OSD_GPU_TEMP }, { OSD_GPU_MEMORY, IDS_OSD_GPU_MEMORY }, { OSD_GPU_POWER, IDS_OSD_GPU_POWER },
    { OSD_FRAME_GRAPH, IDS_OSD_FRAME_GRAPH },
};

static void go_page( struct view *view, UINT_PTR page )
{
    view_navigate( view, page, NULL, FALSE );
}

void view_browse_control_panel( struct view *view );

static void go_home( struct view *view, UINT_PTR param )
{
    view_browse_control_panel( view );
}

/* Windows 7's task pane: what a person wants done, leading to the page that does it */
static void nav_games_tasks( struct view *view )
{
    enum page page = view_state( view )->page;

    view_nav_link( view, load_string( IDS_CONTROL_PANEL_HOME ), go_home, 0 );
    if (page == PAGE_TUNING)
    {
        view_nav_link( view, load_string( IDS_TASK_SHOW_FPS ), go_page, PAGE_GAMES );
        view_nav_link( view, load_string( IDS_TASK_LIMIT_FPS ), go_page, PAGE_GAMES );
        view_nav_link( view, load_string( IDS_TASK_OVERLAY_KEY ), go_page, PAGE_GAMES );
    }
    else
    {
        view_nav_link( view, load_string( IDS_TASK_OVERCLOCK ), go_page, PAGE_TUNING );
        view_nav_link( view, load_string( IDS_TASK_UNDERVOLT ), go_page, PAGE_TUNING );
    }
    view_nav_link( view, load_string( IDS_TASK_GPU_FOR_GAME ), go_page, PAGE_GRAPHICS );
    view_nav_see_also( view, load_string( IDS_POWER_OPTIONS ), go_page, PAGE_PLANS );
    view_nav_see_also( view, load_string( IDS_TASK_BATTERY ), go_page, PAGE_BATTERY );
}

static void label_row_begin( struct view *view, UINT label )
{
    view_row_begin( view );
    view_text_at( view, STYLE_BODY, load_string( label ), 0 );
}

static HWND combo_beside( struct view *view, UINT id )
{
    return view_control_beside( view, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, LABEL_WIDTH,
                                COMBO_WIDTH, px( 300 ), id );
}

static void combo_add( HWND combo, const WCHAR *text, int value, int current )
{
    int index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );

    SendMessageW( combo, CB_SETITEMDATA, index, value );
    if (value == current) SendMessageW( combo, CB_SETCURSEL, index, 0 );
}

static int combo_value( struct view *view, UINT id, int fallback )
{
    HWND combo = GetDlgItem( view_window( view ), id );
    int index;

    if (!combo || (index = SendMessageW( combo, CB_GETCURSEL, 0, 0 )) < 0) return fallback;
    return SendMessageW( combo, CB_GETITEMDATA, index, 0 );
}

static BOOL checked( struct view *view, UINT id )
{
    HWND check = GetDlgItem( view_window( view ), id );

    return check && SendMessageW( check, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
}

static HWND check_box( struct view *view, UINT id, UINT text, BOOL on, int indent, int width )
{
    HWND check = view_control( view, WC_BUTTONW, load_string( text ), BS_AUTOCHECKBOX | WS_TABSTOP,
                               width ? width : view_content_width( view ) - indent, px( 20 ), indent, id );

    SendMessageW( check, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0 );
    return check;
}

/* DXVK's limit, for every game started after it */
static void write_frame_limit( int fps )
{
    char text[128];
    DWORD written;
    HANDLE file;
    int len;

    if (!fps)
    {
        DeleteFileW( ARCTIC_DXVK_CONF );
        return;
    }
    len = snprintf( text, sizeof(text), "dxgi.maxFrameRate = %d\r\nd3d9.maxFrameRate = %d\r\n", fps, fps );
    CreateDirectoryW( ARCTIC_GAMES_DIR, NULL );
    file = CreateFileW( ARCTIC_DXVK_CONF, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile( file, text, len, &written, NULL );
    CloseHandle( file );
}

static void set_frame_log( BOOL on )
{
    HANDLE file;

    if (!on)
    {
        DeleteFileW( ARCTIC_DWM_DIAGNOSTICS );
        return;
    }
    CreateDirectoryW( ARCTIC_GAMES_DIR, NULL );
    file = CreateFileW( ARCTIC_DWM_DIAGNOSTICS, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
}

/* the overlay reads the settings again; started when it has something to do */
static void tell_overlay( const struct arctic_games *games )
{
    HWND overlay = FindWindowW( ARCTIC_OVERLAY_CLASS, NULL );

    if (overlay) PostMessageW( overlay, WM_ARCTIC_GAMES_RELOAD, 0, 0 );
    else if (games->overlay || games->hotkey_overlay || games->hotkey_panel)
        ShellExecuteW( NULL, NULL, L"gameosd.exe", NULL, NULL, SW_SHOWNORMAL );
}

void games_build( struct view *view )
{
    struct arctic_games games;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_HOTKEY_CLASS };
    int column = LABEL_WIDTH;
    WCHAR text[64];
    HWND combo, hotkey;

    InitCommonControlsEx( &icc );
    arctic_games_read( &games );
    nav_games_tasks( view );
    view_text( view, STYLE_TITLE, load_string( IDS_GAMES_TITLE ) );
    view_space( view, px( 12 ) );

    view_group( view, load_string( IDS_GAME_MODE ) );
    view_space( view, px( 8 ) );
    check_box( view, ID_GAME_MODE, IDS_GAME_MODE, games.game_mode, 0, 0 );
    view_text_at( view, STYLE_SMALL, load_string( IDS_GAME_MODE_TEXT ), px( 18 ) );
    view_space( view, px( 16 ) );

    view_group( view, load_string( IDS_FRAME_RATE ) );
    view_space( view, px( 8 ) );
    label_row_begin( view, IDS_MAX_FPS );
    combo = combo_beside( view, ID_MAX_FPS );
    {
        /* the monitor's own rate is in the list too, in its place (a custom
         * 68 Hz between 60 and 75): a game capped at it runs no frame the
         * screen cannot show */
        DEVMODEW mode = { .dmSize = sizeof(mode) };
        int refresh = 0;

        if (EnumDisplaySettingsW( NULL, ENUM_REGISTRY_SETTINGS, &mode ) && mode.dmDisplayFrequency > 1)
            refresh = mode.dmDisplayFrequency;
        for (UINT i = 0; i < ARRAY_SIZE(frame_rates); i++)
        {
            if (refresh && frame_rates[i] && refresh < frame_rates[i] && (i < 2 || refresh > frame_rates[i - 1]))
            {
                swprintf( text, ARRAY_SIZE(text), L"%d", refresh );
                combo_add( combo, text, refresh, games.max_fps );
                refresh = 0;
            }
            if (frame_rates[i] == refresh) refresh = 0;
            if (frame_rates[i]) swprintf( text, ARRAY_SIZE(text), L"%d", frame_rates[i] );
            else lstrcpynW( text, load_string( IDS_NO_LIMIT ), ARRAY_SIZE(text) );
            combo_add( combo, text, frame_rates[i], games.max_fps );
        }
    }
    if (SendMessageW( combo, CB_GETCURSEL, 0, 0 ) < 0)
    {
        swprintf( text, ARRAY_SIZE(text), L"%d", games.max_fps );
        combo_add( combo, text, games.max_fps, games.max_fps );
    }
    view_row_end( view );
    view_text_at( view, STYLE_SMALL, load_string( IDS_MAX_FPS_TEXT ), 0 );
    view_space( view, px( 16 ) );

    view_group( view, load_string( IDS_OVERLAY ) );
    view_space( view, px( 8 ) );
    check_box( view, ID_OVERLAY, IDS_OVERLAY_SHOW, games.overlay, 0, 0 );
    check_box( view, ID_OVERLAY_ALWAYS, IDS_OVERLAY_ALWAYS, games.overlay_always, 0, 0 );
    view_space( view, px( 8 ) );
    label_row_begin( view, IDS_POSITION );
    combo = combo_beside( view, ID_CORNER );
    for (int i = 0; i < 4; i++) combo_add( combo, load_string( IDS_TOP_LEFT + i ), i, games.overlay_corner );
    view_row_end( view );
    view_space( view, px( 6 ) );
    label_row_begin( view, IDS_SIZE );
    combo = combo_beside( view, ID_SCALE );
    for (UINT i = 0; i < ARRAY_SIZE(scales); i++)
    {
        swprintf( text, ARRAY_SIZE(text), L"%d %%", scales[i] );
        combo_add( combo, text, scales[i], games.overlay_scale );
    }
    view_row_end( view );
    view_space( view, px( 10 ) );
    view_text( view, STYLE_GRAY, load_string( IDS_OSD_SHOWS ) );
    view_space( view, px( 4 ) );
    for (UINT i = 0; i < ARRAY_SIZE(items); i += 2)
    {
        view_row_begin( view );
        for (UINT k = i; k < i + 2 && k < ARRAY_SIZE(items); k++)
        {
            HWND check = view_control_beside( view, WC_BUTTONW, load_string( items[k].name ),
                                              BS_AUTOCHECKBOX | WS_TABSTOP, (k - i) * column, column - px( 8 ),
                                              px( 20 ), ID_ITEM_FIRST + k );
            SendMessageW( check, BM_SETCHECK, (games.overlay_items & items[k].bit) ? BST_CHECKED : BST_UNCHECKED, 0 );
        }
        view_row_end( view );
    }
    view_space( view, px( 16 ) );

    view_group( view, load_string( IDS_HOTKEYS ) );
    view_space( view, px( 8 ) );
    label_row_begin( view, IDS_HOTKEY_OVERLAY );
    hotkey = view_control_beside( view, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, LABEL_WIDTH, COMBO_WIDTH,
                                  px( 22 ), ID_HOTKEY_OVERLAY );
    SendMessageW( hotkey, HKM_SETHOTKEY, games.hotkey_overlay, 0 );
    view_row_end( view );
    view_space( view, px( 6 ) );
    label_row_begin( view, IDS_HOTKEY_PANEL );
    hotkey = view_control_beside( view, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, LABEL_WIDTH, COMBO_WIDTH,
                                  px( 22 ), ID_HOTKEY_PANEL );
    SendMessageW( hotkey, HKM_SETHOTKEY, games.hotkey_panel, 0 );
    view_row_end( view );
    view_text_at( view, STYLE_SMALL, load_string( IDS_HOTKEY_TEXT ), 0 );
    view_space( view, px( 16 ) );

    view_group( view, load_string( IDS_DIAGNOSTICS ) );
    view_space( view, px( 8 ) );
    check_box( view, ID_FRAME_LOG, IDS_FRAME_LOG, GetFileAttributesW( ARCTIC_DWM_DIAGNOSTICS ) != INVALID_FILE_ATTRIBUTES,
               0, 0 );
    view_text_at( view, STYLE_SMALL, load_string( IDS_FRAME_LOG_TEXT ), px( 18 ) );
}

static void games_save( struct view *view, UINT id )
{
    struct arctic_games games;
    int old_game_mode, old_fps;

    arctic_games_read( &games );
    old_game_mode = games.game_mode;
    old_fps = games.max_fps;
    games.game_mode = checked( view, ID_GAME_MODE );
    games.max_fps = combo_value( view, ID_MAX_FPS, games.max_fps );
    games.overlay = checked( view, ID_OVERLAY );
    games.overlay_always = checked( view, ID_OVERLAY_ALWAYS );
    games.overlay_corner = combo_value( view, ID_CORNER, games.overlay_corner );
    games.overlay_scale = combo_value( view, ID_SCALE, games.overlay_scale );
    games.overlay_items = 0;
    for (UINT i = 0; i < ARRAY_SIZE(items); i++)
        if (checked( view, ID_ITEM_FIRST + i )) games.overlay_items |= items[i].bit;
    games.hotkey_overlay = (WORD)SendMessageW( GetDlgItem( view_window( view ), ID_HOTKEY_OVERLAY ), HKM_GETHOTKEY, 0, 0 );
    games.hotkey_panel = (WORD)SendMessageW( GetDlgItem( view_window( view ), ID_HOTKEY_PANEL ), HKM_GETHOTKEY, 0, 0 );
    arctic_games_write( &games );

    if (games.max_fps != old_fps) write_frame_limit( games.max_fps );
    if (id == ID_FRAME_LOG) set_frame_log( checked( view, ID_FRAME_LOG ) );
    /* the host keeps game mode: it reads the settings when asked */
    if (games.game_mode != old_game_mode) ArcticPowerRequest( "games-settings" );
    tell_overlay( &games );
}

BOOL games_command( struct view *view, UINT id, UINT code, HWND control )
{
    if ((id == ID_HOTKEY_OVERLAY || id == ID_HOTKEY_PANEL) && code == EN_CHANGE)
    {
        games_save( view, id );
        return TRUE;
    }
    if (id == ID_MAX_FPS || id == ID_CORNER || id == ID_SCALE)
    {
        if (code != CBN_SELCHANGE) return FALSE;
        games_save( view, id );
        return TRUE;
    }
    if (code != BN_CLICKED || id < ID_GAME_MODE || id > ID_FRAME_LOG) return FALSE;
    games_save( view, id );
    return TRUE;
}

/**********************************************************************
 *          Продуктивність
 */

struct tuning_info
{
    WCHAR cpu_name[96];
    int intel, undervolt, core_offset, cache_offset;
    int power_limits, pl1, pl2, pl1_default, pl2_default, pl_max;
    int min_mhz, max_mhz;
    int turbo, turbo_base, turbo_default, turbo_max, oc_bins;
    int gpu_vendor;
    WCHAR gpu_pci[32], gpu_name[96];
    int gpu_power_limit, power_min, power_max, power_default, power;
    int clock_offsets, core_min, core_max, mem_min, mem_max;
    int max_clock, max_clock_mhz;
    WCHAR status[128];
};

struct slider
{
    UINT id;
    int kind;                /* enum slider_kind */
    int low, high, step;
    int standard;            /* the machine's own, for a power limit */
};

struct tuning_data
{
    struct tuning_info info;
    struct arctic_sensors sensors;
    BOOL have_sensors;
    BOOL busy;
    UINT gpu_top_seen;       /* MHz: the dial's top, for a card that does not say its own */
    struct slider sliders[12];
    UINT slider_count;
};

static int info_int( const WCHAR *section, const WCHAR *key )
{
    WCHAR value[32];

    GetPrivateProfileStringW( section, key, L"0", value, ARRAY_SIZE(value), ARCTIC_TUNING_INFO );
    return wcstol( value, NULL, 10 );
}

static void read_info( struct tuning_info *info )
{
    memset( info, 0, sizeof(*info) );
    GetPrivateProfileStringW( L"Cpu", L"Name", L"", info->cpu_name, ARRAY_SIZE(info->cpu_name), ARCTIC_TUNING_INFO );
    info->intel = info_int( L"Cpu", L"Intel" );
    info->undervolt = info_int( L"Cpu", L"Undervolt" );
    info->core_offset = info_int( L"Cpu", L"CoreOffset" );
    info->cache_offset = info_int( L"Cpu", L"CacheOffset" );
    info->power_limits = info_int( L"Cpu", L"PowerLimits" );
    info->pl1 = info_int( L"Cpu", L"Pl1" );
    info->pl2 = info_int( L"Cpu", L"Pl2" );
    info->pl1_default = info_int( L"Cpu", L"Pl1Default" );
    info->pl2_default = info_int( L"Cpu", L"Pl2Default" );
    info->pl_max = info_int( L"Cpu", L"PlMax" );
    info->min_mhz = info_int( L"Cpu", L"MinMhz" );
    info->max_mhz = info_int( L"Cpu", L"MaxMhz" );
    info->turbo = info_int( L"Cpu", L"Turbo" );
    info->turbo_base = info_int( L"Cpu", L"TurboBase" );
    info->turbo_default = info_int( L"Cpu", L"TurboDefault" );
    info->turbo_max = info_int( L"Cpu", L"TurboMax" );
    info->oc_bins = info_int( L"Cpu", L"OcBins" );
    info->gpu_vendor = info_int( L"Gpu", L"Vendor" );
    GetPrivateProfileStringW( L"Gpu", L"Pci", L"", info->gpu_pci, ARRAY_SIZE(info->gpu_pci), ARCTIC_TUNING_INFO );
    GetPrivateProfileStringW( L"Gpu", L"Name", L"", info->gpu_name, ARRAY_SIZE(info->gpu_name), ARCTIC_TUNING_INFO );
    info->gpu_power_limit = info_int( L"Gpu", L"PowerLimit" );
    info->power_min = info_int( L"Gpu", L"PowerMin" );
    info->power_max = info_int( L"Gpu", L"PowerMax" );
    info->power_default = info_int( L"Gpu", L"PowerDefault" );
    info->power = info_int( L"Gpu", L"Power" );
    info->clock_offsets = info_int( L"Gpu", L"ClockOffsets" );
    info->core_min = info_int( L"Gpu", L"CoreMin" );
    info->core_max = info_int( L"Gpu", L"CoreMax" );
    info->mem_min = info_int( L"Gpu", L"MemMin" );
    info->mem_max = info_int( L"Gpu", L"MemMax" );
    info->max_clock = info_int( L"Gpu", L"MaxClock" );
    info->max_clock_mhz = info_int( L"Gpu", L"MaxClockMhz" );
    GetPrivateProfileStringW( L"State", L"Status", L"", info->status, ARRAY_SIZE(info->status), ARCTIC_TUNING_INFO );

    /* the card's name as the other pages show it */
    if (info->gpu_pci[0])
    {
        struct arctic_gpu_list *list = malloc( sizeof(*list) );

        if (list && ArcticGpuList( list, FALSE ))
            for (UINT i = 0; i < list->count; i++)
                if (!wcsicmp( list->gpus[i].pci, info->gpu_pci )) lstrcpynW( info->gpu_name, list->gpus[i].name,
                                                                             ARRAY_SIZE(info->gpu_name) );
        free( list );
    }
}

static struct tuning_data *tuning_of( struct view *view )
{
    struct page_state *state = view_state( view );

    if (!state->data) state->data = calloc( 1, sizeof(struct tuning_data) );
    return state->data;
}

static const WCHAR *failed_names( const WCHAR *status, WCHAR *out, size_t count )
{
    static const struct { const WCHAR *token; UINT name; } names[] =
    {
        { L"core", IDS_CORE_OFFSET }, { L"cache", IDS_CACHE_OFFSET }, { L"power", IDS_PL1 },
        { L"max", IDS_CPU_MAX }, { L"gpu-power", IDS_GPU_POWER_LIMIT }, { L"gpu-core", IDS_GPU_CORE_OFFSET },
        { L"gpu-mem", IDS_GPU_MEM_OFFSET }, { L"gpu-max", IDS_GPU_MAX }, { L"turbo", IDS_TURBO },
    };
    WCHAR copy[128], *token, *context = NULL;

    out[0] = 0;
    lstrcpynW( copy, status, ARRAY_SIZE(copy) );
    for (token = wcstok( copy + 6, L" ", &context ); token; token = wcstok( NULL, L" ", &context ))
        for (UINT i = 0; i < ARRAY_SIZE(names); i++)
            if (!wcscmp( token, names[i].token ))
            {
                if (out[0]) wcsncat( out, L", ", count - wcslen( out ) - 1 );
                wcsncat( out, load_string( names[i].name ), count - wcslen( out ) - 1 );
            }
    return out;
}

/**********************************************************************
 *          The dials: as MSI Afterburner's, the clock round the edge and
 *          the temperature inside it. Drawn here, each pixel covered as
 *          much as the arc covers it: Wine's GDI+ does not smooth lines.
 */

struct canvas
{
    UINT32 *bits;            /* 0x00RRGGBB, on the page's white */
    int width, height;
};

/* an arc of a ring with round ends: from start, clockwise, sweep degrees */
static void draw_arc( struct canvas *c, float cx, float cy, float r, float half, float start, float sweep,
                      COLORREF color )
{
    float a0 = start * 3.14159265f / 180, a1 = (start + sweep) * 3.14159265f / 180;
    float ends[2][2] = { { cx + r * cosf( a0 ), cy + r * sinf( a0 ) }, { cx + r * cosf( a1 ), cy + r * sinf( a1 ) } };
    int x0 = max( 0, (int)(cx - r - half - 2) ), x1 = min( c->width, (int)(cx + r + half + 3) );
    int y0 = max( 0, (int)(cy - r - half - 2) ), y1 = min( c->height, (int)(cy + r + half + 3) );

    if (sweep <= 0) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float px_ = x + 0.5f - cx, py_ = y + 0.5f - cy, dist = sqrtf( px_ * px_ + py_ * py_ ), cover = 0;
            float angle = atan2f( py_, px_ ) * 180 / 3.14159265f;
            float rel = fmodf( angle - start + 720, 360 );
            UINT32 *p = &c->bits[y * c->width + x];

            if (rel <= sweep) cover = half - fabsf( dist - r ) + 0.5f;
            for (int e = 0; e < 2; e++)
            {
                float dx = x + 0.5f - ends[e][0], dy = y + 0.5f - ends[e][1];
                cover = max( cover, half - sqrtf( dx * dx + dy * dy ) + 0.5f );
            }
            if (cover <= 0) continue;
            if (cover > 1) cover = 1;
            *p = ((UINT32)(((*p >> 16) & 0xff) * (1 - cover) + GetRValue( color ) * cover) << 16) |
                 ((UINT32)(((*p >> 8) & 0xff) * (1 - cover) + GetGValue( color ) * cover) << 8) |
                 (UINT32)((*p & 0xff) * (1 - cover) + GetBValue( color ) * cover);
        }
}

static COLORREF temperature_color( UINT celsius )
{
    if (celsius >= 90) return RGB( 0xe8, 0x11, 0x23 );
    if (celsius >= 80) return RGB( 0xf7, 0x63, 0x0c );
    return RGB( 0x10, 0x89, 0x3e );
}

static HFONT dial_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

static void centered( HDC hdc, HFONT font, COLORREF color, const WCHAR *text, int cx, int top, int bottom )
{
    RECT rect = { cx - px( 120 ), top, cx + px( 120 ), bottom };
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, &rect, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER );
    SelectObject( hdc, old );
}

static void volts( WCHAR *text, size_t count, UINT mv )
{
    WCHAR sep[4] = L",";

    GetLocaleInfoW( LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, sep, ARRAY_SIZE(sep) );
    swprintf( text, count, L"%u%s%03u В", mv / 1000, sep, mv % 1000 );
}

/* one dial: the clock against its top, the temperature against 100 °C */
static void draw_dial( HDC hdc, struct canvas *canvas, int cx, int cy, UINT mhz, UINT top_mhz, UINT temperature_mc,
                       UINT voltage_mv, const WCHAR *name, const WCHAR *details )
{
    static HFONT font_big, font_small;  /* not "small": rpcndr.h makes that a type */
    float r = px( 80 ), r_inner = px( 64 ), outer = px( 10 ) / 2.0f, inner = px( 5 ) / 2.0f;
    UINT celsius = temperature_mc == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (temperature_mc + 500) / 1000;
    WCHAR text[64];

    if (!font_big)
    {
        font_big = dial_font( px( 32 ), FW_LIGHT );
        font_small = dial_font( px( 12 ), FW_NORMAL );
    }
    draw_arc( canvas, cx, cy, r, outer, 135, 270, view_dark() ? RGB( 0x3c, 0x3c, 0x3c ) : RGB( 0xe8, 0xe8, 0xe8 ) );
    if (mhz != ARCTIC_UNKNOWN && top_mhz)
        draw_arc( canvas, cx, cy, r, outer, 135, 270.0f * min( 1.0f, (float)mhz / top_mhz ), RGB( 0x00, 0x78, 0xd7 ) );
    draw_arc( canvas, cx, cy, r_inner, inner, 135, 270, view_dark() ? RGB( 0x2b, 0x2b, 0x2b ) : RGB( 0xf2, 0xf2, 0xf2 ) );
    if (celsius != ARCTIC_UNKNOWN)
        draw_arc( canvas, cx, cy, r_inner, inner, 135, 270.0f * min( 1.0f, celsius / 100.0f ), temperature_color( celsius ) );

    if (mhz != ARCTIC_UNKNOWN) swprintf( text, ARRAY_SIZE(text), L"%u", mhz );
    else lstrcpyW( text, L"—" );
    centered( hdc, font_big, view_dark() ? RGB( 0xff, 0xff, 0xff ) : RGB( 0, 0, 0 ), text, cx, cy - px( 34 ), cy + px( 6 ) );
    centered( hdc, font_small, view_dark() ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x6d, 0x6d, 0x6d ), L"МГц", cx, cy + px( 4 ), cy + px( 20 ) );
    if (celsius != ARCTIC_UNKNOWN)
    {
        swprintf( text, ARRAY_SIZE(text), L"%u °C", celsius );
        centered( hdc, font_small, temperature_color( celsius ), text, cx, cy + px( 22 ), cy + px( 38 ) );
    }
    if (voltage_mv != ARCTIC_UNKNOWN)
    {
        volts( text, ARRAY_SIZE(text), voltage_mv );
        centered( hdc, font_small, view_dark() ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x6d, 0x6d, 0x6d ), text, cx, cy + px( 38 ), cy + px( 54 ) );
    }
    centered( hdc, view_font( STYLE_BOLD ), view_dark() ? RGB( 0xff, 0xff, 0xff ) : RGB( 0, 0, 0 ), name, cx, cy + px( 74 ), cy + px( 92 ) );
    centered( hdc, font_small, view_dark() ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x6d, 0x6d, 0x6d ), details, cx, cy + px( 92 ), cy + px( 108 ) );
}

static void join( WCHAR *text, size_t count, const WCHAR *part )
{
    if (text[0]) wcsncat( text, L" · ", count - wcslen( text ) - 1 );
    wcsncat( text, part, count - wcslen( text ) - 1 );
}

static void paint_dials( struct view *view, HDC hdc, const RECT *rect )
{
    struct tuning_data *data = tuning_of( view );
    const struct arctic_sensors *s;
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER) } };
    struct canvas canvas;
    HBITMAP bitmap;
    HDC mem;
    WCHAR details[128], part[32];
    UINT gpu_top;
    int cpu_x = px( 100 ), gpu_x = px( 340 ), cy = px( 96 );

    if (!data || !data->have_sensors) return;
    s = &data->sensors;

    /* the arcs on a white bitmap of the dials' area, then the words over them */
    canvas.width = min( rect->right - rect->left, px( 460 ) );
    canvas.height = rect->bottom - rect->top;
    info.bmiHeader.biWidth = canvas.width;
    info.bmiHeader.biHeight = -canvas.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if (canvas.width <= 0 || canvas.height <= 0) return;
    if (!(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&canvas.bits, NULL, 0 ))) return;
    {
        /* the page's colour, white or the dark mode's */
        COLORREF page = view_page_color();
        UINT32 pixel = (GetRValue( page ) << 16) | (GetGValue( page ) << 8) | GetBValue( page );

        for (int i = 0; i < canvas.width * canvas.height; i++) canvas.bits[i] = pixel;
    }
    mem = CreateCompatibleDC( hdc );
    SelectObject( mem, bitmap );
    SetBkMode( mem, TRANSPARENT );

    details[0] = 0;
    if (s->cpu_load_percent != ARCTIC_UNKNOWN)
    {
        swprintf( part, ARRAY_SIZE(part), L"%u %%", s->cpu_load_percent );
        join( details, ARRAY_SIZE(details), part );
    }
    if (s->cpu_power_mw != ARCTIC_UNKNOWN)
    {
        swprintf( part, ARRAY_SIZE(part), L"%u Вт", (s->cpu_power_mw + 500) / 1000 );
        join( details, ARRAY_SIZE(details), part );
    }
    draw_dial( mem, &canvas, cpu_x, cy, s->cpu_mhz, max( data->info.max_mhz, s->cpu_mhz == ARCTIC_UNKNOWN ? 0 : s->cpu_mhz ),
               s->cpu_temperature_mc, s->cpu_voltage_mv, load_string( IDS_TUNING_PROCESSOR ), details );

    /* the card's top: its own, else the most seen */
    if (s->gpu_mhz != ARCTIC_UNKNOWN) data->gpu_top_seen = max( data->gpu_top_seen, s->gpu_mhz );
    gpu_top = data->info.max_clock_mhz ? data->info.max_clock_mhz : max( data->gpu_top_seen, 1000 );
    details[0] = 0;
    if (s->gpu_load_percent != ARCTIC_UNKNOWN)
    {
        swprintf( part, ARRAY_SIZE(part), L"%u %%", s->gpu_load_percent );
        join( details, ARRAY_SIZE(details), part );
    }
    if (s->gpu_power_mw != ARCTIC_UNKNOWN)
    {
        swprintf( part, ARRAY_SIZE(part), L"%u Вт", (s->gpu_power_mw + 500) / 1000 );
        join( details, ARRAY_SIZE(details), part );
    }
    if (s->gpu_memory_used_mb != ARCTIC_UNKNOWN && s->gpu_memory_total_mb != ARCTIC_UNKNOWN)
    {
        swprintf( part, ARRAY_SIZE(part), L"%u / %u МБ", s->gpu_memory_used_mb, s->gpu_memory_total_mb );
        join( details, ARRAY_SIZE(details), part );
    }
    draw_dial( mem, &canvas, gpu_x, cy, s->gpu_mhz, gpu_top, s->gpu_temperature_mc, s->gpu_voltage_mv,
               load_string( IDS_TUNING_GPU ), details );

    BitBlt( hdc, rect->left, rect->top, canvas.width, canvas.height, mem, 0, 0, SRCCOPY );
    DeleteDC( mem );
    DeleteObject( bitmap );
}

/**********************************************************************
 *          The sliders
 */

enum slider_kind
{
    KIND_MV,          /* a voltage offset */
    KIND_WATTS,       /* a power limit: at its default, the default (0) */
    KIND_LIMIT_MHZ,   /* a top clock: at the top, no limit (0) */
    KIND_OFFSET_MHZ,  /* a clock offset */
    KIND_RATIO,       /* the turbo multiplier: at the firmware's, the firmware's (0) */
};

static void slider_text( const struct slider *slider, int value, WCHAR *text, size_t count )
{
    switch (slider->kind)
    {
    case KIND_MV:
        if (value < 0) swprintf( text, count, L"−%d мВ", -value );
        else if (value > 0) swprintf( text, count, L"+%d мВ", value );
        else swprintf( text, count, L"0 мВ" );
        break;
    case KIND_WATTS:
        if (value == slider->standard) swprintf( text, count, L"%s", format_string( IDS_STANDARD_W, value ) );
        else swprintf( text, count, L"%d Вт", value );
        break;
    case KIND_LIMIT_MHZ:
        if (value >= slider->high) lstrcpynW( text, load_string( IDS_NO_LIMIT ), count );
        else swprintf( text, count, L"%d МГц", value );
        break;
    case KIND_RATIO:
        swprintf( text, count, L"×%d · %d МГц", value, value * 100 );
        break;
    case KIND_OFFSET_MHZ:
        if (value > 0) swprintf( text, count, L"+%d МГц", value );
        else if (value < 0) swprintf( text, count, L"−%d МГц", -value );
        else swprintf( text, count, L"0 МГц" );
        break;
    }
}

static int slider_value( const struct slider *slider, HWND trackbar )
{
    return slider->low + SendMessageW( trackbar, TBM_GETPOS, 0, 0 ) * slider->step;
}

/* what the settings keep: the default and no limit are 0 */
static int slider_setting( const struct slider *slider, int value )
{
    if ((slider->kind == KIND_WATTS || slider->kind == KIND_RATIO) && value == slider->standard) return 0;
    if (slider->kind == KIND_LIMIT_MHZ && value >= slider->high) return 0;
    return value;
}

static void add_slider( struct view *view, struct tuning_data *data, UINT id, UINT label, enum slider_kind kind,
                        int low, int high, int step, int standard, int setting )
{
    struct slider *slider;
    WCHAR text[64];
    HWND trackbar;
    int current;

    if (data->slider_count >= ARRAY_SIZE(data->sliders) || high <= low) return;
    slider = &data->sliders[data->slider_count++];
    slider->id = id;
    slider->kind = kind;
    slider->low = low;
    slider->high = low + (high - low) / step * step;
    slider->step = step;
    slider->standard = standard;
    if (kind == KIND_WATTS || kind == KIND_RATIO) current = setting ? setting : standard;
    else if (kind == KIND_LIMIT_MHZ) current = setting ? setting : slider->high;
    else current = setting;
    current = max( slider->low, min( slider->high, current ) );

    label_row_begin( view, label );
    trackbar = view_control_beside( view, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, LABEL_WIDTH - px( 6 ),
                                    px( 230 ), px( 26 ), id );
    SendMessageW( trackbar, TBM_SETRANGE, FALSE, MAKELPARAM( 0, (slider->high - slider->low) / step ) );
    SendMessageW( trackbar, TBM_SETPAGESIZE, 0, max( 1, (slider->high - slider->low) / step / 10 ) );
    SendMessageW( trackbar, TBM_SETPOS, TRUE, (current - slider->low + step / 2) / step );
    slider_text( slider, slider_value( slider, trackbar ), text, ARRAY_SIZE(text) );
    view_control_beside( view, WC_STATICW, text, SS_LEFT | SS_CENTERIMAGE, LABEL_WIDTH + px( 232 ), px( 170 ),
                                 px( 26 ), id + 100 );
    view_row_end( view );
    view_space( view, px( 4 ) );
}

static struct slider *slider_of( struct tuning_data *data, UINT id )
{
    for (UINT i = 0; i < data->slider_count; i++) if (data->sliders[i].id == id) return &data->sliders[i];
    return NULL;
}

static int slider_setting_of( struct view *view, struct tuning_data *data, UINT id )
{
    struct slider *slider = slider_of( data, id );
    HWND trackbar = GetDlgItem( view_window( view ), id );

    if (!slider || !trackbar) return 0;
    return slider_setting( slider, slider_value( slider, trackbar ) );
}

void tuning_build( struct view *view )
{
    struct tuning_data *data = tuning_of( view );
    struct tuning_info *info;
    struct arctic_games games;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    int width = view_content_width( view );
    WCHAR text[128];

    if (!data) return;
    InitCommonControlsEx( &icc );
    info = &data->info;
    read_info( info );
    arctic_games_read( &games );
    data->slider_count = 0;
    data->have_sensors = ArcticSensors( &data->sensors );
    ArcticSensors( NULL );

    nav_games_tasks( view );
    view_text( view, STYLE_TITLE, load_string( IDS_TUNING_TITLE ) );
    view_space( view, px( 8 ) );
    view_paint_area( view, px( 206 ), paint_dials );
    view_space( view, px( 10 ) );

    view_group( view, load_string( IDS_TUNING_PROCESSOR ) );
    view_space( view, px( 4 ) );
    if (info->cpu_name[0]) view_text( view, STYLE_GRAY, info->cpu_name );
    view_space( view, px( 8 ) );
    if (info->undervolt)
    {
        add_slider( view, data, ID_CORE_OFFSET, IDS_CORE_OFFSET, KIND_MV, -200, 100, 5, 0, games.cpu_core_offset );
        add_slider( view, data, ID_CACHE_OFFSET, IDS_CACHE_OFFSET, KIND_MV, -200, 100, 5, 0, games.cpu_cache_offset );
    }
    if (info->power_limits)
    {
        int top = min( max( max( info->pl_max, info->pl1_default * 2 ), 15 ), 250 );

        add_slider( view, data, ID_PL1, IDS_PL1, KIND_WATTS, 5, top, 1, info->pl1_default, games.cpu_pl1 );
        add_slider( view, data, ID_PL2, IDS_PL2, KIND_WATTS, 5, min( max( top, info->pl2_default * 2 ), 300 ), 1,
                    info->pl2_default, games.cpu_pl2 );
    }
    if (info->turbo)
    {
        add_slider( view, data, ID_TURBO, IDS_TURBO, KIND_RATIO, info->turbo_base, info->turbo_max, 1,
                    info->turbo_default, games.cpu_turbo_ratio );
        if (!info->oc_bins)
            view_text_at( view, STYLE_SMALL, format_string( IDS_TURBO_LOCKED, info->turbo_default ), 0 );
        view_space( view, px( 4 ) );
    }
    if (info->max_mhz > info->min_mhz && info->min_mhz > 0)
        add_slider( view, data, ID_CPU_MAX, IDS_CPU_MAX, KIND_LIMIT_MHZ, (info->min_mhz + 99) / 100 * 100,
                    info->max_mhz, 100, 0, games.cpu_max_mhz );
    if (!info->undervolt && !info->power_limits)
        view_text( view, STYLE_SMALL, load_string( IDS_CPU_LOCKED ) );
    view_space( view, px( 12 ) );

    view_group( view, load_string( IDS_TUNING_GPU ) );
    view_space( view, px( 4 ) );
    if (info->gpu_name[0]) view_text( view, STYLE_GRAY, info->gpu_name );
    view_space( view, px( 8 ) );
    if (info->gpu_power_limit)
        add_slider( view, data, ID_GPU_POWER, IDS_GPU_POWER_LIMIT, KIND_WATTS, info->power_min, info->power_max, 1,
                    info->power_default, games.gpu_power );
    if (info->clock_offsets)
    {
        add_slider( view, data, ID_GPU_CORE, IDS_GPU_CORE_OFFSET, KIND_OFFSET_MHZ, max( info->core_min, -500 ) / 10 * 10,
                    min( info->core_max, 500 ), 10, 0, games.gpu_core_offset );
        add_slider( view, data, ID_GPU_MEM, IDS_GPU_MEM_OFFSET, KIND_OFFSET_MHZ, max( info->mem_min, -1000 ) / 10 * 10,
                    min( info->mem_max, 1000 ), 10, 0, games.gpu_mem_offset );
    }
    if (info->max_clock && info->max_clock_mhz > 300)
        add_slider( view, data, ID_GPU_MAX, IDS_GPU_MAX, KIND_LIMIT_MHZ, 300, info->max_clock_mhz, 10, 0,
                    games.gpu_max_mhz );
    if (!info->gpu_power_limit && !info->clock_offsets && !info->max_clock)
        view_text( view, STYLE_SMALL, load_string( IDS_GPU_LOCKED ) );
    view_space( view, px( 12 ) );

    check_box( view, ID_APPLY_AT_START, IDS_APPLY_AT_START, games.apply_at_start, 0, 0 );
    view_text_at( view, STYLE_SMALL, load_string( IDS_APPLY_AT_START_TEXT ), px( 18 ) );
    view_space( view, px( 12 ) );

    if (!wcscmp( info->status, L"applied" )) view_text( view, STYLE_BODY, load_string( IDS_STATUS_APPLIED ) );
    else if (!wcscmp( info->status, L"held" )) view_text( view, STYLE_BODY, load_string( IDS_STATUS_HELD ) );
    else if (!wcsncmp( info->status, L"failed", 6 ))
        view_text( view, STYLE_BODY, format_string( IDS_STATUS_FAILED, failed_names( info->status, text, ARRAY_SIZE(text) ) ) );
    view_space( view, px( 12 ) );

    view_row_begin( view );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_APPLY ), BS_DEFPUSHBUTTON | WS_TABSTOP,
                         width - px( 2 * 98 + 8 ), px( 98 ), px( 24 ), ID_APPLY );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_RESET ), BS_PUSHBUTTON | WS_TABSTOP, width - px( 98 ),
                         px( 98 ), px( 24 ), ID_RESET );
    view_row_end( view );

    view_set_timer( view, 1000 );
}

void tuning_timer( struct view *view )
{
    struct tuning_data *data = tuning_of( view );
    RECT rect;

    if (!data || data->busy) return;
    /* NVIDIA's library is let go after each look: open, it keeps the card awake */
    data->have_sensors = ArcticSensors( &data->sensors );
    ArcticSensors( NULL );
    GetClientRect( view_window( view ), &rect );
    rect.bottom = px( 260 );   /* the dials, near the top */
    InvalidateRect( view_window( view ), &rect, FALSE );
}

static void tuning_apply( struct view *view, BOOL reset )
{
    struct tuning_data *data = tuning_of( view );
    struct arctic_games games;
    HCURSOR cursor;

    if (!data) return;
    arctic_games_read( &games );
    if (reset)
    {
        games.cpu_core_offset = games.cpu_cache_offset = games.cpu_pl1 = games.cpu_pl2 = games.cpu_max_mhz = 0;
        games.cpu_turbo_ratio = 0;
        games.gpu_power = games.gpu_core_offset = games.gpu_mem_offset = games.gpu_max_mhz = 0;
    }
    else
    {
        games.cpu_core_offset = slider_setting_of( view, data, ID_CORE_OFFSET );
        games.cpu_cache_offset = slider_setting_of( view, data, ID_CACHE_OFFSET );
        games.cpu_pl1 = slider_setting_of( view, data, ID_PL1 );
        games.cpu_pl2 = slider_setting_of( view, data, ID_PL2 );
        games.cpu_max_mhz = slider_setting_of( view, data, ID_CPU_MAX );
        games.cpu_turbo_ratio = slider_setting_of( view, data, ID_TURBO );
        games.gpu_power = slider_setting_of( view, data, ID_GPU_POWER );
        games.gpu_core_offset = slider_setting_of( view, data, ID_GPU_CORE );
        games.gpu_mem_offset = slider_setting_of( view, data, ID_GPU_MEM );
        games.gpu_max_mhz = slider_setting_of( view, data, ID_GPU_MAX );
    }
    games.apply_at_start = checked( view, ID_APPLY_AT_START );
    if (!arctic_games_write( &games )) return;
    data->busy = TRUE;
    cursor = SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_WAIT ) );
    ArcticPowerRequest( "games" );
    SetCursor( cursor );
    data->busy = FALSE;
    view_rebuild( view );
}

BOOL tuning_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct tuning_data *data = tuning_of( view );
    struct slider *slider;

    /* a slider moved (WM_HSCROLL): its value beside it */
    if (data && (slider = slider_of( data, id )))
    {
        WCHAR text[64];

        slider_text( slider, slider_value( slider, control ), text, ARRAY_SIZE(text) );
        SetDlgItemTextW( view_window( view ), id + 100, text );
        return TRUE;
    }
    if (code != BN_CLICKED) return FALSE;
    switch (id)
    {
    case ID_APPLY:
        tuning_apply( view, FALSE );
        return TRUE;
    case ID_RESET:
        tuning_apply( view, TRUE );
        return TRUE;
    case ID_APPLY_AT_START:
    {
        /* only the setting: what applies at the next start */
        struct arctic_games games;

        arctic_games_read( &games );
        games.apply_at_start = checked( view, ID_APPLY_AT_START );
        arctic_games_write( &games );
        return TRUE;
    }
    }
    return FALSE;
}
