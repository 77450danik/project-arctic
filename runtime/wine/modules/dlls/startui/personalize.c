/*
 * The light and the dark mode, and the look of the title bars: the page
 * "Персоналізація" of the display's properties (desk.cpl, ReactOS patch
 * "desk-personalization-instead-of-the-old-appearance")
 *
 * As Windows 11's colours page has it: Light, Dark, or Custom with a mode
 * for Windows (the taskbar, Start, the flyouts and the title bars) and one
 * for programs (every window's contents and the Control Panel), in
 * HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
 * (SystemUsesLightTheme, AppsUseLightTheme), which programs that follow the
 * mode read themselves: Chromium and Electron, Firefox, Qt, .NET. Each
 * change is at once, as there, and said with WM_SETTINGCHANGE
 * "ImmersiveColorSet".
 *
 * The programs' mode is the colour scheme of the visual style (Mizu,
 * "Normal Color" or "Dark", SetSystemVisualStyle) and the system colours
 * (SetSysColors, HKCU\Control Panel\Colors); those of the light one are
 * kept when the dark one comes (HKCU\Software\Arctic\Personalize,
 * LightColors). The mode of Windows makes the taskbar's Windows 10 and 11
 * looks light or dark (taskbar.c) and the title bars (uxtheme).
 *
 * The title bars show what is behind them as the taskbar does: acrylic, a
 * blur, a see-through tint or a solid colour, or none (HKCU\Software\
 * Arctic\Caption: Effect, Color, Opacity, Blur, Text, Light). Their tint is
 * the mode's, as Windows 11's Mica, until a colour is chosen. uxtheme draws
 * them and asks dwm.exe for the effect. Their settings are a window of
 * their own, opened from the page.
 *
 * The mode of Windows takes the colours of the taskbar, Start and the title
 * bars with it, and never their effect, opacity or blur: a shade (black,
 * grey, white) becomes the new mode's, each mode remembers its own (ColorLight,
 * ColorDark beside Color), and a real colour stays.
 *
 * An update puts the registry's defaults back (arctic.reg): when the
 * taskbar comes, the visual style and the colours are made the mode's again.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "commctrl.h"
#include "shlobj.h"
#include "wine/arctic_dwm.h"

#define PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define THEME_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager"
#define CAPTION_KEY     L"Software\\Arctic\\Caption"
#define TASKBAR_KEY     L"Software\\Arctic\\Taskbar"
#define START_LOOK_KEY  L"Software\\Arctic\\StartMenu\\Look\\"
#define SAVED_KEY      L"Software\\Arctic\\Personalize"
#define COLORS_KEY      L"Control Panel\\Colors"

#define FONT_SUBST_KEY  L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes"

#define DARK_SCHEME     L"Dark"
#define LIGHT_SCHEME    L"Normal Color"

/* the sizes of the visual style that are its two looks of the controls */
#define SIZE_WIN10      L"Normal Size"
#define SIZE_WIN11      L"Windows 11"

#define CAPTION_OPACITY 75
#define CAPTION_BLUR    60

enum mode { MODE_LIGHT, MODE_DARK, MODE_CUSTOM, MODE_COUNT };

#define ID_MODE_LABEL   1499
#define ID_TILE_FIRST   1500   /* + enum mode */
#define ID_SYSTEM_LABEL 1510
#define ID_SYSTEM       1511
#define ID_APPS_LABEL   1512
#define ID_APPS         1513
#define ID_CAPTIONS     1514
#define ID_RESET        1515
#define ID_CAPTION_OPEN 1516
#define ID_STYLE_LABEL  1517
#define ID_STYLE        1518
#define ID_ICONS_LABEL  1519
#define ID_ICONS        1520
#define ID_CAPTION_FIRST 1600  /* look_controls_* */

#define APPLY_TIMER     1

/**********************************************************************
 *          The modes
 */

static DWORD reg_dword( HKEY root, const WCHAR *path, const WCHAR *name, DWORD def, BOOL *found )
{
    DWORD value, size = sizeof(value), type;
    HKEY key;

    if (found) *found = FALSE;
    if (RegOpenKeyExW( root, path, 0, KEY_QUERY_VALUE, &key )) return def;
    if (RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size ) || type != REG_DWORD) value = def;
    else if (found) *found = TRUE;
    RegCloseKey( key );
    return value;
}

static void set_dword( HKEY key, const WCHAR *name, DWORD value )
{
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
}

/* the mode of Windows: the taskbar, Start, the flyouts and the title bars */
BOOL system_mode_light(void)
{
    return reg_dword( HKEY_CURRENT_USER, PERSONALIZE_KEY, L"SystemUsesLightTheme", 1, NULL ) != 0;
}

static BOOL apps_mode_light(void)
{
    return reg_dword( HKEY_CURRENT_USER, PERSONALIZE_KEY, L"AppsUseLightTheme", 1, NULL ) != 0;
}

/* the system colours of the dark mode, Windows 11's; CLR_INVALID: as it is */
static const COLORREF dark_colors[COLOR_MENUBAR + 1] =
{
    RGB(0x2b, 0x2b, 0x2b),  /* COLOR_SCROLLBAR */
    CLR_INVALID,            /* COLOR_BACKGROUND */
    RGB(0x20, 0x20, 0x20),  /* COLOR_ACTIVECAPTION */
    RGB(0x20, 0x20, 0x20),  /* COLOR_INACTIVECAPTION */
    RGB(0x2b, 0x2b, 0x2b),  /* COLOR_MENU */
    RGB(0x19, 0x19, 0x19),  /* COLOR_WINDOW */
    RGB(0x64, 0x64, 0x64),  /* COLOR_WINDOWFRAME */
    RGB(0xff, 0xff, 0xff),  /* COLOR_MENUTEXT */
    RGB(0xff, 0xff, 0xff),  /* COLOR_WINDOWTEXT */
    RGB(0xff, 0xff, 0xff),  /* COLOR_CAPTIONTEXT */
    RGB(0x20, 0x20, 0x20),  /* COLOR_ACTIVEBORDER */
    RGB(0x20, 0x20, 0x20),  /* COLOR_INACTIVEBORDER */
    RGB(0x2b, 0x2b, 0x2b),  /* COLOR_APPWORKSPACE */
    RGB(0x41, 0x41, 0x41),  /* COLOR_HIGHLIGHT */
    RGB(0xff, 0xff, 0xff),  /* COLOR_HIGHLIGHTTEXT */
    RGB(0x20, 0x20, 0x20),  /* COLOR_BTNFACE */
    RGB(0x5a, 0x5a, 0x5a),  /* COLOR_BTNSHADOW */
    RGB(0x80, 0x80, 0x80),  /* COLOR_GRAYTEXT */
    RGB(0xff, 0xff, 0xff),  /* COLOR_BTNTEXT */
    RGB(0x80, 0x80, 0x80),  /* COLOR_INACTIVECAPTIONTEXT */
    RGB(0x46, 0x46, 0x46),  /* COLOR_BTNHIGHLIGHT */
    RGB(0x14, 0x14, 0x14),  /* COLOR_3DDKSHADOW */
    RGB(0x3c, 0x3c, 0x3c),  /* COLOR_3DLIGHT */
    RGB(0xff, 0xff, 0xff),  /* COLOR_INFOTEXT */
    RGB(0x2b, 0x2b, 0x2b),  /* COLOR_INFOBK */
    RGB(0x2b, 0x2b, 0x2b),  /* COLOR_BTNALTERNATEFACE */
    RGB(0x99, 0xd6, 0xff),  /* COLOR_HOTLIGHT */
    RGB(0x20, 0x20, 0x20),  /* COLOR_GRADIENTACTIVECAPTION */
    RGB(0x20, 0x20, 0x20),  /* COLOR_GRADIENTINACTIVECAPTION */
    RGB(0x41, 0x41, 0x41),  /* COLOR_MENUHILIGHT */
    RGB(0x20, 0x20, 0x20),  /* COLOR_MENUBAR */
};

/* ... and of the light one, Mizu's (arctic.reg), when none were kept */
static const COLORREF light_colors[COLOR_MENUBAR + 1] =
{
    RGB(0xc8, 0xc8, 0xc8), CLR_INVALID, RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff),
    RGB(0xff, 0xff, 0xff), RGB(0x64, 0x64, 0x64), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0),
    RGB(0xb4, 0xb4, 0xb4), RGB(0xf4, 0xf7, 0xfc), RGB(0xab, 0xab, 0xab), RGB(0xe6, 0xe6, 0xe6), RGB(0, 0, 0),
    RGB(0xf0, 0xf0, 0xf0), RGB(0xa2, 0xa2, 0xa2), RGB(0xac, 0xa8, 0x99), RGB(0, 0, 0), RGB(0x99, 0x99, 0x99),
    RGB(0xff, 0xff, 0xff), RGB(0xa2, 0xa2, 0xa2), RGB(0xf1, 0xef, 0xe2), RGB(0, 0, 0), RGB(0xff, 0xff, 0xe1),
    RGB(0, 0, 0), RGB(0, 0, 0x80), RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff), RGB(0xdc, 0xdc, 0xdc),
    RGB(0xef, 0xee, 0xf3),
};

/* the names of HKCU\Control Panel\Colors, by COLOR_* */
static const WCHAR *const color_names[COLOR_MENUBAR + 1] =
{
    L"Scrollbar", L"Background", L"ActiveTitle", L"InactiveTitle", L"Menu", L"Window", L"WindowFrame",
    L"MenuText", L"WindowText", L"TitleText", L"ActiveBorder", L"InactiveBorder", L"AppWorkSpace", L"Hilight",
    L"HilightText", L"ButtonFace", L"ButtonShadow", L"GrayText", L"ButtonText", L"InactiveTitleText",
    L"ButtonHilight", L"ButtonDkShadow", L"ButtonLight", L"InfoText", L"InfoWindow", L"ButtonAlternateFace",
    L"HotTrackingColor", L"GradientActiveTitle", L"GradientInactiveTitle", L"MenuHilight", L"MenuBar",
};

static BOOL colors_are_dark(void)
{
    return GetSysColor( COLOR_WINDOW ) == dark_colors[COLOR_WINDOW] &&
           GetSysColor( COLOR_BTNFACE ) == dark_colors[COLOR_BTNFACE];
}

/* white text on the light mode's white: half of each mode's colours */
static BOOL colors_are_mixed(void)
{
    return !colors_are_dark() && GetSysColor( COLOR_WINDOWTEXT ) == dark_colors[COLOR_WINDOWTEXT];
}

/* the dark mode's colours among the light ones go: the visual style sets
 * only some of the colours, and what it left was kept as if it were light */
static void clean_light_colors( COLORREF *colors )
{
    for (int i = 0; i <= COLOR_MENUBAR; i++)
        if (colors[i] == dark_colors[i] && light_colors[i] != CLR_INVALID) colors[i] = light_colors[i];
}

/* the light mode's colours as they are now, to come back to */
static void keep_light_colors(void)
{
    COLORREF colors[COLOR_MENUBAR + 1];
    HKEY key;

    if (colors_are_dark()) return;
    for (int i = 0; i <= COLOR_MENUBAR; i++) colors[i] = GetSysColor( i );
    clean_light_colors( colors );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, SAVED_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"LightColors", 0, REG_BINARY, (const BYTE *)colors, sizeof(colors) );
    RegCloseKey( key );
}

static BOOL style_is_11(void);

/* the menus' colour: with Windows 11's style it is the key colour of dwm,
 * black (or white), and a near black (or near white) blur shows through it
 * where the menu has nothing (dwmcore compositor.c, win32u menu.c) */
static COLORREF menu_color_11( BOOL light )
{
    return light ? RGB(0xff, 0xff, 0xff) : RGB(0, 0, 0);
}

static void set_colors( BOOL light )
{
    COLORREF colors[COLOR_MENUBAR + 1];
    INT elements[COLOR_MENUBAR + 1];
    COLORREF values[COLOR_MENUBAR + 1];
    DWORD size = sizeof(colors), type, count = 0;
    WCHAR text[16];
    HKEY key;

    memcpy( colors, light ? light_colors : dark_colors, sizeof(colors) );
    if (light && !RegOpenKeyExW( HKEY_CURRENT_USER, SAVED_KEY, 0, KEY_QUERY_VALUE, &key ))
    {
        COLORREF kept[COLOR_MENUBAR + 1];

        if (!RegQueryValueExW( key, L"LightColors", NULL, &type, (BYTE *)kept, &size ) && type == REG_BINARY &&
            size == sizeof(kept))
        {
            clean_light_colors( kept );
            memcpy( colors, kept, sizeof(colors) );
        }
        RegCloseKey( key );
    }
    colors[COLOR_BACKGROUND] = CLR_INVALID;  /* the desktop keeps its own */
    if (style_is_11()) colors[COLOR_MENU] = menu_color_11( light );

    if (RegCreateKeyExW( HKEY_CURRENT_USER, COLORS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) key = NULL;
    for (int i = 0; i <= COLOR_MENUBAR; i++)
    {
        if (colors[i] == CLR_INVALID) continue;
        elements[count] = i;
        values[count++] = colors[i];
        if (!key) continue;
        swprintf( text, ARRAY_SIZE(text), L"%u %u %u", GetRValue( colors[i] ), GetGValue( colors[i] ),
                  GetBValue( colors[i] ) );
        RegSetValueExW( key, color_names[i], 0, REG_SZ, (const BYTE *)text, (wcslen( text ) + 1) * sizeof(WCHAR) );
    }
    if (key) RegCloseKey( key );
    SetSysColors( count, elements, values );
}

static BOOL scheme_is_dark(void)
{
    WCHAR color[64];
    DWORD size = sizeof(color), type;
    HKEY key;
    BOOL dark = FALSE;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, THEME_KEY, 0, KEY_QUERY_VALUE, &key )) return FALSE;
    if (!RegQueryValueExW( key, L"ColorName", NULL, &type, (BYTE *)color, &size ) && type == REG_SZ)
        dark = !lstrcmpiW( color, DARK_SCHEME );
    RegCloseKey( key );
    return dark;
}

typedef HRESULT (WINAPI *set_visual_style_fn)( const WCHAR *file, const WCHAR *color, const WCHAR *size, UINT flags );

/* the visual style's colour scheme of the mode: every program's windows
 * draw themselves anew (WM_THEMECHANGED) */
static void set_scheme( BOOL light, const WCHAR *new_size )
{
    WCHAR file[MAX_PATH] = L"", size[64] = L"";
    DWORD len, type;
    set_visual_style_fn set_visual_style;
    HMODULE uxtheme;
    HKEY key;

    if (!RegOpenKeyExW( HKEY_CURRENT_USER, THEME_KEY, 0, KEY_QUERY_VALUE, &key ))
    {
        len = sizeof(file);
        if (RegQueryValueExW( key, L"DllName", NULL, &type, (BYTE *)file, &len ) || type != REG_SZ) file[0] = 0;
        len = sizeof(size);
        if (RegQueryValueExW( key, L"SizeName", NULL, &type, (BYTE *)size, &len ) || type != REG_SZ) size[0] = 0;
        RegCloseKey( key );
    }
    if (!file[0]) return;  /* the classic look: no visual style */
    if (new_size) lstrcpynW( size, new_size, ARRAY_SIZE(size) );
    file[ARRAY_SIZE(file) - 1] = size[ARRAY_SIZE(size) - 1] = 0;
    if (!(uxtheme = LoadLibraryW( L"uxtheme.dll" ))) return;
    if ((set_visual_style = (set_visual_style_fn)GetProcAddress( uxtheme, MAKEINTRESOURCEA(65) )))
        set_visual_style( file, light ? LIGHT_SCHEME : DARK_SCHEME, size[0] ? size : NULL, 0 );
}

/* the programs' mode, as the visual style and the colours have it: the
 * style first, so that every window draws itself once, with both. The
 * style sets some of the colours, so all of them are always set after it */
static void apply_apps_mode( BOOL light )
{
    if (!light) keep_light_colors();
    if (scheme_is_dark() == light) set_scheme( light, NULL );
    set_colors( light );
    RedrawWindow( NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
}

static BOOL style_is_11(void)
{
    WCHAR size[64] = L"";
    DWORD len = sizeof(size);

    if (RegGetValueW( HKEY_CURRENT_USER, THEME_KEY, L"SizeName", RRF_RT_REG_SZ, NULL, size, &len )) return FALSE;
    return !lstrcmpiW( size, SIZE_WIN11 );
}

/* the look of the controls: the size of the visual style with its pictures
 * and fonts, and the font of the dialogs, which programs started from now
 * on take. dwm.exe follows the size with its shadows. */
/* the interface font of the Windows 11 style: Open Sans, drawn by Segoe UI's
 * own designer, with Cyrillic (Inter until 2026-11-18) */
#define WIN11_FACE L"Open Sans"

static void set_ui_fonts( BOOL win11 )
{
    const WCHAR *face = win11 ? WIN11_FACE : L"Source Sans Pro";
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    LOGFONTW icon;

    if (SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 ))
    {
        lstrcpyW( metrics.lfCaptionFont.lfFaceName, face );
        lstrcpyW( metrics.lfSmCaptionFont.lfFaceName, face );
        lstrcpyW( metrics.lfMenuFont.lfFaceName, face );
        lstrcpyW( metrics.lfStatusFont.lfFaceName, face );
        lstrcpyW( metrics.lfMessageFont.lfFaceName, face );
        /* Open Sans is as large at the size of the menus as Source Sans Pro a point up */
        metrics.lfCaptionFont.lfHeight = win11 ? metrics.lfMenuFont.lfHeight : MulDiv( metrics.lfMenuFont.lfHeight, 13, 12 );
        SystemParametersInfoW( SPI_SETNONCLIENTMETRICS, sizeof(metrics), &metrics, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
    }
    if (SystemParametersInfoW( SPI_GETICONTITLELOGFONT, sizeof(icon), &icon, 0 ))
    {
        lstrcpyW( icon.lfFaceName, face );
        SystemParametersInfoW( SPI_SETICONTITLELOGFONT, sizeof(icon), &icon, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
    }
}

/* Windows' own UI font, which programs and web pages ask for by name and
 * DirectWrite falls back to for Latin, Cyrillic and Greek: a family of
 * that name made of the style's font (GDI and DirectWrite both read
 * Fonts\Replacements). Without it Chrome drew Cyrillic in Tahoma. */
#define REPLACEMENTS_KEY L"Software\\Arctic\\Runtime\\Fonts\\Replacements"

static void set_segoe_replacements( BOOL win11 )
{
    static const WCHAR inter[] = L"Open Sans\0", source[] = L"Source Sans Pro\0";
    static const WCHAR inter_semibold[] = L"Open Sans SemiBold\0Open Sans\0", source_semibold[] = L"Source Sans Pro Semibold\0Source Sans Pro\0";
    static const WCHAR inter_light[] = L"Open Sans Light\0Open Sans\0";
    static const struct { const WCHAR *name; const WCHAR *win11, *win10; DWORD size11, size10; } names[] =
    {
        { L"Segoe UI", inter, source, sizeof(inter), sizeof(source) },
        { L"Segoe UI Variable", inter, source, sizeof(inter), sizeof(source) },
        { L"Segoe UI Variable Text", inter, source, sizeof(inter), sizeof(source) },
        { L"Segoe UI Variable Display", inter, source, sizeof(inter), sizeof(source) },
        { L"Segoe UI Variable Small", inter, source, sizeof(inter), sizeof(source) },
        { L"Segoe UI Semibold", inter_semibold, source_semibold, sizeof(inter_semibold), sizeof(source_semibold) },
        { L"Segoe UI Light", inter_light, source, sizeof(inter_light), sizeof(source) },
        { L"Segoe UI Semilight", inter_light, source, sizeof(inter_light), sizeof(source) },
    };

    for (int i = 0; i < ARRAY_SIZE(names); i++)
        RegSetKeyValueW( HKEY_CURRENT_USER, REPLACEMENTS_KEY, names[i].name, REG_MULTI_SZ,
                         win11 ? names[i].win11 : names[i].win10, win11 ? names[i].size11 : names[i].size10 );
}

/* the fonts are what the style wants: an update puts the registry's defaults back */
static BOOL ui_fonts_right( BOOL win11 )
{
    const WCHAR *face = win11 ? WIN11_FACE : L"Source Sans Pro", *dialogs = win11 ? WIN11_FACE : L"Tahoma";
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    WCHAR value[64] = L"";
    DWORD size = sizeof(value);

    if (!SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 )) return TRUE;
    /* GDI gives the full name back: Open Sans Regular */
    if (_wcsnicmp( metrics.lfMessageFont.lfFaceName, face, lstrlenW( face ) )) return FALSE;
    /* Segoe UI came later than the rest */
    if (RegGetValueW( HKEY_CURRENT_USER, REPLACEMENTS_KEY, L"Segoe UI", RRF_RT_REG_MULTI_SZ, NULL, value, &size ) ||
        lstrcmpiW( value, face ))
        return FALSE;
    size = sizeof(value);
    return RegGetValueW( HKEY_LOCAL_MACHINE, FONT_SUBST_KEY, L"MS Shell Dlg", RRF_RT_REG_SZ, NULL, value, &size ) ||
           !lstrcmpiW( value, dialogs );
}

static void apply_style( BOOL win11 )
{
    const WCHAR *font = win11 ? WIN11_FACE : L"Tahoma";
    DWORD size = (lstrlenW( font ) + 1) * sizeof(WCHAR);
    HKEY key;

    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, FONT_SUBST_KEY, 0, KEY_SET_VALUE, &key ))
    {
        RegSetValueExW( key, L"MS Shell Dlg", 0, REG_SZ, (const BYTE *)font, size );
        RegSetValueExW( key, L"MS Shell Dlg 2", 0, REG_SZ, (const BYTE *)font, size );
        RegCloseKey( key );
    }
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, SAVED_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        set_dword( key, L"Style", win11 );
        RegCloseKey( key );
    }
    set_segoe_replacements( win11 );
    set_ui_fonts( win11 );
    set_scheme( !scheme_is_dark(), win11 ? SIZE_WIN11 : SIZE_WIN10 );
    set_colors( apps_mode_light() );
    RedrawWindow( NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
}

/* changes happen one after the other, away from the page: every window
 * of every program hears of them */
static SRWLOCK change_lock = SRWLOCK_INIT;

enum { CHANGE_APPS = 1, CHANGE_SHELL = 2, CHANGE_SETTING = 4, CHANGE_STYLE10 = 8, CHANGE_STYLE11 = 16 };

static DWORD WINAPI change_thread( void *arg )
{
    DWORD what = (DWORD)(ULONG_PTR)arg;

    AcquireSRWLockExclusive( &change_lock );
    if (what & (CHANGE_STYLE10 | CHANGE_STYLE11)) apply_style( (what & CHANGE_STYLE11) != 0 );
    if (what & CHANGE_APPS) apply_apps_mode( apps_mode_light() );
    if (what & CHANGE_SETTING)
        SendMessageTimeoutW( HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                             SMTO_ABORTIFHUNG, 2000, NULL );
    /* the taskbar, Start and the flyouts take the mode's look */
    if (what & CHANGE_SHELL) PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
    ReleaseSRWLockExclusive( &change_lock );
    return 0;
}

static void changes( DWORD what )
{
    HANDLE thread = CreateThread( NULL, 0, change_thread, (void *)(ULONG_PTR)what, 0, NULL );

    if (thread) CloseHandle( thread );
    else change_thread( (void *)(ULONG_PTR)what );
}

/* explorer's taskbar has come: after an update put the registry's
 * defaults back, the visual style and the colours are the mode's again */
void personalize_sync(void)
{
    static LONG done;
    BOOL light, found, style11;
    DWORD what = 0;
    HKEY key;

    if (InterlockedExchange( &done, 1 )) return;
    /* Windows 11's defaults, the light mode, written as there: programs that
     * choose the colour of their notification area icons read them */
    reg_dword( HKEY_CURRENT_USER, PERSONALIZE_KEY, L"SystemUsesLightTheme", 1, &found );
    if (!found && !RegCreateKeyExW( HKEY_CURRENT_USER, PERSONALIZE_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        set_dword( key, L"SystemUsesLightTheme", 1 );
        set_dword( key, L"AppsUseLightTheme", apps_mode_light() );
        RegCloseKey( key );
    }
    light = apps_mode_light();
    if (colors_are_dark() == light || scheme_is_dark() == light || (light && colors_are_mixed())) what |= CHANGE_APPS | CHANGE_SETTING;
    /* the look of the controls the user chose, which an update has put back to
     * the defaults; Windows 11's when nothing was chosen */
    style11 = reg_dword( HKEY_CURRENT_USER, SAVED_KEY, L"Style", 1, NULL ) != 0;
    if (style11 != style_is_11() || !ui_fonts_right( style11 )) what |= (style11 ? CHANGE_STYLE11 : CHANGE_STYLE10) | CHANGE_SETTING;
    /* the menus' colour of the style, from before there was one */
    if ((GetSysColor( COLOR_MENU ) == menu_color_11( light )) != style11 && (style11 || !light))
        what |= CHANGE_APPS | CHANGE_SETTING;
    if (what) changes( what );
}

/**********************************************************************
 *          The title bars
 */

struct caption
{
    struct look look;   /* effect 0: none */
    BOOL has_color;     /* a colour of its own, not the mode's */
};

static COLORREF mode_tint( BOOL light )
{
    return light ? RGB(0xf3, 0xf3, 0xf3) : RGB(0x20, 0x20, 0x20);
}

static void caption_load( struct caption *cap )
{
    BOOL found;

    cap->look.preset = 0;
    cap->look.effect = reg_dword( HKEY_CURRENT_USER, CAPTION_KEY, L"Effect", ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, NULL );
    if (cap->look.effect > ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND)
        cap->look.effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    cap->look.color = reg_dword( HKEY_CURRENT_USER, CAPTION_KEY, L"Color", 0, &found ) & 0xffffff;
    cap->has_color = found;
    if (!found) cap->look.color = mode_tint( system_mode_light() );
    cap->look.opacity = min( reg_dword( HKEY_CURRENT_USER, CAPTION_KEY, L"Opacity", CAPTION_OPACITY, NULL ), 100 );
    cap->look.blur = max( 1, min( reg_dword( HKEY_CURRENT_USER, CAPTION_KEY, L"Blur", CAPTION_BLUR, NULL ), 100 ) );
    cap->look.text = reg_dword( HKEY_CURRENT_USER, CAPTION_KEY, L"Text", TEXT_AUTO, NULL );
    if (cap->look.text >= TEXT_COUNT) cap->look.text = TEXT_AUTO;
}

/* uxtheme draws dark on light (Light) over a colour of its own or as
 * chosen; with neither, as the mode is */
static void caption_save( const struct caption *cap )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, CAPTION_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    set_dword( key, L"Effect", cap->look.effect );
    set_dword( key, L"Opacity", cap->look.opacity );
    set_dword( key, L"Blur", cap->look.blur );
    set_dword( key, L"Text", cap->look.text );
    if (cap->has_color) set_dword( key, L"Color", cap->look.color );
    else RegDeleteValueW( key, L"Color" );
    if (cap->has_color || cap->look.text != TEXT_AUTO) set_dword( key, L"Light", look_is_light( &cap->look ) );
    else RegDeleteValueW( key, L"Light" );
    RegCloseKey( key );
}

/**********************************************************************
 *          The mode changes
 */

static BOOL is_shade( COLORREF color )
{
    int r = GetRValue( color ), g = GetGValue( color ), b = GetBValue( color );

    return max( r, max( g, b ) ) - min( r, min( g, b ) ) <= 0x18;
}

/* the colour of one place for the mode that comes: the one it had in that
 * mode, or a shade made the mode's; a real colour stays */
static void color_follow_mode( const WCHAR *path, BOOL light )
{
    const WCHAR *leaving = light ? L"ColorDark" : L"ColorLight", *coming = light ? L"ColorLight" : L"ColorDark";
    DWORD color, kept, size = sizeof(color), type;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key )) return;
    if (!RegQueryValueExW( key, L"Color", NULL, &type, (BYTE *)&color, &size ) && type == REG_DWORD)
    {
        size = sizeof(kept);
        if (RegQueryValueExW( key, coming, NULL, &type, (BYTE *)&kept, &size ) || type != REG_DWORD)
            kept = is_shade( color & 0xffffff ) ? mode_tint( light ) : color;
        set_dword( key, leaving, color );
        set_dword( key, L"Color", kept );
    }
    RegCloseKey( key );
}

static void set_modes( BOOL system_light, BOOL apps_light )
{
    BOOL system_was = system_mode_light(), apps_was = apps_mode_light();
    DWORD what = CHANGE_SETTING;
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, PERSONALIZE_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    set_dword( key, L"SystemUsesLightTheme", system_light );
    set_dword( key, L"AppsUseLightTheme", apps_light );
    RegCloseKey( key );
    if (apps_light != apps_was) what |= CHANGE_APPS;
    if (system_light != system_was)
    {
        struct caption cap;

        color_follow_mode( TASKBAR_KEY, system_light );
        color_follow_mode( START_LOOK_KEY L"Windows10", system_light );
        color_follow_mode( START_LOOK_KEY L"Windows11", system_light );
        color_follow_mode( CAPTION_KEY, system_light );
        /* the text on the title bars, as their new colour wants it */
        caption_load( &cap );
        caption_save( &cap );
        what |= CHANGE_SHELL;
    }
    changes( what );
}

/**********************************************************************
 *          The page
 */

struct page
{
    int mode;           /* enum mode, as the tiles show it */
    BOOL system_light;
    BOOL apps_light;
    struct caption cap;
    BOOL filling;
    HFONT label_font;
    BOOL style11;       /* the controls of Windows 11 */
};

static int mode_of( BOOL system_light, BOOL apps_light )
{
    if (system_light != apps_light) return MODE_CUSTOM;
    return system_light ? MODE_LIGHT : MODE_DARK;
}

static void fill_rect( HDC hdc, int left, int top, int right, int bottom, COLORREF color )
{
    RECT rect = { left, top, right, bottom };
    COLORREF old = SetBkColor( hdc, color );

    ExtTextOutW( hdc, 0, 0, ETO_OPAQUE, &rect, NULL, 0, NULL );
    SetBkColor( hdc, old );
}

/* a desktop in small: a window with its title bar in the mode of Windows
 * and its contents in the programs', the taskbar under it */
static void draw_tile( HWND dialog, struct page *page, const DRAWITEMSTRUCT *item )
{
    int mode = item->CtlID - ID_TILE_FIRST;
    BOOL system_light = mode == MODE_LIGHT, apps_light = mode != MODE_DARK;
    BOOL selected = mode == page->mode;
    HDC hdc = item->hDC;
    RECT rect = item->rcItem, label;
    TEXTMETRICW tm;
    HGDIOBJ old_font;
    int width, height, x0, y0, x1, y1, bar, caption, unit;
    COLORREF accent = accent_color();

    fill_rect( hdc, rect.left, rect.top, rect.right, rect.bottom, GetSysColor( COLOR_BTNFACE ) );
    old_font = SelectObject( hdc, (HFONT)SendMessageW( dialog, WM_GETFONT, 0, 0 ) );
    GetTextMetricsW( hdc, &tm );

    label = rect;
    label.top = rect.bottom - tm.tmHeight - 4;
    x0 = rect.left + 3;
    y0 = rect.top + 3;
    x1 = rect.right - 3;
    y1 = label.top - 3;
    width = x1 - x0;
    height = y1 - y0;
    unit = max( 1, height / 20 );

    if (selected)
    {
        fill_rect( hdc, x0 - 3, y0 - 3, x1 + 3, y0 - 1, accent );
        fill_rect( hdc, x0 - 3, y1 + 1, x1 + 3, y1 + 3, accent );
        fill_rect( hdc, x0 - 3, y0 - 1, x0 - 1, y1 + 1, accent );
        fill_rect( hdc, x1 + 1, y0 - 1, x1 + 3, y1 + 1, accent );
    }

    /* the wallpaper: a blue from top to bottom */
    for (int y = y0; y < y1; y++)
    {
        int t = (y - y0) * 255 / max( 1, height );
        fill_rect( hdc, x0, y, x1, y + 1, RGB( 0x1e + t * 0x30 / 255, 0x4f + t * 0x40 / 255, 0x8c + t * 0x50 / 255 ) );
    }

    /* the window */
    bar = max( 4, height / 7 );
    caption = max( 3, height / 9 );
    {
        int wx0 = x0 + width / 6, wy0 = y0 + height / 7, wx1 = x1 - width / 6, wy1 = y1 - bar - height / 9;
        COLORREF body = apps_light ? RGB(0xff, 0xff, 0xff) : RGB(0x19, 0x19, 0x19);
        COLORREF ink = apps_light ? RGB(0xd0, 0xd0, 0xd0) : RGB(0x48, 0x48, 0x48);

        fill_rect( hdc, wx0, wy0, wx1, wy0 + caption, system_light ? RGB(0xf3, 0xf3, 0xf3) : RGB(0x20, 0x20, 0x20) );
        fill_rect( hdc, wx0, wy0 + caption, wx1, wy1, body );
        /* lines of text, and a button of the accent's colour */
        for (int i = 0; i < 3; i++)
        {
            int ly = wy0 + caption + unit * 3 + i * unit * 3;
            if (ly + unit > wy1 - unit * 4) break;
            fill_rect( hdc, wx0 + unit * 3, ly, wx1 - unit * (3 + i * 5), ly + unit, ink );
        }
        fill_rect( hdc, wx1 - unit * 12, wy1 - unit * 5, wx1 - unit * 3, wy1 - unit * 2, accent );
    }

    /* the taskbar, its buttons in the middle */
    fill_rect( hdc, x0, y1 - bar, x1, y1, system_light ? RGB(0xee, 0xee, 0xee) : RGB(0x1c, 0x1c, 0x1c) );
    for (int i = -1; i <= 1; i++)
    {
        int cx = (x0 + x1) / 2 + i * (bar + unit), half = max( 1, bar / 4 );
        fill_rect( hdc, cx - half, (2 * y1 - bar) / 2 - half, cx + half, (2 * y1 - bar) / 2 + half,
                   i ? (system_light ? RGB(0x70, 0x70, 0x70) : RGB(0xa0, 0xa0, 0xa0)) : accent );
    }

    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, GetSysColor( (item->itemState & ODS_DISABLED) ? COLOR_GRAYTEXT : COLOR_BTNTEXT ) );
    DrawTextW( hdc, load_string( IDS_MODE_LIGHT + mode ), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
    if (item->itemState & ODS_FOCUS)
    {
        RECT focus = label;
        InflateRect( &focus, -2, 0 );
        DrawFocusRect( hdc, &focus );
    }
    SelectObject( hdc, old_font );
}

static void show_page( HWND dialog, struct page *page )
{
    BOOL custom = page->mode == MODE_CUSTOM;

    page->filling = TRUE;
    for (int i = 0; i < MODE_COUNT; i++) InvalidateRect( GetDlgItem( dialog, ID_TILE_FIRST + i ), NULL, FALSE );
    SendDlgItemMessageW( dialog, ID_SYSTEM, CB_SETCURSEL, page->system_light ? 0 : 1, 0 );
    SendDlgItemMessageW( dialog, ID_APPS, CB_SETCURSEL, page->apps_light ? 0 : 1, 0 );
    EnableWindow( GetDlgItem( dialog, ID_SYSTEM_LABEL ), custom );
    EnableWindow( GetDlgItem( dialog, ID_SYSTEM ), custom );
    EnableWindow( GetDlgItem( dialog, ID_APPS_LABEL ), custom );
    EnableWindow( GetDlgItem( dialog, ID_APPS ), custom );
    SendDlgItemMessageW( dialog, ID_STYLE, CB_SETCURSEL, page->style11, 0 );
    page->filling = FALSE;
}

static void mode_changed( HWND dialog, struct page *page )
{
    show_page( dialog, page );
    set_modes( page->system_light, page->apps_light );
    caption_load( &page->cap );
}

/**********************************************************************
 *          The window of the title bars
 */

static void show_captions( HWND dialog, struct page *page )
{
    BOOL effect = page->cap.look.effect != 0;

    page->filling = TRUE;
    look_controls_show( dialog, ID_CAPTION_FIRST, &page->cap.look, TRUE );
    if (!effect)
    {
        SendDlgItemMessageW( dialog, ID_CAPTION_FIRST + LC_EFFECT, CB_SETCURSEL, LOOK_EFFECT_COUNT, 0 );
        for (int id = LC_COLOR_LABEL; id < LC_COUNT; id++) EnableWindow( GetDlgItem( dialog, ID_CAPTION_FIRST + id ), FALSE );
    }
    EnableWindow( GetDlgItem( dialog, ID_RESET ), effect );
    page->filling = FALSE;
}

/* the title bars as in the window, at once */
static void caption_changed( HWND dialog, struct page *page )
{
    show_captions( dialog, page );
    caption_save( &page->cap );
    /* a slider sends many: the windows redraw once it rests */
    SetTimer( dialog, APPLY_TIMER, 150, NULL );
}

static void init_captions( HWND dialog, struct page *page )
{
    HWND owner = GetWindow( dialog, GW_OWNER );
    RECT rect, over;
    int y = look_controls_create( dialog, 7, ID_CAPTION_FIRST ) + 6;

    SetWindowTextW( dialog, load_string( IDS_CAPTIONS ) );
    SendDlgItemMessageW( dialog, ID_CAPTION_FIRST + LC_EFFECT, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_EFFECT_NONE ) );
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_RESET ), BS_PUSHBUTTON | WS_TABSTOP, 7, y, 60, 14, ID_RESET );
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_OK ), BS_DEFPUSHBUTTON | WS_TABSTOP, 195, y, 50, 14, IDOK );
    show_captions( dialog, page );

    if (owner && GetWindowRect( owner, &over ) && GetWindowRect( dialog, &rect ))
    {
        int width = rect.right - rect.left, height = rect.bottom - rect.top;

        SetWindowPos( dialog, NULL, (over.left + over.right - width) / 2, max( 0, (int)(over.top + over.bottom - height) / 2 ),
                      0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    }
}

static INT_PTR WINAPI captions_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct page *page = (struct page *)GetWindowLongPtrW( dialog, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        page = (struct page *)lparam;
        SetWindowLongPtrW( dialog, DWLP_USER, (LONG_PTR)page );
        init_captions( dialog, page );
        return TRUE;

    case WM_COMMAND:
        if (!page || page->filling) break;
        if (LOWORD( wparam ) == IDOK || LOWORD( wparam ) == IDCANCEL)
        {
            /* a change still waiting for its slider to rest */
            if (KillTimer( dialog, APPLY_TIMER )) changes( CHANGE_SETTING );
            EndDialog( dialog, 0 );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_RESET)
        {
            page->cap.has_color = FALSE;
            page->cap.look.color = mode_tint( page->system_light );
            page->cap.look.opacity = CAPTION_OPACITY;
            page->cap.look.blur = CAPTION_BLUR;
            page->cap.look.text = TEXT_AUTO;
            caption_changed( dialog, page );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_CAPTION_FIRST + LC_EFFECT)
        {
            LRESULT sel = SendDlgItemMessageW( dialog, LOWORD( wparam ), CB_GETCURSEL, 0, 0 );

            if (HIWORD( wparam ) != CBN_SELCHANGE || sel < 0) break;
            page->cap.look.effect = sel < LOOK_EFFECT_COUNT ? look_effects[sel].effect : 0;
            caption_changed( dialog, page );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_CAPTION_FIRST + LC_COLOR)
        {
            if (look_controls_command( dialog, ID_CAPTION_FIRST, &page->cap.look, msg, wparam, lparam ))
            {
                page->cap.has_color = TRUE;
                caption_changed( dialog, page );
            }
            return TRUE;
        }
        if (look_controls_command( dialog, ID_CAPTION_FIRST, &page->cap.look, msg, wparam, lparam ))
        {
            caption_changed( dialog, page );
            return TRUE;
        }
        break;

    case WM_HSCROLL:
        if (page && !page->filling &&
            look_controls_command( dialog, ID_CAPTION_FIRST, &page->cap.look, msg, wparam, lparam ))
            caption_changed( dialog, page );
        return TRUE;

    case WM_TIMER:
        if (wparam != APPLY_TIMER) break;
        KillTimer( dialog, APPLY_TIMER );
        changes( CHANGE_SETTING );
        return TRUE;

    case WM_DRAWITEM:
        if (page && look_controls_draw( ID_CAPTION_FIRST, &page->cap.look, wparam, lparam )) return TRUE;
        break;
    }
    return FALSE;
}

/* the size of the rows, the buttons under them, in DLUs */
static const struct page_template captions_template =
{
    { DS_SETFONT | DS_MODALFRAME | DS_SETFOREGROUND | DS_3DLOOK | WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
      0, 0, 0, 0, 252, 134 },
    0, 0, 0, 8, L"MS Shell Dlg",
};

/* the icon theme: a folder of %WINDIR%\\Resources\\Icons that user32 takes the
 * icons of shell32 and the others from; We10X unless the system's own were
 * chosen. shell32 makes the icons it keeps again when it notices; explorer's
 * windows repaint (gadgets.c). */
#define ICONS_KEY  L"Software\\Arctic\\Icons"
#define ICONS_WE10X L"We10X"

static BOOL icons_we10x(void)
{
    WCHAR name[64] = ICONS_WE10X;
    DWORD size = sizeof(name);

    RegGetValueW( HKEY_CURRENT_USER, ICONS_KEY, L"Theme", RRF_RT_REG_SZ, NULL, name, &size );
    return !lstrcmpiW( name, ICONS_WE10X );
}

/* a folder of %WINDIR%\Resources\Icons, "" for the system's own */
static void set_icon_theme( const WCHAR *name )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, ICONS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"Theme", 0, REG_SZ, (const BYTE *)name, (lstrlenW( name ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    SendMessageTimeoutW( HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)ICONS_CHANGED, SMTO_ABORTIFHUNG, 2000, NULL );
    SHChangeNotify( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );
}

static void set_icons( BOOL we10x )
{
    set_icon_theme( we10x ? ICONS_WE10X : L"" );
}

static void init_page( HWND dialog, struct page *page )
{
    HWND combo;
    int y;

    page->system_light = system_mode_light();
    page->apps_light = apps_mode_light();
    page->mode = mode_of( page->system_light, page->apps_light );
    page->style11 = style_is_11();
    caption_load( &page->cap );

    add_dialog_control( dialog, L"STATIC", load_string( IDS_MODE ), 0, 7, 7, 232, 10, ID_MODE_LABEL );
    for (int i = 0; i < MODE_COUNT; i++)
        add_dialog_control( dialog, L"BUTTON", load_string( IDS_MODE_LIGHT + i ), BS_OWNERDRAW | WS_TABSTOP,
                            7 + i * 80, 19, 72, 62, ID_TILE_FIRST + i );

    y = 88;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_MODE_WINDOWS ), SS_CENTERIMAGE, 7, y, 66, 14, ID_SYSTEM_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 60,
                                ID_SYSTEM );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_MODE_LIGHT ) );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_MODE_DARK ) );
    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_MODE_APPS ), SS_CENTERIMAGE, 7, y, 66, 14, ID_APPS_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 60,
                                ID_APPS );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_MODE_LIGHT ) );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_MODE_DARK ) );

    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_CONTROLS_STYLE ), SS_CENTERIMAGE, 7, y, 66, 14, ID_STYLE_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 60,
                                ID_STYLE );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)L"Windows 10" );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)L"Windows 11" );

    y += 26;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_CAPTIONS ), SS_CENTERIMAGE, 7, y, 160, 14, ID_CAPTIONS );
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_CUSTOMIZE ), BS_PUSHBUTTON | WS_TABSTOP, 180, y, 65, 14,
                        ID_CAPTION_OPEN );

    y += 26;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_ICONS ), SS_CENTERIMAGE, 7, y, 66, 14, ID_ICONS_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 60,
                                ID_ICONS );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_ICONS_STANDARD ) );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)ICONS_WE10X );
    SendMessageW( combo, CB_SETCURSEL, icons_we10x(), 0 );

    /* the group titles in the dialog's font, a little heavier */
    {
        LOGFONTW lf;
        HFONT font = (HFONT)SendMessageW( dialog, WM_GETFONT, 0, 0 );

        if (font && GetObjectW( font, sizeof(lf), &lf ))
        {
            lf.lfWeight = FW_SEMIBOLD;
            page->label_font = CreateFontIndirectW( &lf );
        }
        if (page->label_font)
        {
            SendDlgItemMessageW( dialog, ID_CAPTIONS, WM_SETFONT, (WPARAM)page->label_font, FALSE );
            SendDlgItemMessageW( dialog, ID_MODE_LABEL, WM_SETFONT, (WPARAM)page->label_font, FALSE );
        }
    }
    show_page( dialog, page );
}

static INT_PTR WINAPI page_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct page *page = (struct page *)GetWindowLongPtrW( dialog, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        page = (struct page *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW( dialog, DWLP_USER, (LONG_PTR)page );
        init_page( dialog, page );
        return TRUE;

    case WM_COMMAND:
        if (!page || page->filling) break;
        if (LOWORD( wparam ) >= ID_TILE_FIRST && LOWORD( wparam ) < ID_TILE_FIRST + MODE_COUNT)
        {
            int mode = LOWORD( wparam ) - ID_TILE_FIRST;

            if (HIWORD( wparam ) != BN_CLICKED) break;
            page->mode = mode;
            if (mode == MODE_LIGHT) page->system_light = page->apps_light = TRUE;
            else if (mode == MODE_DARK) page->system_light = page->apps_light = FALSE;
            mode_changed( dialog, page );
            return TRUE;
        }
        if ((LOWORD( wparam ) == ID_SYSTEM || LOWORD( wparam ) == ID_APPS) && HIWORD( wparam ) == CBN_SELCHANGE)
        {
            BOOL light = SendDlgItemMessageW( dialog, LOWORD( wparam ), CB_GETCURSEL, 0, 0 ) == 0;

            if (LOWORD( wparam ) == ID_SYSTEM) page->system_light = light;
            else page->apps_light = light;
            mode_changed( dialog, page );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_STYLE && HIWORD( wparam ) == CBN_SELCHANGE)
        {
            BOOL win11 = SendDlgItemMessageW( dialog, ID_STYLE, CB_GETCURSEL, 0, 0 ) == 1;

            if (win11 != page->style11)
            {
                page->style11 = win11;
                changes( (win11 ? CHANGE_STYLE11 : CHANGE_STYLE10) | CHANGE_SETTING );
            }
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_ICONS && HIWORD( wparam ) == CBN_SELCHANGE)
        {
            BOOL we10x = SendDlgItemMessageW( dialog, ID_ICONS, CB_GETCURSEL, 0, 0 ) == 1;

            if (we10x != icons_we10x()) set_icons( we10x );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_CAPTION_OPEN && HIWORD( wparam ) == BN_CLICKED)
        {
            DialogBoxIndirectParamW( startui_instance, &captions_template.header, dialog, captions_proc, (LPARAM)page );
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (page && wparam >= ID_TILE_FIRST && wparam < ID_TILE_FIRST + MODE_COUNT)
        {
            draw_tile( dialog, page, (const DRAWITEMSTRUCT *)lparam );
            return TRUE;
        }
        break;

    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        /* the page itself in the mode it chose */
        InvalidateRect( dialog, NULL, TRUE );
        break;

    case WM_NOTIFY:
        if (((NMHDR *)lparam)->code == PSN_APPLY)
        {
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static UINT CALLBACK page_callback( HWND hwnd, UINT msg, PROPSHEETPAGEW *psp )
{
    struct page *page = (struct page *)psp->lParam;

    if (msg == PSPCB_RELEASE && page)
    {
        if (page->label_font) DeleteObject( page->label_font );
        free( page );
    }
    return 1;
}

/* the size of desk.cpl's pages, in DLUs */
static const struct page_template personalize_template =
{
    { DS_SETFONT | WS_CHILD | WS_CAPTION | DS_3DLOOK | DS_CONTROL, 0, 0, 0, 0, 252, 250 },
    0, 0, 0, 8, L"MS Shell Dlg",
};

/**********************************************************************
 *          For the Control Panel's Personalization (displaycpl.dll)
 *
 * Its page is drawn as Windows 11's and changes what this page changes,
 * the same way: the modes, the look of the controls, the icon theme, and
 * the window of the title bars.
 */

/* 1 Windows light, 2 programs light, 4 the controls of Windows 11 */
DWORD WINAPI ArcticPersonalizeGet(void)
{
    return (system_mode_light() ? 1 : 0) | (apps_mode_light() ? 2 : 0) |
           (reg_dword( HKEY_CURRENT_USER, SAVED_KEY, L"Style", 1, NULL ) ? 4 : 0);
}

void WINAPI ArcticPersonalizeSetModes( BOOL system_light, BOOL apps_light )
{
    system_light = system_light != 0;
    apps_light = apps_light != 0;
    if (system_light == system_mode_light() && apps_light == apps_mode_light()) return;
    set_modes( system_light, apps_light );
}

void WINAPI ArcticPersonalizeSetStyle( BOOL win11 )
{
    HKEY key;

    win11 = win11 != 0;
    if (win11 == (reg_dword( HKEY_CURRENT_USER, SAVED_KEY, L"Style", 1, NULL ) != 0) && win11 == style_is_11()) return;
    /* written now, so that the page shows it before the thread has applied it */
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, SAVED_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        set_dword( key, L"Style", win11 );
        RegCloseKey( key );
    }
    changes( (win11 ? CHANGE_STYLE11 : CHANGE_STYLE10) | CHANGE_SETTING );
}

void WINAPI ArcticPersonalizeSetIconTheme( const WCHAR *name )
{
    WCHAR current[64] = ICONS_WE10X;
    DWORD size = sizeof(current);

    if (!name) name = L"";
    RegGetValueW( HKEY_CURRENT_USER, ICONS_KEY, L"Theme", RRF_RT_REG_SZ, NULL, current, &size );
    if (!lstrcmpiW( current, name )) return;
    set_icon_theme( name );
}

/* the window of the title bars, over the page */
void WINAPI ArcticPersonalizeCaptions( HWND owner )
{
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return;
    page->system_light = system_mode_light();
    page->apps_light = apps_mode_light();
    page->mode = mode_of( page->system_light, page->apps_light );
    page->style11 = style_is_11();
    caption_load( &page->cap );
    DialogBoxIndirectParamW( startui_instance, &captions_template.header, owner, captions_proc, (LPARAM)page );
    free( page );
}

/* the page "Персоналізація", for desk.cpl to show instead of its old appearance */
HPROPSHEETPAGE WINAPI ArcticPersonalizePage(void)
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return NULL;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = startui_instance;
    psp.pResource = &personalize_template.header;
    psp.pfnDlgProc = page_proc;
    psp.pszTitle = load_string( IDS_PERSONALIZE_TITLE );
    psp.lParam = (LPARAM)page;
    psp.pfnCallback = page_callback;
    return CreatePropertySheetPageW( &psp );
}
