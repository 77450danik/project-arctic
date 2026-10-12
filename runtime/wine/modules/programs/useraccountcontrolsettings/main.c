/*
 * UserAccountControlSettings.exe: the settings of User Account Control
 *
 * Windows 7's window, reached from "Облікові записи користувачів" of the
 * Control Panel (usercpl.dll): white body with the blue instruction, a grey
 * footer with OK and Cancel. In Windows it holds the slider of the four
 * notification levels; here it holds what Arctic's User Account Control
 * guards today, the Linux subsystem (docs/linux-gate.md): whether a password
 * is required for it, and the password itself.
 *
 * Nothing is applied until OK, and applying always goes through the consent
 * window with the password that is set now - as changing the settings of UAC
 * in Windows always asks, whatever consent the program already has. The host
 * (arctic-lxss, root) is what asks and what writes: gate.c speaks to it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"
#include "gate.h"

#define ID_REQUIRE  100
#define ID_CHANGE   101
#define ID_OK       102
#define ID_CANCEL   103

/* Windows 7's colours of a Control Panel window */
#define BODY_COLOR     RGB(255, 255, 255)
#define FOOT_COLOR     RGB(240, 240, 240)
#define RULE_COLOR     RGB(223, 223, 223)
#define HEADING_COLOR  RGB(  0,  51, 153)
#define GROUP_COLOR    RGB( 30,  57,  91)
#define GRAY_COLOR     RGB(109, 109, 109)

#define WIDTH   560
#define HEIGHT  380
#define MARGIN   20
#define FOOTER   46

static struct
{
    HWND hwnd, require, change, ok, cancel;
    HFONT font, heading_font, group_font;
    BOOL required;      /* what the gate says now */
    int dpi;
} settings;

static int scale( int value )
{
    return MulDiv( value, settings.dpi, 96 );
}

static const WCHAR *load( UINT id )
{
    static WCHAR text[4][1024];
    static int next;
    WCHAR *out = text[next++ & 3];

    if (!LoadStringW( GetModuleHandleW( NULL ), id, out, 1024 )) out[0] = 0;
    return out;
}

static void say( UINT id, UINT type )
{
    MessageBoxW( settings.hwnd, load( id ), load( IDS_TITLE ), type | MB_OK );
}

/* "The operation was canceled by the user.", in the language of the system */
static void cancelled( void )
{
    WCHAR *text = NULL;

    if (FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                        NULL, ERROR_CANCELLED, 0, (WCHAR *)&text, 0, NULL ) && text)
    {
        MessageBoxW( settings.hwnd, text, load( IDS_TITLE ), MB_ICONINFORMATION | MB_OK );
        LocalFree( text );
    }
    else say( IDS_NOT_CHANGED, MB_ICONINFORMATION );
}

static void report( enum gate_result result )
{
    if (result == GATE_DENIED) cancelled();
    else if (result == GATE_ERROR) say( IDS_NO_SUBSYSTEM, MB_ICONERROR );
}

/* ---- the password ---- */

static INT_PTR CALLBACK password_proc( HWND dialog, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextW( dialog, IDC_PASSWORD_NOTE, load( IDS_PASSWORD_NOTE ) );
        SendMessageW( GetDlgItem( dialog, IDOK ), BCM_SETSHIELD, 0, TRUE );
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL)
        {
            EndDialog( dialog, 0 );
            return TRUE;
        }
        if (LOWORD(wp) == IDOK)
        {
            WCHAR new_password[256] = L"", confirm[256] = L"";

            GetDlgItemTextW( dialog, IDC_NEW_PASSWORD, new_password, ARRAY_SIZE(new_password) );
            GetDlgItemTextW( dialog, IDC_CONFIRM_PASSWORD, confirm, ARRAY_SIZE(confirm) );
            if (!new_password[0])
            {
                MessageBoxW( dialog, load( IDS_PASSWORD_EMPTY ), load( IDS_TITLE ), MB_ICONWARNING | MB_OK );
                SetFocus( GetDlgItem( dialog, IDC_NEW_PASSWORD ) );
                return TRUE;
            }
            if (wcscmp( new_password, confirm ))
            {
                MessageBoxW( dialog, load( IDS_PASSWORDS_DIFFER ), load( IDS_TITLE ), MB_ICONWARNING | MB_OK );
                SetDlgItemTextW( dialog, IDC_NEW_PASSWORD, L"" );
                SetDlgItemTextW( dialog, IDC_CONFIRM_PASSWORD, L"" );
                SetFocus( GetDlgItem( dialog, IDC_NEW_PASSWORD ) );
                return TRUE;
            }
            /* the host asks for the password set now, then takes the new one */
            {
                enum gate_result result = gate_set_password( new_password );

                SecureZeroMemory( new_password, sizeof(new_password) );
                SecureZeroMemory( confirm, sizeof(confirm) );
                if (result != GATE_OK)
                {
                    report( result );
                    return TRUE;
                }
            }
            /* a password that is set is a password that is meant to be asked
             * for: the box is ticked, and OK applies it like any other change */
            SendMessageW( settings.require, BM_SETCHECK, BST_CHECKED, 0 );
            EndDialog( dialog, 1 );
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* ---- the window ---- */

static void paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( hwnd, &ps );
    RECT client, rect;
    HFONT old;
    int y;

    GetClientRect( hwnd, &client );
    rect = client;
    rect.bottom = client.bottom - scale( FOOTER );
    FillRect( hdc, &rect, (HBRUSH)GetStockObject( WHITE_BRUSH ) );
    rect.top = rect.bottom;
    rect.bottom = client.bottom;
    FillRect( hdc, &rect, CreateSolidBrush( FOOT_COLOR ) );
    rect.bottom = rect.top + 1;
    FillRect( hdc, &rect, CreateSolidBrush( RULE_COLOR ) );

    SetBkMode( hdc, TRANSPARENT );
    old = SelectObject( hdc, settings.heading_font );
    SetTextColor( hdc, HEADING_COLOR );
    rect = client;
    rect.left = scale( MARGIN );
    rect.right = client.right - scale( MARGIN );
    rect.top = scale( MARGIN );
    rect.bottom = rect.top + scale( 44 );
    DrawTextW( hdc, load( IDS_HEADING ), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX );
    y = MARGIN + 30;

    SelectObject( hdc, settings.font );
    SetTextColor( hdc, GetSysColor( COLOR_WINDOWTEXT ) );
    for (UINT id = IDS_ABOUT_UAC; id <= IDS_ABOUT_LINUX; id++)
    {
        rect.top = scale( y );
        rect.bottom = rect.top + scale( 46 );
        DrawTextW( hdc, load( id ), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX );
        y += id == IDS_ABOUT_UAC ? 34 : 48;
    }

    /* the group of the subsystem, with the line Windows 7 draws after it */
    SelectObject( hdc, settings.group_font );
    SetTextColor( hdc, GROUP_COLOR );
    rect.top = scale( y );
    rect.bottom = rect.top + scale( 20 );
    DrawTextW( hdc, load( IDS_GROUP ), -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX );
    rect.top = rect.bottom + scale( 2 );
    rect.bottom = rect.top + 1;
    FillRect( hdc, &rect, CreateSolidBrush( RULE_COLOR ) );

    /* under the checkbox: when the password and the consent are asked for */
    SelectObject( hdc, settings.font );
    SetTextColor( hdc, GRAY_COLOR );
    rect.left = scale( MARGIN + 20 );
    rect.top = scale( y + 56 );
    rect.bottom = rect.top + scale( 40 );
    DrawTextW( hdc, load( IDS_WHEN_ASKED ), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX );

    SelectObject( hdc, old );
    EndPaint( hwnd, &ps );
}

static void apply_and_close( void )
{
    BOOL asked = SendMessageW( settings.require, BM_GETCHECK, 0, 0 ) == BST_CHECKED;

    if (asked != settings.required)
    {
        enum gate_result result = gate_require_password( asked );

        if (result != GATE_OK)
        {
            report( result );
            SendMessageW( settings.require, BM_SETCHECK, settings.required ? BST_CHECKED : BST_UNCHECKED, 0 );
            return;
        }
        settings.required = asked;
    }
    DestroyWindow( settings.hwnd );
}

static LRESULT CALLBACK window_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_PAINT:
        paint( hwnd );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode( (HDC)wp, TRANSPARENT );
        return (LRESULT)GetStockObject( WHITE_BRUSH );
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_CHANGE:
            DialogBoxParamW( GetModuleHandleW( NULL ), (const WCHAR *)MAKEINTRESOURCE(IDD_PASSWORD), hwnd,
                             password_proc, 0 );
            return 0;
        case ID_OK:
            apply_and_close();
            return 0;
        case ID_CANCEL:
            DestroyWindow( hwnd );
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static HWND button( const WCHAR *text, DWORD style, int x, int y, int width, int height, UINT id )
{
    HWND control = CreateWindowExW( 0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, scale( x ),
                                    scale( y ), scale( width ), scale( height ), settings.hwnd, (HMENU)(UINT_PTR)id,
                                    GetModuleHandleW( NULL ), NULL );

    SendMessageW( control, WM_SETFONT, (WPARAM)settings.font, TRUE );
    return control;
}

static void make_fonts( void )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    LOGFONTW heading, group;

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    settings.font = CreateFontIndirectW( &metrics.lfMessageFont );
    heading = group = metrics.lfMessageFont;
    heading.lfHeight = heading.lfHeight * 4 / 3;
    settings.heading_font = CreateFontIndirectW( &heading );
    group.lfWeight = FW_BOLD;
    settings.group_font = CreateFontIndirectW( &group );
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WNDCLASSEXW window_class = { sizeof(window_class) };
    int group_y = MARGIN + 30 + 34 + 48;
    enum gate_result state;
    RECT work, frame;
    HDC screen;
    MSG msg;

    InitCommonControls();
    screen = GetDC( NULL );
    settings.dpi = GetDeviceCaps( screen, LOGPIXELSX );
    ReleaseDC( NULL, screen );
    if (settings.dpi < 96) settings.dpi = 96;
    make_fonts();

    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    window_class.hbrBackground = (HBRUSH)GetStockObject( WHITE_BRUSH );
    window_class.lpszClassName = L"ArcticUserAccountControlSettings";
    RegisterClassExW( &window_class );

    SetRect( &frame, 0, 0, scale( WIDTH ), scale( HEIGHT ) );
    AdjustWindowRectEx( &frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0 );
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    settings.hwnd = CreateWindowExW( WS_EX_DLGMODALFRAME, window_class.lpszClassName, load( IDS_TITLE ),
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                     (work.left + work.right - (frame.right - frame.left)) / 2,
                                     (work.top + work.bottom - (frame.bottom - frame.top)) / 2,
                                     frame.right - frame.left, frame.bottom - frame.top, NULL, NULL, instance, NULL );
    if (!settings.hwnd) return 1;

    settings.require = button( load( IDS_REQUIRE_PASSWORD ), BS_AUTOCHECKBOX, MARGIN + 20, group_y + 32, WIDTH - 80, 20,
                               ID_REQUIRE );
    settings.change = button( load( IDS_CHANGE_PASSWORD ), BS_PUSHBUTTON, MARGIN + 20, group_y + 108, 150, 26,
                              ID_CHANGE );
    settings.ok = button( load( IDS_OK ), BS_DEFPUSHBUTTON, WIDTH - 208, HEIGHT - 36, 92, 26, ID_OK );
    settings.cancel = button( load( IDS_CANCEL ), BS_PUSHBUTTON, WIDTH - 108, HEIGHT - 36, 92, 26, ID_CANCEL );
    /* both ask the host, which puts up the consent window: the shield, as on
     * every button of Windows that leads to an elevation */
    SendMessageW( settings.change, BCM_SETSHIELD, 0, TRUE );
    SendMessageW( settings.ok, BCM_SETSHIELD, 0, TRUE );

    state = gate_password_required( &settings.required );
    if (state != GATE_OK)
    {
        report( state );
        return 1;
    }
    SendMessageW( settings.require, BM_SETCHECK, settings.required ? BST_CHECKED : BST_UNCHECKED, 0 );

    ShowWindow( settings.hwnd, SW_SHOW );
    SetForegroundWindow( settings.hwnd );
    SetFocus( settings.require );

    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        if (IsDialogMessageW( settings.hwnd, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
