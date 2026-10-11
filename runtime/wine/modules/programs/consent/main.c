/*
 * consent.exe: the window of User Account Control
 *
 * Arctic's own consent UI, as Windows 7 shows it. It is only a window: it
 * asks nothing of the system and decides nothing. The Application
 * Information service (appinfo.dll) starts it with its standard handles on
 * a pipe and speaks these lines with it; behind the service the host's
 * arctic-lxss, running as root, is what checks the password and records the
 * answer (docs/linux-gate.md).
 *
 *   in   prompt <0|1 password needed> <program> <path> <what is asked>
 *   out  yes <password>   |   no
 *   in   ok   |   wrong   |   wait <seconds>
 *
 * The desktop behind is dimmed and the window sits in the band above every
 * program window (SetWindowBand, Wine 0041), which is as close to Windows'
 * secure desktop as Arctic gets until CreateDesktop/SwitchDesktop work.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"

#define ID_YES      100
#define ID_NO       101
#define ID_PASSWORD 102

#define WM_SERVER_LINE (WM_APP + 1)  /* wParam: a line from the service, to free */

/* Windows 7's colours for this window */
#define CAPTION_COLOR  RGB(  7,  70, 128)
#define CAPTION_TEXT   RGB(255, 255, 255)
#define BODY_COLOR     RGB(255, 255, 255)
#define FOOT_COLOR     RGB(240, 240, 240)
#define RULE_COLOR     RGB(223, 223, 223)
#define HEADING_COLOR  RGB(  0,  51, 153)
#define LINK_COLOR     RGB(  0, 102, 204)
#define ERROR_COLOR    RGB(168,   0,   0)

struct consent
{
    WCHAR  program[256];   /* what the window calls the program being run */
    WCHAR  source[256];    /* the program that asked for it */
    WCHAR  path[MAX_PATH]; /* its image, under "Show details" */
    WCHAR  command[1024];  /* what it wants to run, under "Show details" */
    BOOL   password_needed;
    BOOL   settings;       /* the request is the gate's own settings */
    BOOL   details;        /* "Show details" is open */
    BOOL   waiting;        /* an answer is with the service */
    WCHAR  message[256];   /* "The password is incorrect." and such */
    int    pause_left;     /* seconds the Yes button stays disabled */
    HWND   hwnd, backdrop, yes, no, password;
    HFONT  font, heading_font;
    HICON  shield;
    int    dpi;
};

static struct consent consent;
static HANDLE server_in, server_out;

static int scale( int value )
{
    return MulDiv( value, consent.dpi, 96 );
}

static void load_string( UINT id, WCHAR *out, int len )
{
    if (!LoadStringW( GetModuleHandleW( NULL ), id, out, len )) out[0] = 0;
}

/* ---- the service ---- */

static void answer( const char *line )
{
    DWORD written;

    WriteFile( server_out, line, (DWORD)strlen( line ), &written, NULL );
    WriteFile( server_out, "\n", 1, &written, NULL );
    FlushFileBuffers( server_out );
}

/* one \n-terminated line, or NULL when the service is gone */
static char *read_line( void )
{
    static char buffer[4096];
    size_t len = 0;
    DWORD got;

    while (len + 1 < sizeof(buffer))
    {
        if (!ReadFile( server_in, buffer + len, 1, &got, NULL ) || !got) return NULL;
        if (buffer[len] == '\n')
        {
            buffer[len] = 0;
            return buffer;
        }
        len++;
    }
    return NULL;
}

static WCHAR *to_wide( const char *utf8 )
{
    int len = MultiByteToWideChar( CP_UTF8, 0, utf8, -1, NULL, 0 );
    WCHAR *out = malloc( (len > 0 ? len : 1) * sizeof(WCHAR) );

    if (len <= 0 || !MultiByteToWideChar( CP_UTF8, 0, utf8, -1, out, len )) out[0] = 0;
    return out;
}

/* everything the service says after the prompt, to the window */
static DWORD WINAPI reader_thread( void *arg )
{
    for (;;)
    {
        char *line = read_line();

        if (!line) /* the service is gone: the same as No */
        {
            PostMessageW( consent.hwnd, WM_CLOSE, 0, 0 );
            return 0;
        }
        PostMessageW( consent.hwnd, WM_SERVER_LINE, (WPARAM)_strdup( line ), 0 );
    }
}

/* ---- the window ---- */

static void play_sound( void )
{
    WCHAR path[MAX_PATH];
    DWORD size = sizeof(path), type;

    /* the system's own "Windows User Account Control" sound */
    if (!RegGetValueW( HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Apps\\.Default\\WindowsUAC\\.Current", NULL,
                       RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, path, &size ) && path[0])
    {
        WCHAR expanded[MAX_PATH];

        if (ExpandEnvironmentStringsW( path, expanded, ARRAY_SIZE(expanded) ))
            PlaySoundW( expanded, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT );
    }
}

static int window_height( void )
{
    int height = scale( consent.password_needed ? 252 : 206 );

    if (consent.details) height += scale( 58 );
    if (consent.message[0]) height += scale( 20 );
    return height;
}

static void place_controls( void )
{
    int width = scale( 510 ), height = window_height(), button_y = height - scale( 44 );
    RECT work;

    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    SetWindowPos( consent.hwnd, NULL, (work.left + work.right - width) / 2,
                  (work.top + work.bottom - height) / 2, width, height, SWP_NOZORDER );
    SetWindowPos( consent.no, NULL, width - scale( 108 ), button_y, scale( 92 ), scale( 26 ), SWP_NOZORDER );
    SetWindowPos( consent.yes, NULL, width - scale( 208 ), button_y, scale( 92 ), scale( 26 ), SWP_NOZORDER );
    if (consent.password)
        SetWindowPos( consent.password, NULL, scale( 74 ), button_y - scale( 36 ), scale( 200 ), scale( 23 ),
                      SWP_NOZORDER );
    InvalidateRect( consent.hwnd, NULL, TRUE );
}

static void draw_text( HDC hdc, const WCHAR *text, int x, int y, int width, COLORREF colour, HFONT font )
{
    RECT rect = { scale( x ), scale( y ), scale( x + width ), scale( y ) + 4 * consent.dpi / 96 * 20 };
    HFONT old = SelectObject( hdc, font ? font : consent.font );

    SetTextColor( hdc, colour );
    DrawTextW( hdc, text, -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( hdc, old );
}

/* where "Show details" is, so a click on it can be seen */
static RECT link_rect( void )
{
    RECT rect = { scale( 20 ), window_height() - scale( 76 ), scale( 200 ), window_height() - scale( 58 ) };

    return rect;
}

static void paint( HWND hwnd )
{
    WCHAR text[512], label[128];
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( hwnd, &ps );
    RECT client, rect;
    int y;

    GetClientRect( hwnd, &client );
    rect = client;
    rect.bottom = scale( 30 );
    SetBkMode( hdc, TRANSPARENT );
    FillRect( hdc, &rect, CreateSolidBrush( CAPTION_COLOR ) );
    rect.top = rect.bottom;
    rect.bottom = client.bottom - scale( 56 );
    FillRect( hdc, &rect, CreateSolidBrush( BODY_COLOR ) );
    rect.top = rect.bottom;
    rect.bottom = client.bottom;
    FillRect( hdc, &rect, CreateSolidBrush( FOOT_COLOR ) );
    rect.bottom = rect.top + 1;
    FillRect( hdc, &rect, CreateSolidBrush( RULE_COLOR ) );

    load_string( IDS_TITLE, text, ARRAY_SIZE(text) );
    draw_text( hdc, text, 10, 7, 480, CAPTION_TEXT, NULL );

    if (consent.shield) DrawIconEx( hdc, scale( 20 ), scale( 44 ), consent.shield, scale( 32 ), scale( 32 ), 0,
                                    NULL, DI_NORMAL );
    load_string( IDS_HEADING, text, ARRAY_SIZE(text) );
    draw_text( hdc, text, 64, 44, 420, HEADING_COLOR, consent.heading_font );

    y = 84;
    load_string( IDS_PROGRAM, label, ARRAY_SIZE(label) );
    draw_text( hdc, label, 64, y, 120, GetSysColor( COLOR_WINDOWTEXT ), NULL );
    draw_text( hdc, consent.program, 150, y, 330, GetSysColor( COLOR_WINDOWTEXT ), NULL );
    y += 18;
    load_string( IDS_PUBLISHER, label, ARRAY_SIZE(label) );
    draw_text( hdc, label, 64, y, 120, GetSysColor( COLOR_WINDOWTEXT ), NULL );
    load_string( IDS_PUBLISHER_NAME, text, ARRAY_SIZE(text) );
    draw_text( hdc, text, 150, y, 330, GetSysColor( COLOR_WINDOWTEXT ), NULL );
    y += 18;
    load_string( IDS_REQUESTER, label, ARRAY_SIZE(label) );
    draw_text( hdc, label, 64, y, 120, GetSysColor( COLOR_WINDOWTEXT ), NULL );
    draw_text( hdc, consent.source, 150, y, 330, GetSysColor( COLOR_WINDOWTEXT ), NULL );

    if (consent.details)
    {
        y += 22;
        load_string( IDS_LOCATION, label, ARRAY_SIZE(label) );
        draw_text( hdc, label, 64, y, 120, GetSysColor( COLOR_GRAYTEXT ), NULL );
        draw_text( hdc, consent.path, 150, y, 330, GetSysColor( COLOR_GRAYTEXT ), NULL );
        y += 18;
        load_string( IDS_COMMAND, label, ARRAY_SIZE(label) );
        draw_text( hdc, label, 64, y, 120, GetSysColor( COLOR_GRAYTEXT ), NULL );
        draw_text( hdc, consent.command, 150, y, 330, GetSysColor( COLOR_GRAYTEXT ), NULL );
    }

    if (consent.password_needed)
    {
        load_string( IDS_PASSWORD_PROMPT, text, ARRAY_SIZE(text) );
        draw_text( hdc, text, 20, (window_height() - scale( 56 )) * 96 / consent.dpi - 58, 460,
                   GetSysColor( COLOR_WINDOWTEXT ), NULL );
    }
    if (consent.message[0])
        draw_text( hdc, consent.message, 20, (window_height() - scale( 56 )) * 96 / consent.dpi - 20, 460,
                   ERROR_COLOR, NULL );

    /* "Show details" / "Hide details", as a link */
    {
        RECT where = link_rect();
        HFONT underlined;
        LOGFONTW lf;

        load_string( consent.details ? IDS_HIDE_DETAILS : IDS_SHOW_DETAILS, text, ARRAY_SIZE(text) );
        GetObjectW( consent.font, sizeof(lf), &lf );
        lf.lfUnderline = TRUE;
        underlined = CreateFontIndirectW( &lf );
        SelectObject( hdc, underlined );
        SetTextColor( hdc, LINK_COLOR );
        DrawTextW( hdc, text, -1, &where, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX );
        SelectObject( hdc, consent.font );
        DeleteObject( underlined );
    }
    EndPaint( hwnd, &ps );
}

static void set_message( UINT id, int seconds )
{
    WCHAR format[256];

    if (!id) consent.message[0] = 0;
    else
    {
        load_string( id, format, ARRAY_SIZE(format) );
        if (seconds) swprintf( consent.message, ARRAY_SIZE(consent.message), format, seconds );
        else wcscpy( consent.message, format );
    }
    place_controls();
}

static void send_answer( BOOL yes )
{
    if (consent.waiting) return;
    if (!yes)
    {
        answer( "no" );
        ExitProcess( 1 );
    }
    {
        WCHAR typed[256] = L"";
        char line[1024] = "yes";

        if (consent.password) GetWindowTextW( consent.password, typed, ARRAY_SIZE(typed) );
        if (typed[0])
        {
            int len = WideCharToMultiByte( CP_UTF8, 0, typed, -1, line + 4, (int)sizeof(line) - 5, NULL, NULL );

            line[3] = '\t';
            if (len <= 0) line[4] = 0;
        }
        consent.waiting = TRUE;
        EnableWindow( consent.yes, FALSE );
        answer( line );
        SecureZeroMemory( line, sizeof(line) );
        SecureZeroMemory( typed, sizeof(typed) );
    }
}

static void server_line( char *line )
{
    if (!strcmp( line, "ok" )) ExitProcess( 0 );
    consent.waiting = FALSE;
    if (!strncmp( line, "wait", 4 ))
    {
        consent.pause_left = atoi( line + 4 );
        if (consent.pause_left <= 0) consent.pause_left = 30;
        set_message( IDS_WAIT, consent.pause_left );
        SetTimer( consent.hwnd, 1, 1000, NULL );
    }
    else /* "wrong", and anything else the service may say */
    {
        set_message( IDS_WRONG, 0 );
        EnableWindow( consent.yes, TRUE );
    }
    if (consent.password)
    {
        SetWindowTextW( consent.password, L"" );
        SetFocus( consent.password );
    }
}

static LRESULT CALLBACK consent_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_PAINT:
        paint( hwnd );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
    {
        POINT at;
        RECT where = link_rect();

        GetCursorPos( &at );
        ScreenToClient( hwnd, &at );
        if (PtInRect( &where, at ))
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_HAND ) );
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN:
    {
        POINT at = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT where = link_rect();

        if (PtInRect( &where, at ))
        {
            consent.details = !consent.details;
            place_controls();
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_YES) send_answer( TRUE );
        else if (LOWORD(wp) == ID_NO) send_answer( FALSE );
        return 0;
    case WM_TIMER:
        if (--consent.pause_left > 0) set_message( IDS_WAIT, consent.pause_left );
        else
        {
            KillTimer( hwnd, 1 );
            set_message( 0, 0 );
            EnableWindow( consent.yes, TRUE );
            if (consent.password) SetFocus( consent.password );
        }
        return 0;
    case WM_SERVER_LINE:
    {
        char *line = (char *)wp;

        server_line( line );
        free( line );
        return 0;
    }
    case WM_CLOSE:
        send_answer( FALSE );
        return 0;
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void create_backdrop( void )
{
    int x = GetSystemMetrics( SM_XVIRTUALSCREEN ), y = GetSystemMetrics( SM_YVIRTUALSCREEN );
    int width = GetSystemMetrics( SM_CXVIRTUALSCREEN ), height = GetSystemMetrics( SM_CYVIRTUALSCREEN );

    consent.backdrop = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        L"ArcticConsentBackdrop", NULL, WS_POPUP, x, y, width, height, NULL, NULL,
                                        GetModuleHandleW( NULL ), NULL );
    if (!consent.backdrop) return;
    SetLayeredWindowAttributes( consent.backdrop, 0, 120, LWA_ALPHA );
    ShowWindow( consent.backdrop, SW_SHOWNOACTIVATE );
}

/* above every program window, as Windows puts the secure desktop */
static void raise_band( HWND hwnd )
{
    BOOL (WINAPI *set_window_band)( HWND, HWND, DWORD ) =
        (void *)GetProcAddress( GetModuleHandleW( L"user32.dll" ), "SetWindowBand" );

    if (set_window_band) set_window_band( hwnd, NULL, 4 /* ZBID_SYSTEM_TOOLS */ );
}

static void make_fonts( void )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    LOGFONTW big;

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    consent.font = CreateFontIndirectW( &metrics.lfMessageFont );
    big = metrics.lfMessageFont;
    big.lfHeight = big.lfHeight * 6 / 5;
    consent.heading_font = CreateFontIndirectW( &big );
}

/* "prompt <0|1> <program> <path> <what is asked>" */
static BOOL take_prompt( void )
{
    WCHAR program[256];
    char *line = read_line(), *field[5] = { 0 };
    WCHAR *wide;
    int count = 0;

    if (!line || strncmp( line, "prompt\t", 7 )) return FALSE;
    field[count++] = line;
    for (char *p = line; *p && count < 5; p++)
        if (*p == '\t')
        {
            *p = 0;
            field[count++] = p + 1;
        }
    if (count < 4) return FALSE;
    consent.password_needed = field[1][0] == '1';
    consent.settings = count > 4 && !strcmp( field[4], "settings" );

    wide = to_wide( field[2] );
    wcscpy_s( consent.source, ARRAY_SIZE(consent.source), wide );
    free( wide );
    wide = to_wide( field[3] );
    wcscpy_s( consent.path, ARRAY_SIZE(consent.path), wide );
    free( wide );
    if (count > 4)
    {
        wide = to_wide( field[4] );
        wcscpy_s( consent.command, ARRAY_SIZE(consent.command), wide );
        free( wide );
    }
    load_string( consent.settings ? IDS_PROGRAM_SETTINGS : IDS_PROGRAM_WSL, program, ARRAY_SIZE(program) );
    wcscpy_s( consent.program, ARRAY_SIZE(consent.program), program );
    return TRUE;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WNDCLASSEXW backdrop_class = { sizeof(backdrop_class) }, window_class = { sizeof(window_class) };
    WCHAR title[256], text[256];
    HDC screen;
    MSG msg;

    server_in = GetStdHandle( STD_INPUT_HANDLE );
    server_out = GetStdHandle( STD_OUTPUT_HANDLE );
    if (!take_prompt()) return 1;

    screen = GetDC( NULL );
    consent.dpi = GetDeviceCaps( screen, LOGPIXELSX );
    ReleaseDC( NULL, screen );
    if (consent.dpi < 96) consent.dpi = 96;
    make_fonts();
    if (!(consent.shield = LoadIconW( NULL, (const WCHAR *)IDI_SHIELD )))
        consent.shield = LoadIconW( NULL, (const WCHAR *)IDI_WARNING );

    backdrop_class.lpfnWndProc = DefWindowProcW;
    backdrop_class.hInstance = instance;
    backdrop_class.hbrBackground = CreateSolidBrush( RGB(0, 0, 0) );
    backdrop_class.lpszClassName = L"ArcticConsentBackdrop";
    RegisterClassExW( &backdrop_class );
    window_class.lpfnWndProc = consent_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    window_class.lpszClassName = L"ArcticConsent";
    RegisterClassExW( &window_class );

    create_backdrop();
    load_string( IDS_TITLE, title, ARRAY_SIZE(title) );
    consent.hwnd = CreateWindowExW( WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"ArcticConsent", title, WS_POPUP | WS_BORDER,
                                    0, 0, scale( 510 ), scale( 206 ), NULL, NULL, instance, NULL );
    if (!consent.hwnd) return 1;

    load_string( IDS_YES, text, ARRAY_SIZE(text) );
    consent.yes = CreateWindowExW( 0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0,
                                   0, consent.hwnd, (HMENU)ID_YES, instance, NULL );
    load_string( IDS_NO, text, ARRAY_SIZE(text) );
    consent.no = CreateWindowExW( 0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                                  consent.hwnd, (HMENU)ID_NO, instance, NULL );
    if (consent.password_needed)
    {
        consent.password = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", NULL,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 0,
                                            0, consent.hwnd, (HMENU)ID_PASSWORD, instance, NULL );
        load_string( IDS_PASSWORD_CUE, text, ARRAY_SIZE(text) );
        SendMessageW( consent.password, EM_SETCUEBANNER, 0, (LPARAM)text );
    }
    SendMessageW( consent.yes, WM_SETFONT, (WPARAM)consent.font, TRUE );
    SendMessageW( consent.no, WM_SETFONT, (WPARAM)consent.font, TRUE );
    if (consent.password) SendMessageW( consent.password, WM_SETFONT, (WPARAM)consent.font, TRUE );

    place_controls();
    raise_band( consent.backdrop );
    raise_band( consent.hwnd );
    ShowWindow( consent.hwnd, SW_SHOW );
    SetForegroundWindow( consent.hwnd );
    SetActiveWindow( consent.hwnd );
    SetFocus( consent.password ? consent.password : consent.no );
    play_sound();
    CreateThread( NULL, 0, reader_thread, NULL, 0, NULL );

    while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
    {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)
        {
            if (IsWindowEnabled( consent.yes )) send_answer( TRUE );
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            send_answer( FALSE );
            continue;
        }
        if (IsDialogMessageW( consent.hwnd, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    answer( "no" );
    return 1;
}
