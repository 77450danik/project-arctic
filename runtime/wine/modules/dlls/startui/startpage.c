/*
 * The page "Меню «Пуск»" of the taskbar's properties
 *
 * Which Start menu the Start button opens: the standard one of two columns
 * (menu.c), the menus of Windows 10 (menu10.c) and 11 (menu11.c), or
 * explorer's classic one; "Налаштувати..." opens what each has to set.
 * Each style takes the look of the taskbar's page "Оформлення", as it is
 * on that page even before it is applied, or, with "Inherit" off, its own:
 * the standard menu and the classic one have theirs, the menus of Windows
 * 10 and 11 the effect, colour, opacity, blur and text chosen here.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "startui.h"
#include "commctrl.h"
#include "shlobj.h"

#define ID_STYLE_FIRST  1400   /* + enum start_style */
#define ID_CUSTOMIZE    1410
#define ID_INHERIT      1411
#define ID_LOOK_FIRST   1420

/* the radio buttons top to bottom */
static const int style_order[STYLE_COUNT] = { STYLE_STANDARD, STYLE_WIN10, STYLE_WIN11, STYLE_CLASSIC };
static const UINT style_names[STYLE_COUNT] =
{
    [STYLE_STANDARD] = IDS_STYLE_STANDARD, [STYLE_CLASSIC] = IDS_STYLE_CLASSIC,
    [STYLE_WIN10] = IDS_STYLE_WIN10, [STYLE_WIN11] = IDS_STYLE_WIN11,
};

struct page
{
    HWND tray;
    void (WINAPI *classic_customize)( HWND );
    int style;
    BOOL inherit[STYLE_COUNT];
    struct look own[STYLE_COUNT];
    BOOL filling;
};

static BOOL has_own_look( int style )
{
    return style == STYLE_WIN10 || style == STYLE_WIN11;
}

static void show_page( HWND dialog, struct page *page )
{
    BOOL own = has_own_look( page->style ), inherit = page->inherit[page->style];
    struct look look;

    page->filling = TRUE;
    for (int i = 0; i < STYLE_COUNT; i++)
        CheckDlgButton( dialog, ID_STYLE_FIRST + i, i == page->style ? BST_CHECKED : BST_UNCHECKED );
    CheckDlgButton( dialog, ID_INHERIT, inherit ? BST_CHECKED : BST_UNCHECKED );
    /* inherited: what the taskbar's page shows now */
    if (inherit) taskbar_page_look( &look );
    else look = page->own[page->style];
    look_controls_show( dialog, ID_LOOK_FIRST, &look, own && !inherit );
    for (int id = 0; id < LC_COUNT; id++) ShowWindow( GetDlgItem( dialog, ID_LOOK_FIRST + id ), own ? SW_SHOW : SW_HIDE );
    page->filling = FALSE;
}

static void changed( HWND dialog, struct page *page )
{
    if (page->filling) return;
    show_page( dialog, page );
    SendMessageW( GetParent( dialog ), PSM_CHANGED, (WPARAM)dialog, 0 );
}

static void init_page( HWND dialog, struct page *page )
{
    int y = 7;

    for (int i = 0; i < STYLE_COUNT; i++, y += 14)
    {
        int style = style_order[i];
        add_dialog_control( dialog, L"BUTTON", load_string( style_names[style] ),
                            BS_AUTORADIOBUTTON | WS_TABSTOP | (i ? 0 : WS_GROUP), 7, y, 170, 10, ID_STYLE_FIRST + style );
    }
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_CUSTOMIZE ), BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 180, 5,
                        65, 14, ID_CUSTOMIZE );
    y += 8;
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_INHERIT ), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 7, y,
                        238, 10, ID_INHERIT );
    look_controls_create( dialog, y + 18, ID_LOOK_FIRST );

    page->style = start_style_load();
    for (int i = 0; i < STYLE_COUNT; i++)
    {
        page->inherit[i] = start_look_inherits( i );
        start_look_load_own( i, &page->own[i] );
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
        if (LOWORD( wparam ) >= ID_STYLE_FIRST && LOWORD( wparam ) < ID_STYLE_FIRST + STYLE_COUNT)
        {
            if (HIWORD( wparam ) != BN_CLICKED) break;
            page->style = LOWORD( wparam ) - ID_STYLE_FIRST;
            changed( dialog, page );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_INHERIT)
        {
            page->inherit[page->style] = IsDlgButtonChecked( dialog, ID_INHERIT ) == BST_CHECKED;
            changed( dialog, page );
            return TRUE;
        }
        if (LOWORD( wparam ) == ID_CUSTOMIZE)
        {
            switch (page->style)
            {
            case STYLE_STANDARD: customize_dialog( dialog ); break;
            case STYLE_CLASSIC: if (page->classic_customize) page->classic_customize( dialog ); break;
            default: modern_customize_dialog( dialog, page->style ); break;
            }
            return TRUE;
        }
        if (has_own_look( page->style ) && !page->inherit[page->style] &&
            look_controls_command( dialog, ID_LOOK_FIRST, &page->own[page->style], msg, wparam, lparam ))
        {
            changed( dialog, page );
            return TRUE;
        }
        break;

    case WM_HSCROLL:
        if (page && has_own_look( page->style ) && !page->inherit[page->style] &&
            look_controls_command( dialog, ID_LOOK_FIRST, &page->own[page->style], msg, wparam, lparam ))
            changed( dialog, page );
        return TRUE;

    case WM_DRAWITEM:
        if (page)
        {
            struct look look;

            if (page->inherit[page->style]) taskbar_page_look( &look );
            else look = page->own[page->style];
            if (look_controls_draw( ID_LOOK_FIRST, &look, wparam, lparam )) return TRUE;
        }
        break;

    case WM_NOTIFY:
        if (!page) break;
        switch (((NMHDR *)lparam)->code)
        {
        case PSN_SETACTIVE:
            /* what the page Оформлення has now */
            show_page( dialog, page );
            break;
        case PSN_APPLY:
        {
            void (WINAPI *set_settings)( SHELLSTATE *, DWORD, BOOL );
            SHELLSTATE state = { 0 };

            for (int i = 0; i < STYLE_COUNT; i++) start_look_save( i, page->inherit[i], &page->own[i] );
            start_style_save( page->style );
            /* SHGetSetSettings, exported by ordinal only */
            set_settings = (void *)GetProcAddress( GetModuleHandleW( L"shell32.dll" ), (const char *)68 );
            state.fStartPanelOn = page->style != STYLE_CLASSIC;
            if (set_settings) set_settings( &state, 0x00400000 /* SSF_STARTPANELON */, TRUE );
            PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

static UINT CALLBACK page_callback( HWND hwnd, UINT msg, PROPSHEETPAGEW *psp )
{
    if (msg == PSPCB_RELEASE) free( (struct page *)psp->lParam );
    return 1;
}

/* the page "Меню «Пуск»", for explorer to add in place of its own; the
 * classic menu's Customize is explorer's */
HPROPSHEETPAGE WINAPI ArcticStartMenuPage( HWND tray, void (WINAPI *classic_customize)( HWND ) )
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return NULL;
    page->tray = tray;
    page->classic_customize = classic_customize;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = startui_instance;
    psp.pResource = &page_template.header;
    psp.pfnDlgProc = page_proc;
    psp.pszTitle = load_string( IDS_START_TITLE );
    psp.lParam = (LPARAM)page;
    psp.pfnCallback = page_callback;
    return CreatePropertySheetPageW( &psp );
}
