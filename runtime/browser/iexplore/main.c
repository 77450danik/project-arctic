/*
 * Internet Explorer
 *
 * The browser's frame: the window, the address bar and the status bar. The
 * page itself belongs to our own engine, arctichtml.dll, which makes its
 * own window inside this one and draws, through Vulkan when the card
 * allows it (docs/browser.md).
 *
 * The window classes are the ones Windows uses - IEFrame here and
 * "Internet Explorer_Server" in the engine - so that what looks for the
 * browser's windows finds them where it is used to. Like the engine it is a
 * plain Windows program: it runs on any Windows 10 or 11 as in Arctic.
 *
 * Stage 0 of docs/browser.md: the frame is what the rest is built on, so it
 * carries only what the stage needs. Back and forward are there but idle
 * until stage 3 brings the travel log, and the tabs, the favourites and the
 * gear come with the stages that have something to put in them.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include <windows.h>
#include <commctrl.h>

#include "arctic_html.h"

#include "resource.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static HINSTANCE instance;
static ARCTIC_HTML_VIEW view;
static HWND frame, toolbar, address, go, status;
static HFONT shell_font;
static UINT dpi = 96;

static int scale( int value )
{
    return MulDiv( value, dpi, 96 );
}

static const WCHAR *load_string( UINT id )
{
    static WCHAR buffers[4][256];
    static unsigned int next;
    WCHAR *buffer = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( instance, id, buffer, 256 )) *buffer = 0;
    return buffer;
}

static void set_shell_font(void)
{
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    HFONT font;

    if (!SystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi ))
        return;
    if (!(font = CreateFontIndirectW( &metrics.lfMessageFont ))) return;

    if (shell_font) DeleteObject( shell_font );
    shell_font = font;
    SendMessageW( address, WM_SETFONT, (WPARAM)font, TRUE );
    SendMessageW( go, WM_SETFONT, (WPARAM)font, TRUE );
    SendMessageW( status, WM_SETFONT, (WPARAM)font, TRUE );
}

static void show_status(void)
{
    int parts[2];
    RECT rect;

    GetClientRect( status, &rect );
    parts[0] = rect.right - scale( 140 );
    parts[1] = -1;
    if (parts[0] < scale( 80 )) parts[0] = rect.right / 2;
    SendMessageW( status, SB_SETPARTS, ARRAY_SIZE(parts), (LPARAM)parts );
    SendMessageW( status, SB_SETTEXTW, 0, (LPARAM)load_string( IDS_READY ) );
    SendMessageW( status, SB_SETTEXTW, 1,
                  (LPARAM)load_string( ArcticHtmlOnGpu( view ) ? IDS_ON_VULKAN : IDS_ON_GDI ) );
}

static void layout(void)
{
    int margin = scale( 4 ), row = scale( 30 ), buttons = scale( 62 ), go_width = scale( 84 );
    RECT client, bar, content;

    GetClientRect( frame, &client );

    SendMessageW( status, WM_SIZE, 0, 0 );
    GetWindowRect( status, &bar );

    SetWindowPos( toolbar, 0, margin, margin, buttons, row, SWP_NOZORDER | SWP_NOACTIVATE );
    SetWindowPos( address, 0, margin * 2 + buttons, margin,
                  max( client.right - (margin * 4 + buttons + go_width), scale( 60 ) ), row,
                  SWP_NOZORDER | SWP_NOACTIVATE );
    SetWindowPos( go, 0, client.right - margin - go_width, margin, go_width, row,
                  SWP_NOZORDER | SWP_NOACTIVATE );

    content.left = 0;
    content.top = row + margin * 2;
    content.right = client.right;
    content.bottom = max( client.bottom - (bar.bottom - bar.top), content.top );
    ArcticHtmlSetRect( view, &content );

    show_status();
}

/* "about:arctic" and "https://..." name their scheme, "example.com" does
 * not: a colon before any dot or slash is what tells them apart. */
static BOOL has_scheme( const WCHAR *text )
{
    for (const WCHAR *p = text; *p; p++)
    {
        if (*p == ':') return TRUE;
        if (*p == '.' || *p == '/' || *p == ' ') break;
    }
    return FALSE;
}

/* What was typed is an address when it says so, and stage 0 shows whatever
 * it is told either way; the search engine and the real guesses come with
 * the navigation of stage 2. */
static void navigate(void)
{
    WCHAR typed[2084], url[2084];

    if (!GetWindowTextW( address, typed, ARRAY_SIZE(typed) ) || !*typed) return;

    if (has_scheme( typed ))
        lstrcpynW( url, typed, ARRAY_SIZE(url) );
    else
    {
        lstrcpynW( url, L"https://", ARRAY_SIZE(url) );
        lstrcatW( url, typed );
    }
    SetWindowTextW( address, url );
    ArcticHtmlNavigate( view, url );
    SetFocus( ArcticHtmlGetWindow( view ) );
}

/* an EDIT does not report Enter to its parent, so the frame asks for it */
static LRESULT CALLBACK address_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR data )
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN)
    {
        navigate();
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;   /* no beep */
    return DefSubclassProc( hwnd, msg, wp, lp );
}

static BOOL create_children( HWND hwnd )
{
    TBBUTTON buttons[] =
    {
        /* idle until the travel log of stage 3 */
        { HIST_BACK,    IDM_BACK,    0, BTNS_BUTTON },
        { HIST_FORWARD, IDM_FORWARD, 0, BTNS_BUTTON },
    };
    RECT content = { 0, 0, 0, 0 };

    toolbar = CreateWindowExW( 0, TOOLBARCLASSNAMEW, NULL,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                               CCS_NODIVIDER | CCS_NORESIZE | CCS_NOPARENTALIGN,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_TOOLBAR, instance, NULL );
    address = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", NULL,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_ADDRESS, instance, NULL );
    go = CreateWindowExW( 0, L"BUTTON", load_string( IDS_GO ),
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          0, 0, 0, 0, hwnd, (HMENU)IDC_GO, instance, NULL );
    status = CreateWindowExW( 0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                              0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, instance, NULL );
    if (!toolbar || !address || !go || !status) return FALSE;

    SendMessageW( toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0 );
    SendMessageW( toolbar, TB_LOADIMAGES, IDB_HIST_SMALL_COLOR, (LPARAM)HINST_COMMCTRL );
    SendMessageW( toolbar, TB_ADDBUTTONSW, ARRAY_SIZE(buttons), (LPARAM)buttons );

    SetWindowSubclass( address, address_proc, 0, 0 );
    SendMessageW( address, EM_SETLIMITTEXT, 2048, 0 );

    if (FAILED(ArcticHtmlCreateView( hwnd, &content, &view )))
    {
        OutputDebugStringA( "iexplore: the engine gives no view\n" );
        return FALSE;
    }
    set_shell_font();
    return TRUE;
}

static LRESULT WINAPI frame_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_CREATE:
        frame = hwnd;
        dpi = GetDpiForWindow( hwnd );
        if (!create_children( hwnd )) return -1;
        return 0;

    case WM_SIZE:
        if (view) layout();
        return 0;

    case WM_DPICHANGED:
    {
        const RECT *where = (const RECT *)lp;

        dpi = LOWORD(wp);
        set_shell_font();
        SetWindowPos( hwnd, 0, where->left, where->top, where->right - where->left,
                      where->bottom - where->top, SWP_NOZORDER | SWP_NOACTIVATE );
        if (view) layout();
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus( address );
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_GO:
        case IDOK:  /* Enter in the address bar, which IsDialogMessage sends here */
            navigate();
            return 0;
        }
        return 0;

    case ARCTIC_HTML_WM_TITLE:
    {
        WCHAR caption[320];

        wsprintfW( caption, load_string( IDS_TITLE_FORMAT ), (const WCHAR *)lp );
        SetWindowTextW( hwnd, caption );
        return 0;
    }
    case WM_DESTROY:
        ArcticHtmlDestroyView( view );
        view = NULL;
        if (shell_font) DeleteObject( shell_font );
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI wWinMain( HINSTANCE instance_handle, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX controls = { .dwSize = sizeof(controls),
                                      .dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSW class = { .lpfnWndProc = frame_proc, .hInstance = instance_handle,
                        .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
                        .lpszClassName = L"IEFrame" };
    MSG msg;

    instance = instance_handle;
    InitCommonControlsEx( &controls );

    if (!RegisterClassW( &class )) return 1;
    if (!CreateWindowExW( 0, class.lpszClassName, load_string( IDS_TITLE ),
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          1024, 720, 0, 0, instance_handle, NULL ))
        return 1;

    while (*cmdline == ' ' || *cmdline == '\t') cmdline++;
    if (*cmdline)
    {
        SetWindowTextW( address, cmdline );
        navigate();
    }
    else
    {
        SetWindowTextW( address, L"about:arctic" );
        navigate();
        SetFocus( address );
        SendMessageW( address, EM_SETSEL, 0, -1 );   /* typing replaces it, as in a browser */
    }

    while (GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        /* F5 reloads wherever the focus is, as in the browser it stands in for */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5)
        {
            navigate();
            continue;
        }
        if (IsDialogMessageW( frame, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
