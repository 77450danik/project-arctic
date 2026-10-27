/*
 * Arctic: the Games panel's settings (docs/games.md)
 *
 * C:\ProgramData\Arctic\games.ini, one [Games] section of numbers, ASCII:
 * the Games panel (powercpl.dll) writes it, the overlay (gameosd.exe) and
 * the host (arctic-init: game mode, the tuning) read it. The host writes
 * what this machine can tune in tuning-info.ini beside it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_GAMES_H
#define __WINE_ARCTIC_GAMES_H

#define ARCTIC_GAMES_DIR         L"C:\\ProgramData\\Arctic"
#define ARCTIC_GAMES_INI         ARCTIC_GAMES_DIR L"\\games.ini"
#define ARCTIC_TUNING_INFO       ARCTIC_GAMES_DIR L"\\tuning-info.ini"
#define ARCTIC_DXVK_CONF         ARCTIC_GAMES_DIR L"\\dxvk.conf"        /* DXVK_CONFIG_FILE of every NT process */
#define ARCTIC_DWM_DIAGNOSTICS   ARCTIC_GAMES_DIR L"\\dwm-diagnostics"  /* there: dwm logs every game's frames */
/* dwm's: the game's process id while one fills a monitor */
#define ARCTIC_GAME_FILE         L"\\\\?\\unix\\run\\arctic\\power\\game"

/* the overlay's window; the panel posts it WM_ARCTIC_GAMES_RELOAD after a change */
#define ARCTIC_OVERLAY_CLASS     L"ArcticGameOverlay"
#define WM_ARCTIC_GAMES_RELOAD   (WM_APP + 1)

/* what the overlay shows */
#define OSD_FPS        0x0001
#define OSD_FRAME_TIME 0x0002
#define OSD_CPU_LOAD   0x0004
#define OSD_CPU_MHZ    0x0008
#define OSD_CPU_TEMP   0x0010
#define OSD_CPU_POWER  0x0020
#define OSD_RAM        0x0040
#define OSD_GPU_LOAD   0x0080
#define OSD_GPU_MHZ    0x0100
#define OSD_GPU_TEMP   0x0200
#define OSD_GPU_MEMORY 0x0400
#define OSD_GPU_POWER  0x0800
#define OSD_FRAME_GRAPH 0x1000  /* the time of each frame, as a graph */
#define OSD_CPU_VOLTAGE 0x2000
#define OSD_DEFAULT    (OSD_FPS | OSD_FRAME_TIME | OSD_CPU_LOAD | OSD_CPU_TEMP | OSD_GPU_LOAD | OSD_GPU_TEMP | OSD_GPU_MEMORY | OSD_FRAME_GRAPH)

enum osd_corner { OSD_TOP_LEFT, OSD_TOP_RIGHT, OSD_BOTTOM_LEFT, OSD_BOTTOM_RIGHT };

struct arctic_games
{
    int game_mode;        /* the game gets the priority, memory combining pauses */
    int max_fps;          /* 0: no limit */
    int overlay;          /* shown over games */
    int overlay_always;   /* ... and over the desktop too */
    int overlay_corner;   /* enum osd_corner */
    int overlay_scale;    /* percent */
    int overlay_items;    /* OSD_* */
    int hotkey_overlay;   /* HKM_GETHOTKEY's: the key, its modifiers (HOTKEYF_*) above */
    int hotkey_panel;
    /* the tuning, done by the host; 0 is the machine's own */
    int apply_at_start;
    int cpu_core_offset;  /* mV, 0 or less */
    int cpu_cache_offset;
    int cpu_pl1;          /* W */
    int cpu_pl2;
    int cpu_max_mhz;
    int cpu_turbo_ratio;  /* the turbo multiplier, 0 the firmware's */
    int gpu_power;        /* W */
    int gpu_core_offset;  /* MHz */
    int gpu_mem_offset;
    int gpu_max_mhz;
};

static const struct { const WCHAR *name; size_t offset; int fallback; } arctic_games_keys[] =
{
#define ARCTIC_GAMES_KEY(name, field, fallback) { L##name, offsetof(struct arctic_games, field), fallback }
    ARCTIC_GAMES_KEY( "GameMode", game_mode, 1 ),
    ARCTIC_GAMES_KEY( "MaxFps", max_fps, 0 ),
    ARCTIC_GAMES_KEY( "Overlay", overlay, 0 ),
    ARCTIC_GAMES_KEY( "OverlayAlways", overlay_always, 0 ),
    ARCTIC_GAMES_KEY( "OverlayCorner", overlay_corner, OSD_TOP_LEFT ),
    ARCTIC_GAMES_KEY( "OverlayScale", overlay_scale, 100 ),
    ARCTIC_GAMES_KEY( "OverlayItems", overlay_items, OSD_DEFAULT ),
    ARCTIC_GAMES_KEY( "HotkeyOverlay", hotkey_overlay, 0 ),
    ARCTIC_GAMES_KEY( "HotkeyPanel", hotkey_panel, 0 ),
    ARCTIC_GAMES_KEY( "ApplyAtStart", apply_at_start, 0 ),
    ARCTIC_GAMES_KEY( "CpuCoreOffset", cpu_core_offset, 0 ),
    ARCTIC_GAMES_KEY( "CpuCacheOffset", cpu_cache_offset, 0 ),
    ARCTIC_GAMES_KEY( "CpuPl1", cpu_pl1, 0 ),
    ARCTIC_GAMES_KEY( "CpuPl2", cpu_pl2, 0 ),
    ARCTIC_GAMES_KEY( "CpuMaxMhz", cpu_max_mhz, 0 ),
    ARCTIC_GAMES_KEY( "CpuTurboRatio", cpu_turbo_ratio, 0 ),
    ARCTIC_GAMES_KEY( "GpuPower", gpu_power, 0 ),
    ARCTIC_GAMES_KEY( "GpuCoreOffset", gpu_core_offset, 0 ),
    ARCTIC_GAMES_KEY( "GpuMemOffset", gpu_mem_offset, 0 ),
    ARCTIC_GAMES_KEY( "GpuMaxMhz", gpu_max_mhz, 0 ),
#undef ARCTIC_GAMES_KEY
};

static inline void arctic_games_read( struct arctic_games *games )
{
    for (size_t i = 0; i < ARRAY_SIZE(arctic_games_keys); i++)
    {
        WCHAR value[32];
        int *field = (int *)((char *)games + arctic_games_keys[i].offset);

        if (GetPrivateProfileStringW( L"Games", arctic_games_keys[i].name, L"", value, ARRAY_SIZE(value),
                                      ARCTIC_GAMES_INI ) && value[0])
            *field = wcstol( value, NULL, 10 );
        else
            *field = arctic_games_keys[i].fallback;
    }
}

/* written whole, in ASCII, for the host to read as it is */
static inline BOOL arctic_games_write( const struct arctic_games *games )
{
    char text[2048];
    size_t len = 0;
    DWORD written;
    HANDLE file;
    BOOL ok;

    len += snprintf( text + len, sizeof(text) - len, "[Games]\r\n" );
    for (size_t i = 0; i < ARRAY_SIZE(arctic_games_keys); i++)
    {
        char name[32];

        WideCharToMultiByte( CP_ACP, 0, arctic_games_keys[i].name, -1, name, sizeof(name), NULL, NULL );
        len += snprintf( text + len, sizeof(text) - len, "%s=%d\r\n", name,
                         *(const int *)((const char *)games + arctic_games_keys[i].offset) );
    }
    CreateDirectoryW( ARCTIC_GAMES_DIR, NULL );
    file = CreateFileW( ARCTIC_GAMES_INI, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile( file, text, len, &written, NULL ) && written == len;
    CloseHandle( file );
    return ok;
}

#endif
