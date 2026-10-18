/*
 * The look of the taskbar and of the Start menus, and the taskbar's page
 * "Оформлення"
 *
 * What lies behind the taskbar shows through as dwm.exe composes it for
 * SetWindowCompositionAttribute(WCA_ACCENT_POLICY): acrylic (blurred,
 * tinted, grained), a blur without the grain, a see-through tint without
 * blur, or a solid colour. The presets are the taskbars of Windows 10 (the
 * default: acrylic, #101010 at 75%) and Windows 11 (#1C1C1C at 85%), the
 * accent colour, a clear blur, a glassy see-through and a solid one, and
 * Custom: any effect, colour, opacity and blur. They live in
 * HKCU\Software\Arctic\Taskbar (Preset, Effect, Color, Opacity, Blur, Text)
 * and the page "Оформлення" of the taskbar's properties chooses them.
 *
 * The text is white over a dark look and black over a light one, by itself
 * (Text 0), or as chosen (1 white, 2 black). A surface with black text is
 * drawn dark on white, and dwm.exe keys its white instead of its black
 * (DWMWA_ARCTIC_LIGHT).
 *
 * Each style of the Start menu takes the taskbar's look, or, with Inherit
 * off, its own: the standard menu its acrylic of #202020 at 80%, the
 * classic one none, the menus of Windows 10 and 11 what is chosen for them
 * in HKCU\Software\Arctic\StartMenu\Look\<style>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "startui.h"
#include "winreg.h"
#include "commctrl.h"
#include "commdlg.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define REG_KEY       L"Software\\Arctic\\Taskbar"
#define START_KEY     L"Software\\Arctic\\StartMenu"

#define ID_PRESET     1200
#define ID_EFFECT     1201
#define ID_COLOR      1202
#define ID_OPACITY    1203
#define ID_PERCENT    1204
#define ID_BLUR       1205
#define ID_BLUR_VALUE 1206
#define ID_TEXT       1207

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum preset { PRESET_WIN10, PRESET_WIN11, PRESET_ACCENT, PRESET_BLUR, PRESET_CLEAR, PRESET_SOLID, PRESET_CUSTOM, PRESET_COUNT };

static const UINT preset_names[PRESET_COUNT] =
{
    IDS_LOOK_WIN10, IDS_LOOK_WIN11, IDS_LOOK_ACCENT, IDS_LOOK_BLUR, IDS_LOOK_CLEAR, IDS_LOOK_SOLID, IDS_LOOK_CUSTOM,
};

const struct look_effect look_effects[LOOK_EFFECT_COUNT] =
{
    { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND,   IDS_EFFECT_ACRYLIC },
    { ARCTIC_ACCENT_ENABLE_BLURBEHIND,          IDS_EFFECT_BLUR },
    { ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT, IDS_EFFECT_CLEAR },
    { ARCTIC_ACCENT_ENABLE_GRADIENT,            IDS_EFFECT_SOLID },
};

const UINT look_text_names[TEXT_COUNT] = { IDS_TEXT_AUTO, IDS_TEXT_WHITE, IDS_TEXT_BLACK };

COLORREF accent_color(void)
{
    DWORD color;
    BOOL opaque;

    if (SUCCEEDED(DwmGetColorizationColor( &color, &opaque )) && (color & 0xffffff))
        return RGB( (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff );
    return RGB( 0x00, 0x78, 0xd7 );
}

/* what a preset stands for */
static void preset_look( struct look *look )
{
    look->blur = LOOK_BLUR_DEFAULT;
    look->text = TEXT_AUTO;
    switch (look->preset)
    {
    case PRESET_WIN10:
        look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
        look->color = RGB( 0x10, 0x10, 0x10 );
        look->opacity = 75;
        break;
    case PRESET_WIN11:
        look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
        look->color = RGB( 0x1c, 0x1c, 0x1c );
        look->opacity = 85;
        break;
    case PRESET_ACCENT:
        look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
        look->color = accent_color();
        look->opacity = 80;
        break;
    case PRESET_BLUR:
        look->effect = ARCTIC_ACCENT_ENABLE_BLURBEHIND;
        look->color = RGB( 0, 0, 0 );
        look->opacity = 30;
        break;
    case PRESET_CLEAR:
        look->effect = ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT;
        look->color = RGB( 0, 0, 0 );
        look->opacity = 25;
        break;
    case PRESET_SOLID:
        look->effect = ARCTIC_ACCENT_ENABLE_GRADIENT;
        look->color = RGB( 0x10, 0x10, 0x10 );
        look->opacity = 100;
        break;
    }
}

static DWORD read_dword( HKEY key, const WCHAR *name, DWORD def )
{
    DWORD value, size = sizeof(value), type;

    if (key && !RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size ) && type == REG_DWORD) return value;
    return def;
}

static void write_dword( HKEY key, const WCHAR *name, DWORD value )
{
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
}

/* effect, colour, opacity, blur and text of a key, over what the look already has */
static void read_look_values( HKEY key, struct look *look )
{
    look->effect = read_dword( key, L"Effect", look->effect );
    if (look->effect < ARCTIC_ACCENT_ENABLE_GRADIENT || look->effect > ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND)
        look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    look->color = read_dword( key, L"Color", look->color ) & 0xffffff;
    look->opacity = min( read_dword( key, L"Opacity", look->opacity ), 100 );
    look->blur = max( 1, min( read_dword( key, L"Blur", look->blur ), 100 ) );
    look->text = read_dword( key, L"Text", look->text );
    if (look->text >= TEXT_COUNT) look->text = TEXT_AUTO;
}

static void write_look_values( HKEY key, const struct look *look )
{
    write_dword( key, L"Effect", look->effect );
    write_dword( key, L"Color", look->color );
    write_dword( key, L"Opacity", look->opacity );
    write_dword( key, L"Blur", look->blur );
    write_dword( key, L"Text", look->text );
}

void look_load_taskbar( struct look *look )
{
    HKEY key = NULL;

    RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key );
    look->preset = read_dword( key, L"Preset", PRESET_WIN10 );
    if (look->preset >= PRESET_COUNT) look->preset = PRESET_WIN10;
    preset_look( look );
    if (look->preset == PRESET_CUSTOM)
    {
        look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
        look->color = RGB( 0x10, 0x10, 0x10 );
        look->opacity = 75;
        read_look_values( key, look );
    }
    else
    {
        /* a preset keeps the text and the blur chosen with it */
        look->blur = max( 1, min( read_dword( key, L"Blur", look->blur ), 100 ) );
        look->text = read_dword( key, L"Text", look->text );
        if (look->text >= TEXT_COUNT) look->text = TEXT_AUTO;
    }
    if (key) RegCloseKey( key );
}

static void save_taskbar_look( const struct look *look )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    write_dword( key, L"Preset", look->preset );
    write_look_values( key, look );
    RegCloseKey( key );
}

/* sRGB to the light it gives, 0-1 */
static float linear( BYTE c )
{
    float v = c / 255.0f;
    return v <= 0.04045f ? v / 12.92f : powf( (v + 0.055f) / 1.055f, 2.4f );
}

/* Black text where it reads better than white: over the tint as dwm.exe
 * lays it, with a dim backdrop showing through what is not tinted. Black
 * wins where the luminance is over 0.18, where the contrasts are equal. */
BOOL look_is_light( const struct look *look )
{
    float tint, alpha, behind = 0.08f;

    if (look->text == TEXT_WHITE) return FALSE;
    if (look->text == TEXT_BLACK) return TRUE;
    tint = 0.2126f * linear( GetRValue( look->color ) ) + 0.7152f * linear( GetGValue( look->color ) ) +
           0.0722f * linear( GetBValue( look->color ) );
    alpha = look->effect == ARCTIC_ACCENT_ENABLE_GRADIENT ? 1.0f : look->opacity / 100.0f;
    return alpha * tint + (1 - alpha) * behind > 0.18f;
}

/* the window takes the look; light: it draws dark on white */
static void apply_look( HWND hwnd, const struct look *look, BOOL light )
{
    DWORD alpha = look->effect == ARCTIC_ACCENT_ENABLE_GRADIENT ? 255 : look->opacity * 255 / 100;
    struct arctic_accent_policy policy = { look->effect, 0, 0, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
    DWORD blur = look->blur, keyed_light = light;

    /* 0xAABBGGRR */
    policy.gradient_color = (alpha << 24) | (GetBValue( look->color ) << 16) | (GetGValue( look->color ) << 8) |
                            GetRValue( look->color );
    DwmSetWindowAttribute( hwnd, DWMWA_ARCTIC_BLUR, &blur, sizeof(blur) );
    DwmSetWindowAttribute( hwnd, DWMWA_ARCTIC_LIGHT, &keyed_light, sizeof(keyed_light) );
    SetWindowCompositionAttribute( hwnd, &attr );
    RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN );
}

BOOL look_apply( HWND hwnd, const struct look *look )
{
    BOOL light = look_is_light( look );

    apply_look( hwnd, look, light );
    return light;
}

/* the taskbar takes the look chosen: when it is made, and when the page
 * applies. explorer draws it white on black, whatever the text. */
/* explorer's taskbar, with this library in its process: whether it draws
 * dark on white now, for its own parts and the icons of its notification
 * area; and whether it can (it calls ArcticApplyTaskbarLookEx) */
static BOOL taskbar_light, taskbar_can_be_light;

static void apply_taskbar( HWND tray, const struct look *look )
{
    taskbar_light = taskbar_can_be_light && look_is_light( look );
    apply_look( tray, look, taskbar_light );
}

void WINAPI ArcticApplyTaskbarLook( HWND tray )
{
    struct look look;

    look_load_taskbar( &look );
    apply_taskbar( tray, &look );
}

/* ... and a taskbar that draws dark on white over a light look: says which */
BOOL WINAPI ArcticApplyTaskbarLookEx( HWND tray )
{
    taskbar_can_be_light = TRUE;
    ArcticApplyTaskbarLook( tray );
    return taskbar_light;
}

BOOL WINAPI ArcticTaskbarIsLight(void)
{
    return taskbar_light;
}

/* the same look on every surface of the shell, as Windows' transparency
 * setting reaches the Start menu, the flyouts and the toasts: each calls
 * this when it shows. This one keeps them white on black. */
void WINAPI ArcticApplyShellLook( HWND hwnd )
{
    ArcticApplyTaskbarLook( hwnd );
}

/* ... for the surfaces that draw dark on white over a light look: TRUE when
 * they have to */
BOOL WINAPI ArcticApplyShellLookEx( HWND hwnd )
{
    return ArcticApplyTaskbarLookEx( hwnd );
}

/* whether the shell's surfaces draw dark on white now */
BOOL WINAPI ArcticShellIsLight(void)
{
    struct look look;

    look_load_taskbar( &look );
    return look_is_light( &look );
}

/* for what draws its own backdrop (the toasts): the effect (ARCTIC_ACCENT_*),
 * the tint as RGB and its alpha 0-255 */
BOOL WINAPI ArcticGetShellLook( DWORD *effect, DWORD *rgb, DWORD *alpha )
{
    struct look look;

    look_load_taskbar( &look );
    if (effect) *effect = look.effect;
    if (rgb) *rgb = look.color & 0xffffff;
    if (alpha) *alpha = look.effect == ARCTIC_ACCENT_ENABLE_GRADIENT ? 255 : look.opacity * 255 / 100;
    return TRUE;
}

/**********************************************************************
 *          The looks of the Start menus
 */

static const WCHAR *style_key_names[STYLE_COUNT] = { L"Standard", L"Classic", L"Windows10", L"Windows11" };

static HKEY open_style_key( int style, REGSAM access, BOOL create )
{
    WCHAR name[128];
    HKEY key = NULL;

    swprintf( name, ARRAY_SIZE(name), START_KEY L"\\Look\\%s", style_key_names[style] );
    if (create) RegCreateKeyExW( HKEY_CURRENT_USER, name, 0, NULL, 0, access, NULL, &key, NULL );
    else RegOpenKeyExW( HKEY_CURRENT_USER, name, 0, access, &key );
    return key;
}

/* what a style shows without the taskbar's look */
void start_look_defaults( int style, struct look *look )
{
    look->preset = PRESET_CUSTOM;
    look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    look->blur = LOOK_BLUR_DEFAULT;
    look->text = TEXT_AUTO;
    switch (style)
    {
    case STYLE_WIN10:
        look->color = RGB( 0x1f, 0x1f, 0x1f );
        look->opacity = 80;
        break;
    case STYLE_WIN11:
        look->color = RGB( 0x20, 0x20, 0x20 );
        look->opacity = 85;
        break;
    default:
        /* the acrylic the standard menu had before it followed the taskbar */
        look->color = RGB( 0x20, 0x20, 0x20 );
        look->opacity = 80;
        break;
    }
}

BOOL start_look_inherits( int style )
{
    HKEY key = open_style_key( style, KEY_READ, FALSE );
    BOOL inherit = !!read_dword( key, L"Inherit", TRUE );

    if (key) RegCloseKey( key );
    return inherit;
}

/* the look a style has set for it, inherited or not */
void start_look_load_own( int style, struct look *look )
{
    HKEY key;

    start_look_defaults( style, look );
    if (style != STYLE_WIN10 && style != STYLE_WIN11) return;
    if (!(key = open_style_key( style, KEY_READ, FALSE ))) return;
    read_look_values( key, look );
    RegCloseKey( key );
}

/* the look a style shows: the taskbar's, or its own */
void start_look_load( int style, struct look *look )
{
    if (start_look_inherits( style )) look_load_taskbar( look );
    else start_look_load_own( style, look );
}

void start_look_save( int style, BOOL inherit, const struct look *own )
{
    HKEY key = open_style_key( style, KEY_WRITE, TRUE );

    if (!key) return;
    write_dword( key, L"Inherit", inherit );
    if (own && (style == STYLE_WIN10 || style == STYLE_WIN11)) write_look_values( key, own );
    RegCloseKey( key );
}

/* For the classic menu of explorer: TRUE when it takes the taskbar's look,
 * which it then has on the window; *light says it draws dark on white */
BOOL WINAPI ArcticApplyClassicMenuLook( HWND hwnd, BOOL *light )
{
    struct look look;

    if (!start_look_inherits( STYLE_CLASSIC )) return FALSE;
    look_load_taskbar( &look );
    if (light) *light = look_is_light( &look );
    else look.text = TEXT_WHITE;
    apply_look( hwnd, &look, light && *light );
    return TRUE;
}

/**********************************************************************
 *          The controls of a look, on this page and on the Start menu's
 */

HWND add_dialog_control( HWND dialog, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
                         int height, int id )
{
    RECT rect = { x, y, x + width, y + height };
    HWND hwnd;

    MapDialogRect( dialog, &rect );
    hwnd = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, rect.left, rect.top,
                            rect.right - rect.left, rect.bottom - rect.top, dialog, (HMENU)(INT_PTR)id,
                            startui_instance, NULL );
    SendMessageW( hwnd, WM_SETFONT, SendMessageW( dialog, WM_GETFONT, 0, 0 ), FALSE );
    return hwnd;
}

/* the rows Effect, Colour, Opacity, Blur and Text from y down, the ids
 * from first (look_controls_*); returns the y under them */
int look_controls_create( HWND dialog, int y, int first )
{
    HWND combo;

    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_EFFECT ), SS_CENTERIMAGE, 7, y, 66, 14, first + LC_EFFECT_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 80,
                                first + LC_EFFECT );
    for (UINT i = 0; i < LOOK_EFFECT_COUNT; i++)
        SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( look_effects[i].name ) );
    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_COLOR ), SS_CENTERIMAGE, 7, y, 66, 14, first + LC_COLOR_LABEL );
    add_dialog_control( dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 76, y, 50, 14, first + LC_COLOR );
    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_OPACITY ), SS_CENTERIMAGE, 7, y, 66, 14, first + LC_OPACITY_LABEL );
    add_dialog_control( dialog, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 72, y, 140, 16, first + LC_OPACITY );
    SendDlgItemMessageW( dialog, first + LC_OPACITY, TBM_SETRANGE, FALSE, MAKELONG( 0, 100 ) );
    SendDlgItemMessageW( dialog, first + LC_OPACITY, TBM_SETPAGESIZE, 0, 10 );
    add_dialog_control( dialog, L"STATIC", L"", SS_CENTERIMAGE, 215, y, 30, 14, first + LC_OPACITY_VALUE );
    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_BLUR_AMOUNT ), SS_CENTERIMAGE, 7, y, 66, 14, first + LC_BLUR_LABEL );
    add_dialog_control( dialog, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 72, y, 140, 16, first + LC_BLUR );
    SendDlgItemMessageW( dialog, first + LC_BLUR, TBM_SETRANGE, FALSE, MAKELONG( 1, 100 ) );
    SendDlgItemMessageW( dialog, first + LC_BLUR, TBM_SETPAGESIZE, 0, 10 );
    add_dialog_control( dialog, L"STATIC", L"", SS_CENTERIMAGE, 215, y, 30, 14, first + LC_BLUR_VALUE );
    y += 20;
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_TEXT ), SS_CENTERIMAGE, 7, y, 66, 14, first + LC_TEXT_LABEL );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 60,
                                first + LC_TEXT );
    for (UINT i = 0; i < TEXT_COUNT; i++) SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( look_text_names[i] ) );
    return y + 20;
}

void look_controls_show( HWND dialog, int first, const struct look *look, BOOL enabled )
{
    BOOL blurs = look->effect == ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND || look->effect == ARCTIC_ACCENT_ENABLE_BLURBEHIND;
    WCHAR text[16];

    for (UINT i = 0; i < LOOK_EFFECT_COUNT; i++)
        if (look_effects[i].effect == look->effect) SendDlgItemMessageW( dialog, first + LC_EFFECT, CB_SETCURSEL, i, 0 );
    SendDlgItemMessageW( dialog, first + LC_OPACITY, TBM_SETPOS, TRUE, look->opacity );
    swprintf( text, ARRAY_SIZE(text), L"%lu%%", look->opacity );
    SetDlgItemTextW( dialog, first + LC_OPACITY_VALUE, text );
    SendDlgItemMessageW( dialog, first + LC_BLUR, TBM_SETPOS, TRUE, look->blur );
    swprintf( text, ARRAY_SIZE(text), L"%lu", look->blur );
    SetDlgItemTextW( dialog, first + LC_BLUR_VALUE, text );
    SendDlgItemMessageW( dialog, first + LC_TEXT, CB_SETCURSEL, look->text, 0 );
    for (int id = 0; id < LC_COUNT; id++) EnableWindow( GetDlgItem( dialog, first + id ), enabled );
    if (enabled)
    {
        EnableWindow( GetDlgItem( dialog, first + LC_OPACITY ), look->effect != ARCTIC_ACCENT_ENABLE_GRADIENT );
        EnableWindow( GetDlgItem( dialog, first + LC_OPACITY_VALUE ), look->effect != ARCTIC_ACCENT_ENABLE_GRADIENT );
        EnableWindow( GetDlgItem( dialog, first + LC_BLUR ), blurs );
        EnableWindow( GetDlgItem( dialog, first + LC_BLUR_VALUE ), blurs );
    }
    InvalidateRect( GetDlgItem( dialog, first + LC_COLOR ), NULL, TRUE );
}

/* a WM_COMMAND or WM_HSCROLL of the controls changed the look: TRUE */
BOOL look_controls_command( HWND dialog, int first, struct look *look, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_HSCROLL)
    {
        if ((HWND)lparam == GetDlgItem( dialog, first + LC_OPACITY ))
        {
            look->opacity = SendDlgItemMessageW( dialog, first + LC_OPACITY, TBM_GETPOS, 0, 0 );
            return TRUE;
        }
        if ((HWND)lparam == GetDlgItem( dialog, first + LC_BLUR ))
        {
            look->blur = SendDlgItemMessageW( dialog, first + LC_BLUR, TBM_GETPOS, 0, 0 );
            return TRUE;
        }
        return FALSE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD( wparam ) == first + LC_EFFECT && HIWORD( wparam ) == CBN_SELCHANGE)
    {
        LRESULT sel = SendDlgItemMessageW( dialog, first + LC_EFFECT, CB_GETCURSEL, 0, 0 );
        if (sel >= 0 && sel < LOOK_EFFECT_COUNT) look->effect = look_effects[sel].effect;
        return TRUE;
    }
    if (LOWORD( wparam ) == first + LC_TEXT && HIWORD( wparam ) == CBN_SELCHANGE)
    {
        LRESULT sel = SendDlgItemMessageW( dialog, first + LC_TEXT, CB_GETCURSEL, 0, 0 );
        if (sel >= 0 && sel < TEXT_COUNT) look->text = sel;
        return TRUE;
    }
    if (LOWORD( wparam ) == first + LC_COLOR)
    {
        static COLORREF custom[16];
        CHOOSECOLORW cc = { sizeof(cc) };

        cc.hwndOwner = dialog;
        cc.rgbResult = look->color;
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW( &cc )) return FALSE;
        look->color = cc.rgbResult;
        return TRUE;
    }
    return FALSE;
}

/* the colour button's swatch */
BOOL look_controls_draw( int first, const struct look *look, WPARAM wparam, LPARAM lparam )
{
    DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lparam;
    RECT rect;
    HBRUSH brush;

    if (wparam != (WPARAM)(first + LC_COLOR)) return FALSE;
    rect = item->rcItem;
    DrawEdge( item->hDC, &rect, (item->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_ADJUST );
    InflateRect( &rect, -2, -2 );
    brush = CreateSolidBrush( (item->itemState & ODS_DISABLED) ? GetSysColor( COLOR_BTNFACE ) : look->color );
    FillRect( item->hDC, &rect, brush );
    DeleteObject( brush );
    if (item->itemState & ODS_FOCUS)
    {
        InflateRect( &rect, 1, 1 );
        DrawFocusRect( item->hDC, &rect );
    }
    return TRUE;
}

/**********************************************************************
 *          The page
 */

#define ID_LOOK_FIRST 1300

struct page
{
    HWND tray;
    struct look look;
    BOOL filling;
};

/* the look on the page while it is open, applied or not, for the Start
 * menu's page beside it */
static struct page *open_page;

void taskbar_page_look( struct look *look )
{
    if (open_page) *look = open_page->look;
    else look_load_taskbar( look );
}

static void show_look( HWND dialog, struct page *page )
{
    page->filling = TRUE;
    SendDlgItemMessageW( dialog, ID_PRESET, CB_SETCURSEL, page->look.preset, 0 );
    look_controls_show( dialog, ID_LOOK_FIRST, &page->look, TRUE );
    page->filling = FALSE;
}

/* a change of effect, colour, opacity or blur makes the look Custom */
static void changed( HWND dialog, struct page *page, BOOL to_custom )
{
    if (page->filling) return;
    if (to_custom) page->look.preset = PRESET_CUSTOM;
    show_look( dialog, page );
    SendMessageW( GetParent( dialog ), PSM_CHANGED, (WPARAM)dialog, 0 );
    /* as Windows' personalization does, the taskbar shows it at once */
    if (page->tray)
    {
        apply_taskbar( page->tray, &page->look );
        /* its icons too */
        PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
    }
}

static void init_page( HWND dialog, struct page *page )
{
    HWND combo;

    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_INTRO ), 0, 7, 7, 238, 20, -1 );
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_PRESET ), SS_CENTERIMAGE, 7, 30, 66, 14, -1 );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, 30, 169, 120,
                                ID_PRESET );
    for (int i = 0; i < PRESET_COUNT; i++) SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( preset_names[i] ) );
    look_controls_create( dialog, 52, ID_LOOK_FIRST );
    add_dialog_control( dialog, L"STATIC", load_string( IDS_LOOK_NOTE ), 0, 7, 160, 238, 40, -1 );
    look_load_taskbar( &page->look );
    open_page = page;
    show_look( dialog, page );
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
        if (!page) break;
        if (LOWORD( wparam ) == ID_PRESET)
        {
            if (HIWORD( wparam ) != CBN_SELCHANGE) break;
            page->look.preset = SendDlgItemMessageW( dialog, ID_PRESET, CB_GETCURSEL, 0, 0 );
            if (page->look.preset != PRESET_CUSTOM)
            {
                DWORD text = page->look.text;
                preset_look( &page->look );
                page->look.text = text;
            }
            changed( dialog, page, FALSE );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_LOOK_FIRST + LC_TEXT)
        {
            /* the text goes with any preset */
            if (look_controls_command( dialog, ID_LOOK_FIRST, &page->look, msg, wparam, lparam )) changed( dialog, page, FALSE );
            return TRUE;
        }
        if (look_controls_command( dialog, ID_LOOK_FIRST, &page->look, msg, wparam, lparam ))
        {
            changed( dialog, page, TRUE );
            return TRUE;
        }
        break;

    case WM_HSCROLL:
        if (page && look_controls_command( dialog, ID_LOOK_FIRST, &page->look, msg, wparam, lparam ))
            changed( dialog, page, TRUE );
        return TRUE;

    case WM_DRAWITEM:
        if (page && look_controls_draw( ID_LOOK_FIRST, &page->look, wparam, lparam )) return TRUE;
        break;

    case WM_NOTIFY:
        if (page && ((NMHDR *)lparam)->code == PSN_APPLY)
        {
            save_taskbar_look( &page->look );
            if (page->tray) ArcticApplyTaskbarLook( page->tray );
            /* the flyouts take it when they show next; the open ones hear it
             * now, and the taskbar draws its text anew */
            PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        if (page && ((NMHDR *)lparam)->code == PSN_RESET && page->tray)
        {
            /* Cancel: back to what was saved */
            ArcticApplyTaskbarLook( page->tray );
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
        /* whatever was tried on the page, the taskbar keeps what is saved */
        if (page->tray) PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
        if (open_page == page) open_page = NULL;
        free( page );
    }
    return 1;
}

/* an empty page the size of explorer's own (DLUs); its controls are made at run time */
const struct page_template page_template =
{
    { DS_SETFONT | WS_CHILD | WS_CAPTION | DS_3DLOOK | DS_CONTROL, 0, 0, 0, 0, 252, 218 },
    0, 0, 0, 8, L"MS Shell Dlg",
};

/* the page "Оформлення" of the taskbar's properties, for explorer to add */
HPROPSHEETPAGE WINAPI ArcticTaskbarLookPage( HWND tray )
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return NULL;
    page->tray = tray;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = startui_instance;
    psp.pResource = &page_template.header;
    psp.pfnDlgProc = page_proc;
    psp.pszTitle = load_string( IDS_LOOK_TITLE );
    psp.lParam = (LPARAM)page;
    psp.pfnCallback = page_callback;
    return CreatePropertySheetPageW( &psp );
}
