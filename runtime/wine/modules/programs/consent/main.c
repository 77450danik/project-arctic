/*
 * consent.exe: the window of User Account Control
 *
 * Arctic's own consent UI. It is only a window: it asks nothing of the system
 * and decides nothing. The Application Information service (appinfo.dll)
 * starts it with its standard handles on a pipe and speaks these lines with
 * it; behind the service the host's arctic-lxss, running as root, is what
 * checks the password and records the answer (docs/linux-gate.md).
 *
 *   in   prompt <0|1 password needed> <program> <path> <what is asked>
 *   out  yes <password>   |   no
 *   in   ok   |   wrong   |   wait <seconds>
 *
 * The window is an ordinary dialog of the system: its frame, its caption and
 * its controls are Windows', in the visual style of the system, as the window
 * of Windows 7 is - nothing of it is drawn by hand. It sits in the band above
 * every program window (SetWindowBand, Wine 0041), which is as close to
 * Windows' secure desktop as Arctic gets until CreateDesktop/SwitchDesktop
 * work.
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

#define WM_SERVER_LINE (WM_APP + 1)  /* wParam: a line from the service, to free */

#define ERROR_COLOR RGB(168, 0, 0)

static struct
{
    WCHAR program[256];    /* what the window calls the program being run */
    WCHAR source[256];     /* the program that asked for it */
    WCHAR path[MAX_PATH];  /* its image, under "Показати подробиці" */
    WCHAR command[1024];   /* what it wants to run, under "Показати подробиці" */
    BOOL  password_needed;
    BOOL  settings;        /* the request is the gate's own settings */
    BOOL  details;         /* "Показати подробиці" is open */
    BOOL  waiting;         /* an answer is with the service */
    int   pause_left;      /* seconds the Yes button stays disabled */
    HWND  hwnd;
    HBRUSH background;
} consent;

static HANDLE server_in, server_out;
static HWND asked_from;   /* the window that was in front when we came up */

static const WCHAR *load( UINT id )
{
    static WCHAR text[4][1024];
    static int next;
    WCHAR *out = text[next++ & 3];

    if (!LoadStringW( GetModuleHandleW( NULL ), id, out, 1024 )) out[0] = 0;
    return out;
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

/* "prompt <0|1> <program> <path> <what is asked>" */
static BOOL take_prompt( void )
{
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
    wcscpy_s( consent.program, ARRAY_SIZE(consent.program),
              load( consent.settings ? IDS_PROGRAM_SETTINGS : IDS_PROGRAM_WSL ) );
    return TRUE;
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

/* above every program window, as Windows puts the secure desktop */
static void raise_band( HWND hwnd )
{
    BOOL (WINAPI *set_window_band)( HWND, HWND, DWORD ) =
        (void *)GetProcAddress( GetModuleHandleW( L"user32.dll" ), "SetWindowBand" );

    if (set_window_band) set_window_band( hwnd, NULL, 4 /* ZBID_SYSTEM_TOOLS */ );
}

static void show( HWND dialog, UINT id, BOOL visible )
{
    ShowWindow( GetDlgItem( dialog, id ), visible ? SW_SHOW : SW_HIDE );
}

/* The dialog holds every line a request may need; what this one does not use
 * is hidden and what follows moves up, as a dialog that grows and shrinks
 * does. The places in the template are the measure: nothing is laid out by
 * hand here, only moved by the height of what is gone. */
#define DETAILS_DU   24   /* the two rows under "Запит від" */
#define PASSWORD_DU  26   /* the line that asks for the password, and the box */
#define MESSAGE_DU   12   /* "Неправильний пароль." */

static void move_up( HWND dialog, UINT id, const RECT *place, int up )
{
    SetWindowPos( GetDlgItem( dialog, id ), NULL, place->left, place->top - up, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
}

static void relayout( HWND dialog )
{
    static const UINT moving[] = { IDC_DETAILS, IDC_PASSWORD_PROMPT, IDC_PASSWORD, IDC_MESSAGE, IDC_ANSWER_YES, IDC_ANSWER_NO };
    static const UINT details_rows[] = { IDC_LOCATION_LABEL, IDC_LOCATION, IDC_COMMAND_LABEL, IDC_COMMAND };
    static RECT places[ARRAY_SIZE(moving)];
    static BOOL remembered;
    RECT unit = { 0, 0, 4, 8 }, item, client;
    WCHAR message[256];
    int du, no_details, no_password, no_message;

    MapDialogRect( dialog, &unit );
    du = unit.bottom;   /* eight dialog units, in pixels */

    if (!remembered)
    {
        for (unsigned int i = 0; i < ARRAY_SIZE(moving); i++)
        {
            GetWindowRect( GetDlgItem( dialog, moving[i] ), &places[i] );
            MapWindowPoints( NULL, dialog, (POINT *)&places[i], 2 );
        }
        remembered = TRUE;
    }

    GetDlgItemTextW( dialog, IDC_MESSAGE, message, ARRAY_SIZE(message) );
    no_details = consent.details ? 0 : DETAILS_DU * du / 8;
    no_password = consent.password_needed ? 0 : PASSWORD_DU * du / 8;
    /* while a password is asked for, the line for "Неправильний пароль." is
     * kept even when it is empty: the window then does not jump about between
     * attempts, and the box under the hands of the person stays where it was */
    no_message = (message[0] || consent.password_needed) ? 0 : MESSAGE_DU * du / 8;

    for (unsigned int i = 0; i < ARRAY_SIZE(details_rows); i++) show( dialog, details_rows[i], consent.details );
    show( dialog, IDC_PASSWORD_PROMPT, consent.password_needed );
    show( dialog, IDC_PASSWORD, consent.password_needed );
    show( dialog, IDC_MESSAGE, message[0] != 0 );

    move_up( dialog, IDC_DETAILS, &places[0], no_details );
    move_up( dialog, IDC_PASSWORD_PROMPT, &places[1], no_details );
    move_up( dialog, IDC_PASSWORD, &places[2], no_details );
    move_up( dialog, IDC_MESSAGE, &places[3], no_details + no_password );
    move_up( dialog, IDC_ANSWER_YES, &places[4], no_details + no_password + no_message );
    move_up( dialog, IDC_ANSWER_NO, &places[5], no_details + no_password + no_message );

    /* the window ends under the buttons */
    GetWindowRect( GetDlgItem( dialog, IDC_ANSWER_NO ), &item );
    MapWindowPoints( NULL, dialog, (POINT *)&item, 2 );
    GetClientRect( dialog, &client );
    GetWindowRect( dialog, &unit );
    SetWindowPos( dialog, NULL, 0, 0, unit.right - unit.left,
                  (unit.bottom - unit.top) - (client.bottom - (item.bottom + 10 * du / 8)),
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
    InvalidateRect( dialog, NULL, TRUE );
}

static void set_message( HWND dialog, UINT id, int seconds )
{
    WCHAR text[256] = L"";

    if (id)
    {
        if (seconds) swprintf( text, ARRAY_SIZE(text), load( id ), seconds );
        else wcscpy_s( text, ARRAY_SIZE(text), load( id ) );
    }
    SetDlgItemTextW( dialog, IDC_MESSAGE, text );
    relayout( dialog );
}

/* back to the console (or whatever else) that asked, as closing any window of
 * the system gives the focus back to the one under it */
static void give_focus_back( void )
{
    if (asked_from && IsWindow( asked_from ) && IsWindowVisible( asked_from )) SetForegroundWindow( asked_from );
}

static void send_answer( HWND dialog, BOOL yes )
{
    WCHAR typed[256] = L"";
    char line[1024] = "yes";

    if (consent.waiting) return;
    if (!yes)
    {
        answer( "no" );
        give_focus_back();
        ExitProcess( 1 );
    }
    GetDlgItemTextW( dialog, IDC_PASSWORD, typed, ARRAY_SIZE(typed) );
    if (typed[0])
    {
        int len = WideCharToMultiByte( CP_UTF8, 0, typed, -1, line + 4, (int)sizeof(line) - 5, NULL, NULL );

        line[3] = '\t';
        if (len <= 0) line[4] = 0;
    }
    consent.waiting = TRUE;
    EnableWindow( GetDlgItem( dialog, IDC_ANSWER_YES ), FALSE );
    answer( line );
    SecureZeroMemory( line, sizeof(line) );
    SecureZeroMemory( typed, sizeof(typed) );
}

static void server_line( HWND dialog, char *line )
{
    if (!strcmp( line, "ok" ))
    {
        give_focus_back();
        ExitProcess( 0 );
    }
    consent.waiting = FALSE;
    if (!strncmp( line, "wait", 4 ))
    {
        consent.pause_left = atoi( line + 4 );
        if (consent.pause_left <= 0) consent.pause_left = 30;
        set_message( dialog, IDS_WAIT, consent.pause_left );
        SetTimer( dialog, 1, 1000, NULL );
    }
    else /* "wrong", and anything else the service may say */
    {
        set_message( dialog, IDS_WRONG, 0 );
        EnableWindow( GetDlgItem( dialog, IDC_ANSWER_YES ), TRUE );
    }
    SetDlgItemTextW( dialog, IDC_PASSWORD, L"" );
    SetFocus( GetDlgItem( dialog, consent.password_needed ? IDC_PASSWORD : IDC_ANSWER_YES ) );
}

static void fill( HWND dialog )
{
    HICON shield;

    SetWindowTextW( dialog, load( IDS_TITLE ) );
    SetDlgItemTextW( dialog, IDC_HEADING, load( IDS_HEADING ) );
    SetDlgItemTextW( dialog, IDC_PROGRAM_LABEL, load( IDS_PROGRAM ) );
    SetDlgItemTextW( dialog, IDC_PROGRAM, consent.program );
    SetDlgItemTextW( dialog, IDC_PUBLISHER_LABEL, load( IDS_PUBLISHER ) );
    SetDlgItemTextW( dialog, IDC_PUBLISHER, load( IDS_PUBLISHER_NAME ) );
    SetDlgItemTextW( dialog, IDC_REQUESTER_LABEL, load( IDS_REQUESTER ) );
    SetDlgItemTextW( dialog, IDC_REQUESTER, consent.source );
    SetDlgItemTextW( dialog, IDC_LOCATION_LABEL, load( IDS_LOCATION ) );
    SetDlgItemTextW( dialog, IDC_LOCATION, consent.path );
    SetDlgItemTextW( dialog, IDC_COMMAND_LABEL, load( IDS_COMMAND ) );
    SetDlgItemTextW( dialog, IDC_COMMAND, consent.command );
    SetDlgItemTextW( dialog, IDC_DETAILS, load( IDS_SHOW_DETAILS ) );
    SetDlgItemTextW( dialog, IDC_PASSWORD_PROMPT, load( IDS_PASSWORD_PROMPT ) );
    SetDlgItemTextW( dialog, IDC_ANSWER_YES, load( IDS_YES ) );
    SetDlgItemTextW( dialog, IDC_ANSWER_NO, load( IDS_NO ) );
    SetDlgItemTextW( dialog, IDC_MESSAGE, L"" );

    if (!(shield = LoadImageW( NULL, (const WCHAR *)IDI_SHIELD, IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE )))
        shield = LoadIconW( NULL, (const WCHAR *)IDI_WARNING );
    if (shield) SendDlgItemMessageW( dialog, IDC_SHIELD, STM_SETICON, (WPARAM)shield, 0 );
    SendDlgItemMessageW( dialog, IDC_PASSWORD, EM_SETCUEBANNER, 0, (LPARAM)load( IDS_PASSWORD_CUE ) );
}

static INT_PTR CALLBACK consent_proc( HWND dialog, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        consent.hwnd = dialog;
        asked_from = GetForegroundWindow();
        fill( dialog );
        relayout( dialog );
        raise_band( dialog );
        SetForegroundWindow( dialog );
        play_sound();
        CreateThread( NULL, 0, reader_thread, NULL, 0, NULL );
        if (consent.password_needed)
        {
            SetFocus( GetDlgItem( dialog, IDC_PASSWORD ) );
            return FALSE;   /* the focus is ours */
        }
        return TRUE;
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID( (HWND)lp ) == IDC_MESSAGE)
        {
            SetTextColor( (HDC)wp, ERROR_COLOR );
            SetBkMode( (HDC)wp, TRANSPARENT );
            if (!consent.background) consent.background = GetSysColorBrush( COLOR_BTNFACE );
            return (INT_PTR)consent.background;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_ANSWER_YES:
        case IDOK:
            send_answer( dialog, TRUE );
            return TRUE;
        case IDC_ANSWER_NO:
        case IDCANCEL:
            send_answer( dialog, FALSE );
            return TRUE;
        case IDC_DETAILS:
            consent.details = !consent.details;
            SetDlgItemTextW( dialog, IDC_DETAILS, load( consent.details ? IDS_HIDE_DETAILS : IDS_SHOW_DETAILS ) );
            relayout( dialog );
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (--consent.pause_left > 0) set_message( dialog, IDS_WAIT, consent.pause_left );
        else
        {
            KillTimer( dialog, 1 );
            set_message( dialog, 0, 0 );
            EnableWindow( GetDlgItem( dialog, IDC_ANSWER_YES ), TRUE );
            SetFocus( GetDlgItem( dialog, consent.password_needed ? IDC_PASSWORD : IDC_ANSWER_YES ) );
        }
        return TRUE;
    case WM_SERVER_LINE:
    {
        char *line = (char *)wp;

        server_line( dialog, line );
        free( line );
        return TRUE;
    }
    case WM_CLOSE:
        send_answer( dialog, FALSE );
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    InitCommonControls();   /* the visual style of the system, as the manifest asks */
    server_in = GetStdHandle( STD_INPUT_HANDLE );
    server_out = GetStdHandle( STD_OUTPUT_HANDLE );
    if (!take_prompt()) return 1;

    DialogBoxParamW( instance, (const WCHAR *)MAKEINTRESOURCE(IDD_CONSENT), NULL, consent_proc, 0 );
    answer( "no" );
    return 1;
}
