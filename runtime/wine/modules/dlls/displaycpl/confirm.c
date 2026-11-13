/*
 * Display of the Control Panel: after a change of the monitors
 *
 * The question of Windows 11, "Зберегти ці параметри дисплея?", which goes
 * back to what was there if nobody answers in 15 seconds (a mode the monitor
 * cannot show leaves it dark); the monitors' numbers for "Ідентифікувати".
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "displaycpl.h"

#define KEEP_SECONDS 15

struct keep
{
    int left;
    HFONT title;
};

static void keep_text( HWND dialog, struct keep *keep )
{
    SetDlgItemTextW( dialog, IDC_KEEP_TEXT, format_string( IDS_KEEP_TEXT, keep->left ) );
}

static INT_PTR CALLBACK keep_proc( HWND dialog, UINT msg, WPARAM wp, LPARAM lp )
{
    struct keep *keep = (struct keep *)GetWindowLongPtrW( dialog, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LOGFONTW font;
        RECT rect, owner;
        HWND parent = GetWindow( dialog, GW_OWNER );

        keep = (struct keep *)lp;
        SetWindowLongPtrW( dialog, DWLP_USER, (LONG_PTR)keep );
        keep->left = KEEP_SECONDS;
        GetObjectW( (HFONT)SendMessageW( dialog, WM_GETFONT, 0, 0 ), sizeof(font), &font );
        font.lfHeight = -px( 20 );
        font.lfWeight = FW_SEMIBOLD;
        keep->title = CreateFontIndirectW( &font );
        SendDlgItemMessageW( dialog, IDC_KEEP_TITLE, WM_SETFONT, (WPARAM)keep->title, FALSE );
        SetWindowTextW( dialog, load_string( IDS_DISPLAY ) );
        SetDlgItemTextW( dialog, IDC_KEEP_TITLE, load_string( IDS_KEEP_TITLE ) );
        SetDlgItemTextW( dialog, IDOK, load_string( IDS_KEEP ) );
        SetDlgItemTextW( dialog, IDCANCEL, load_string( IDS_REVERT ) );
        keep_text( dialog, keep );
        /* in the middle of the window that asked, which may have moved to another monitor */
        GetWindowRect( dialog, &rect );
        if (parent && GetWindowRect( parent, &owner ))
            SetWindowPos( dialog, HWND_TOP, (owner.left + owner.right - (rect.right - rect.left)) / 2,
                          (owner.top + owner.bottom - (rect.bottom - rect.top)) / 2, 0, 0, SWP_NOSIZE );
        SetTimer( dialog, 1, 1000, NULL );
        return TRUE;
    }
    case WM_TIMER:
        if (--keep->left <= 0)
        {
            KillTimer( dialog, 1 );
            EndDialog( dialog, IDCANCEL );
        }
        else keep_text( dialog, keep );
        return TRUE;
    case WM_COMMAND:
        if (LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL)
        {
            KillTimer( dialog, 1 );
            EndDialog( dialog, LOWORD( wp ) );
        }
        return TRUE;
    case WM_DESTROY:
        if (keep && keep->title) DeleteObject( keep->title );
        return FALSE;
    }
    return FALSE;
}

BOOL keep_settings( HWND owner )
{
    struct keep keep = { 0 };

    return DialogBoxParamW( cp_instance, MAKEINTRESOURCEW( IDD_KEEP ), GetAncestor( owner, GA_ROOT ), keep_proc,
                            (LPARAM)&keep ) == IDOK;
}

void show_failure( HWND owner, LONG result )
{
    if (result == DISP_CHANGE_SUCCESSFUL) return;
    MessageBoxW( GetAncestor( owner, GA_ROOT ), load_string( result == DISP_CHANGE_RESTART ? IDS_RESTART : IDS_FAILED ),
                 load_string( IDS_DISPLAY ), MB_OK | (result == DISP_CHANGE_RESTART ? MB_ICONINFORMATION : MB_ICONERROR) );
}

/**********************************************************************
 *          The monitors' numbers
 *
 * Windows 11 shows each one's number in a dark box at its bottom left for a
 * few seconds; nothing takes the focus.
 */

#define NUMBER_CLASS L"ArcticDisplayNumber"

static LRESULT WINAPI number_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_TIMER:
        DestroyWindow( hwnd );
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        WCHAR text[8];
        RECT rect;
        HFONT font, old;
        HBRUSH brush = CreateSolidBrush( RGB( 0x20, 0x20, 0x20 ) );
        NONCLIENTMETRICSW metrics = { sizeof(metrics) };

        GetClientRect( hwnd, &rect );
        FillRect( hdc, &rect, brush );
        DeleteObject( brush );
        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
        metrics.lfMessageFont.lfHeight = -(rect.bottom * 2 / 3);
        metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
        metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        font = CreateFontIndirectW( &metrics.lfMessageFont );
        old = SelectObject( hdc, font );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, RGB( 255, 255, 255 ) );
        swprintf( text, ARRAY_SIZE(text), L"%u", (UINT)GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
        DrawTextW( hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
        SelectObject( hdc, old );
        DeleteObject( font );
        EndPaint( hwnd, &ps );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

void identify_monitors( const struct displays *d )
{
    static BOOL registered;
    int size = px( 120 ), margin = px( 40 );

    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.lpfnWndProc = number_proc;
        cls.hInstance = cp_instance;
        cls.lpszClassName = NUMBER_CLASS;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        RegisterClassW( &cls );
        registered = TRUE;
    }
    for (UINT i = 0; i < d->count; i++)
    {
        const struct monitor *m = &d->list[i];
        HWND hwnd;

        if (!m->attached) continue;
        hwnd = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, NUMBER_CLASS, NULL, WS_POPUP,
                                m->pos.x + margin, m->pos.y + (int)m->current.dmPelsHeight - margin - size,
                                size, size, NULL, NULL, cp_instance, NULL );
        if (!hwnd) continue;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, m->number );
        SetTimer( hwnd, 1, 3000, NULL );
        ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    }
}
