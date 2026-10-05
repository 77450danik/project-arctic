/*
 * The look of the taskbar, and its page in the taskbar's properties
 *
 * What lies behind the taskbar shows through as dwm.exe composes it for
 * SetWindowCompositionAttribute(WCA_ACCENT_POLICY): acrylic (blurred,
 * tinted, grained), a blur without the grain, a see-through tint without
 * blur, or a solid colour. The presets are the taskbars of Windows 10 (the
 * default: acrylic, #101010 at 75%) and Windows 11 (#1C1C1C at 85%), the
 * accent colour, a clear blur, a glassy see-through and a solid one, and
 * Custom: any effect, colour and opacity. They live in
 * HKCU\Software\Arctic\Taskbar (Preset, Effect, Color, Opacity) and the
 * page "Оформлення" of the taskbar's properties chooses them.
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
#include "commdlg.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define REG_KEY       L"Software\\Arctic\\Taskbar"

#define ID_PRESET     1200
#define ID_EFFECT     1201
#define ID_COLOR      1202
#define ID_OPACITY    1203
#define ID_PERCENT    1204

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum preset { PRESET_WIN10, PRESET_WIN11, PRESET_ACCENT, PRESET_BLUR, PRESET_CLEAR, PRESET_SOLID, PRESET_CUSTOM, PRESET_COUNT };

struct look
{
    DWORD preset;
    DWORD effect;     /* ARCTIC_ACCENT_* */
    COLORREF color;
    DWORD opacity;    /* percent */
};

static const UINT preset_names[PRESET_COUNT] =
{
    IDS_LOOK_WIN10, IDS_LOOK_WIN11, IDS_LOOK_ACCENT, IDS_LOOK_BLUR, IDS_LOOK_CLEAR, IDS_LOOK_SOLID, IDS_LOOK_CUSTOM,
};

static const struct { DWORD effect; UINT name; } effects[] =
{
    { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND,   IDS_EFFECT_ACRYLIC },
    { ARCTIC_ACCENT_ENABLE_BLURBEHIND,          IDS_EFFECT_BLUR },
    { ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT, IDS_EFFECT_CLEAR },
    { ARCTIC_ACCENT_ENABLE_GRADIENT,            IDS_EFFECT_SOLID },
};

static COLORREF accent_color(void)
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

static void load_look( struct look *look )
{
    HKEY key = NULL;

    RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key );
    look->preset = read_dword( key, L"Preset", PRESET_WIN10 );
    if (look->preset >= PRESET_COUNT) look->preset = PRESET_WIN10;
    if (look->preset == PRESET_CUSTOM)
    {
        look->effect = read_dword( key, L"Effect", ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND );
        if (look->effect < ARCTIC_ACCENT_ENABLE_GRADIENT || look->effect > ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND)
            look->effect = ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND;
        look->color = read_dword( key, L"Color", RGB( 0x10, 0x10, 0x10 ) ) & 0xffffff;
        look->opacity = min( read_dword( key, L"Opacity", 75 ), 100 );
    }
    else preset_look( look );
    if (key) RegCloseKey( key );
}

static void save_look( const struct look *look )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"Preset", 0, REG_DWORD, (const BYTE *)&look->preset, sizeof(DWORD) );
    RegSetValueExW( key, L"Effect", 0, REG_DWORD, (const BYTE *)&look->effect, sizeof(DWORD) );
    RegSetValueExW( key, L"Color", 0, REG_DWORD, (const BYTE *)&look->color, sizeof(DWORD) );
    RegSetValueExW( key, L"Opacity", 0, REG_DWORD, (const BYTE *)&look->opacity, sizeof(DWORD) );
    RegCloseKey( key );
}

static void apply_look( HWND tray, const struct look *look )
{
    DWORD alpha = look->effect == ARCTIC_ACCENT_ENABLE_GRADIENT ? 255 : look->opacity * 255 / 100;
    struct arctic_accent_policy policy = { look->effect, 0, 0, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };

    /* 0xAABBGGRR */
    policy.gradient_color = (alpha << 24) | (GetBValue( look->color ) << 16) | (GetGValue( look->color ) << 8) |
                            GetRValue( look->color );
    SetWindowCompositionAttribute( tray, &attr );
    RedrawWindow( tray, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN );
}

/* the taskbar takes the look chosen: when it is made, and when the page applies */
void WINAPI ArcticApplyTaskbarLook( HWND tray )
{
    struct look look;

    load_look( &look );
    apply_look( tray, &look );
}

/**********************************************************************
 *          The page
 */

struct page
{
    HWND tray;
    struct look look;
    HBRUSH swatch;
    BOOL filling;
};

static HWND add_control( HWND dialog, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
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

static void show_look( HWND dialog, struct page *page )
{
    WCHAR text[16];

    page->filling = TRUE;
    SendDlgItemMessageW( dialog, ID_PRESET, CB_SETCURSEL, page->look.preset, 0 );
    for (UINT i = 0; i < ARRAY_SIZE(effects); i++)
        if (effects[i].effect == page->look.effect) SendDlgItemMessageW( dialog, ID_EFFECT, CB_SETCURSEL, i, 0 );
    SendDlgItemMessageW( dialog, ID_OPACITY, TBM_SETPOS, TRUE, page->look.opacity );
    swprintf( text, ARRAY_SIZE(text), L"%lu%%", page->look.opacity );
    SetDlgItemTextW( dialog, ID_PERCENT, text );
    EnableWindow( GetDlgItem( dialog, ID_OPACITY ), page->look.effect != ARCTIC_ACCENT_ENABLE_GRADIENT );
    if (page->swatch) DeleteObject( page->swatch );
    page->swatch = CreateSolidBrush( page->look.color );
    InvalidateRect( GetDlgItem( dialog, ID_COLOR ), NULL, TRUE );
    page->filling = FALSE;
}

/* a change of effect, colour or opacity makes the look Custom */
static void changed( HWND dialog, struct page *page, BOOL to_custom )
{
    if (page->filling) return;
    if (to_custom) page->look.preset = PRESET_CUSTOM;
    show_look( dialog, page );
    SendMessageW( GetParent( dialog ), PSM_CHANGED, (WPARAM)dialog, 0 );
    /* as Windows' personalization does, the taskbar shows it at once */
    if (page->tray) apply_look( page->tray, &page->look );
}

static void init_page( HWND dialog, struct page *page )
{
    HWND combo;

    add_control( dialog, L"STATIC", load_string( IDS_LOOK_INTRO ), 0, 7, 7, 238, 20, -1 );
    add_control( dialog, L"STATIC", load_string( IDS_LOOK_PRESET ), SS_CENTERIMAGE, 7, 30, 66, 14, -1 );
    combo = add_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, 30, 169, 120,
                         ID_PRESET );
    for (int i = 0; i < PRESET_COUNT; i++) SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( preset_names[i] ) );
    add_control( dialog, L"STATIC", load_string( IDS_LOOK_EFFECT ), SS_CENTERIMAGE, 7, 52, 66, 14, -1 );
    combo = add_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, 52, 169, 80,
                         ID_EFFECT );
    for (UINT i = 0; i < ARRAY_SIZE(effects); i++)
        SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( effects[i].name ) );
    add_control( dialog, L"STATIC", load_string( IDS_LOOK_COLOR ), SS_CENTERIMAGE, 7, 74, 66, 14, -1 );
    add_control( dialog, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 76, 74, 50, 14, ID_COLOR );
    add_control( dialog, L"STATIC", load_string( IDS_LOOK_OPACITY ), SS_CENTERIMAGE, 7, 96, 66, 14, -1 );
    add_control( dialog, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 72, 96, 140, 16, ID_OPACITY );
    SendDlgItemMessageW( dialog, ID_OPACITY, TBM_SETRANGE, FALSE, MAKELONG( 0, 100 ) );
    SendDlgItemMessageW( dialog, ID_OPACITY, TBM_SETPAGESIZE, 0, 10 );
    add_control( dialog, L"STATIC", L"", SS_CENTERIMAGE, 215, 96, 30, 14, ID_PERCENT );
    add_control( dialog, L"STATIC", load_string( IDS_LOOK_NOTE ), 0, 7, 122, 238, 40, -1 );
    load_look( &page->look );
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
        switch (LOWORD( wparam ))
        {
        case ID_PRESET:
            if (HIWORD( wparam ) != CBN_SELCHANGE) break;
            page->look.preset = SendDlgItemMessageW( dialog, ID_PRESET, CB_GETCURSEL, 0, 0 );
            if (page->look.preset != PRESET_CUSTOM) preset_look( &page->look );
            changed( dialog, page, FALSE );
            return TRUE;
        case ID_EFFECT:
            if (HIWORD( wparam ) != CBN_SELCHANGE) break;
            {
                LRESULT sel = SendDlgItemMessageW( dialog, ID_EFFECT, CB_GETCURSEL, 0, 0 );
                if (sel >= 0 && sel < (LRESULT)ARRAY_SIZE(effects)) page->look.effect = effects[sel].effect;
            }
            changed( dialog, page, TRUE );
            return TRUE;
        case ID_COLOR:
        {
            static COLORREF custom[16];
            CHOOSECOLORW cc = { sizeof(cc) };

            cc.hwndOwner = dialog;
            cc.rgbResult = page->look.color;
            cc.lpCustColors = custom;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW( &cc ))
            {
                page->look.color = cc.rgbResult;
                changed( dialog, page, TRUE );
            }
            return TRUE;
        }
        }
        break;

    case WM_HSCROLL:
        if (page && (HWND)lparam == GetDlgItem( dialog, ID_OPACITY ))
        {
            page->look.opacity = SendDlgItemMessageW( dialog, ID_OPACITY, TBM_GETPOS, 0, 0 );
            changed( dialog, page, TRUE );
        }
        return TRUE;

    case WM_DRAWITEM:
        if (page && wparam == ID_COLOR)
        {
            DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lparam;
            RECT rect = item->rcItem;

            DrawEdge( item->hDC, &rect, (item->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED,
                      BF_RECT | BF_ADJUST );
            InflateRect( &rect, -2, -2 );
            FillRect( item->hDC, &rect, page->swatch );
            if (item->itemState & ODS_FOCUS)
            {
                InflateRect( &rect, 1, 1 );
                DrawFocusRect( item->hDC, &rect );
            }
            return TRUE;
        }
        break;

    case WM_NOTIFY:
        if (page && ((NMHDR *)lparam)->code == PSN_APPLY)
        {
            save_look( &page->look );
            if (page->tray) ArcticApplyTaskbarLook( page->tray );
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
        if (page->tray) ArcticApplyTaskbarLook( page->tray );
        if (page->swatch) DeleteObject( page->swatch );
        free( page );
    }
    return 1;
}

/* the page "Оформлення" of the taskbar's properties, for explorer to add */
HPROPSHEETPAGE WINAPI ArcticTaskbarLookPage( HWND tray )
{
    /* an empty page the size of explorer's own (DLUs); its controls are made at run time */
    static const struct
    {
        DLGTEMPLATE header;
        WORD menu, cls, title;
        WORD point_size;
        WCHAR face[13];
    } DECLSPEC_ALIGN(4) template =
    {
        { DS_SETFONT | WS_CHILD | WS_CAPTION | DS_3DLOOK | DS_CONTROL, 0, 0, 0, 0, 252, 218 },
        0, 0, 0, 8, L"MS Shell Dlg",
    };
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return NULL;
    page->tray = tray;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = startui_instance;
    psp.pResource = &template.header;
    psp.pfnDlgProc = page_proc;
    psp.pszTitle = load_string( IDS_LOOK_TITLE );
    psp.lParam = (LPARAM)page;
    psp.pfnCallback = page_callback;
    return CreatePropertySheetPageW( &psp );
}
