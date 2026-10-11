/*
 * appinfo.dll: the Application Information service (Appinfo)
 *
 * The broker of User Account Control, as Windows has it: it is the one
 * thing that puts the consent window on the screen, one at a time, and it
 * is a service so that the rest of elevation (RAiLaunchAdminProcess for
 * runas and requireAdministrator programs) can grow here later.
 *
 * What it brokers today is the Linux subsystem (docs/linux-gate.md). The
 * host's arctic-lxss runs as root and will not start a window itself; it
 * asks this service over 127.0.0.1, and the service shows consent.exe and
 * carries the lines between them. The service never sees the gate's
 * password file and never decides anything: root checks the password and
 * root records the answer. A program that connects here on its own can
 * only make a window appear, which is also what calling Windows' own
 * elevation API gets you.
 *
 *   C:\ProgramData\Arctic\Appinfo\session   "port token", written at start
 *
 *   client -> token
 *   client -> prompt <0|1 password needed> <program> <path> <what is asked>
 *   server -> yes <password>  |  no
 *   client -> ok  |  wrong  |  wait <seconds>      (until ok, or either end goes)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#include <ntsecapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appinfo);

#define TOKEN_LEN 32
/* as in Windows: one consent window at a time, and it waits for nobody for
 * longer than the host's own patience (docs/linux-gate.md) */
#define ONE_WINDOW_AT_A_TIME L"Global\\ArcticAppinfoConsent"

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status = { SERVICE_WIN32_SHARE_PROCESS, SERVICE_STOPPED };
static SOCKET listener = INVALID_SOCKET;
static HANDLE one_window;
static char token[TOKEN_LEN + 1];

static void set_state( DWORD state )
{
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    if (status_handle) SetServiceStatus( status_handle, &status );
}

/* ---- lines ---- */

static BOOL send_line( SOCKET sock, const char *line )
{
    int len = (int)strlen( line ), sent = 0;
    char end = '\n';

    while (sent < len)
    {
        int n = send( sock, line + sent, len - sent, 0 );

        if (n <= 0) return FALSE;
        sent += n;
    }
    return send( sock, &end, 1, 0 ) == 1;
}

static BOOL read_line( SOCKET sock, char *buffer, size_t size )
{
    size_t len = 0;

    while (len + 1 < size)
    {
        char c;

        if (recv( sock, &c, 1, 0 ) != 1) return FALSE;
        if (c == '\n')
        {
            buffer[len] = 0;
            return TRUE;
        }
        buffer[len++] = c;
    }
    return FALSE;
}

static BOOL write_pipe( HANDLE pipe, const char *line )
{
    DWORD written;

    return WriteFile( pipe, line, (DWORD)strlen( line ), &written, NULL ) &&
           WriteFile( pipe, "\n", 1, &written, NULL );
}

static BOOL read_pipe( HANDLE pipe, char *buffer, size_t size )
{
    size_t len = 0;

    while (len + 1 < size)
    {
        DWORD got;

        if (!ReadFile( pipe, buffer + len, 1, &got, NULL ) || !got) return FALSE;
        if (buffer[len] == '\n')
        {
            buffer[len] = 0;
            return TRUE;
        }
        len++;
    }
    return FALSE;
}

/* ---- the consent window ---- */

struct window
{
    HANDLE process, to_window, from_window;
};

/* The environment of the window, with the one override that makes Wine take
 * consent.exe from the host's read-only image whatever lies on C: */
static WCHAR *window_environment( void )
{
    WCHAR *block = GetEnvironmentStringsW(), *out, *at;
    size_t size = 1;
    static const WCHAR override[] = L"WINEDLLOVERRIDES=consent.exe=b";

    for (WCHAR *e = block; *e; e += wcslen( e ) + 1)
        if (_wcsnicmp( e, L"WINEDLLOVERRIDES=", 17 )) size += wcslen( e ) + 1;
    size += ARRAY_SIZE(override);
    out = malloc( size * sizeof(WCHAR) );
    at = out;
    for (WCHAR *e = block; *e; e += wcslen( e ) + 1)
        if (_wcsnicmp( e, L"WINEDLLOVERRIDES=", 17 ))
        {
            wcscpy( at, e );
            at += wcslen( at ) + 1;
        }
    wcscpy( at, override );
    at += wcslen( at ) + 1;
    *at = 0;
    FreeEnvironmentStringsW( block );
    return out;
}

static BOOL start_window( struct window *window )
{
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    WCHAR path[MAX_PATH], *environment;
    BOOL started;

    memset( window, 0, sizeof(*window) );
    if (!CreatePipe( &in_read, &in_write, &inherit, 0 ) || !CreatePipe( &out_read, &out_write, &inherit, 0 ))
        goto failed;
    SetHandleInformation( in_write, HANDLE_FLAG_INHERIT, 0 );
    SetHandleInformation( out_read, HANDLE_FLAG_INHERIT, 0 );

    GetSystemDirectoryW( path, ARRAY_SIZE(path) );
    wcscat( path, L"\\consent.exe" );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle( STD_ERROR_HANDLE );
    environment = window_environment();
    started = CreateProcessW( path, NULL, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, environment, NULL, &si, &pi );
    free( environment );
    CloseHandle( in_read );
    CloseHandle( out_write );
    if (!started)
    {
        ERR( "the consent window (%s) did not start: %lu\n", debugstr_w(path), GetLastError() );
        CloseHandle( in_write );
        CloseHandle( out_read );
        return FALSE;
    }
    CloseHandle( pi.hThread );
    window->process = pi.hProcess;
    window->to_window = in_write;
    window->from_window = out_read;
    return TRUE;

failed:
    if (in_read) CloseHandle( in_read );
    if (in_write) CloseHandle( in_write );
    if (out_read) CloseHandle( out_read );
    if (out_write) CloseHandle( out_write );
    return FALSE;
}

static void end_window( struct window *window )
{
    if (window->to_window) CloseHandle( window->to_window );
    if (window->from_window) CloseHandle( window->from_window );
    if (window->process)
    {
        if (WaitForSingleObject( window->process, 5000 ) != WAIT_OBJECT_0) TerminateProcess( window->process, 1 );
        CloseHandle( window->process );
    }
}

/* One request: the window goes up, and the lines pass each way until the
 * host says "ok", the window is answered "no", or either end goes away. */
static void serve_client( SOCKET sock )
{
    char line[4096];
    struct window window;

    if (!read_line( sock, line, sizeof(line) ) || strcmp( line, token )) return;
    if (!read_line( sock, line, sizeof(line) ) || strncmp( line, "prompt\t", 7 )) return;

    /* every other request waits here, as Windows shows one window at a time */
    if (one_window) WaitForSingleObject( one_window, INFINITE );
    if (!start_window( &window ))
    {
        send_line( sock, "no" );
        if (one_window) ReleaseMutex( one_window );
        return;
    }
    if (write_pipe( window.to_window, line ))
        for (;;)
        {
            if (!read_pipe( window.from_window, line, sizeof(line) )) /* the window is gone: as good as no */
            {
                send_line( sock, "no" );
                break;
            }
            if (!send_line( sock, line )) break;                      /* the host is gone */
            if (!strcmp( line, "no" )) break;
            if (!read_line( sock, line, sizeof(line) )) break;
            if (!write_pipe( window.to_window, line )) break;
            if (!strcmp( line, "ok" )) break;
        }
    end_window( &window );
    if (one_window) ReleaseMutex( one_window );
}

static DWORD WINAPI client_thread( void *arg )
{
    SOCKET sock = (SOCKET)(ULONG_PTR)arg;

    serve_client( sock );
    closesocket( sock );
    return 0;
}

/* ---- the service ---- */

/* where the host looks for this service */
static void write_session_file( unsigned short port )
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    char text[64];
    DWORD written;
    HANDLE file;

    if (!GetEnvironmentVariableW( L"ProgramData", dir, ARRAY_SIZE(dir) )) wcscpy( dir, L"C:\\ProgramData" );
    wcscat( dir, L"\\Arctic" );
    CreateDirectoryW( dir, NULL );
    wcscat( dir, L"\\Appinfo" );
    CreateDirectoryW( dir, NULL );
    swprintf( path, ARRAY_SIZE(path), L"%s\\session", dir );
    file = CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        ERR( "%s: %lu\n", debugstr_w(path), GetLastError() );
        return;
    }
    snprintf( text, sizeof(text), "%u %s\n", port, token );
    WriteFile( file, text, (DWORD)strlen( text ), &written, NULL );
    CloseHandle( file );
}

static BOOL start_listening( void )
{
    struct sockaddr_in address = { 0 };
    unsigned char random_bytes[TOKEN_LEN / 2];
    int length = sizeof(address);
    WSADATA wsa;

    if (WSAStartup( MAKEWORD(2, 2), &wsa )) return FALSE;
    if (!RtlGenRandom( random_bytes, sizeof(random_bytes) )) return FALSE;
    for (int i = 0; i < TOKEN_LEN / 2; i++) snprintf( token + 2 * i, 3, "%02x", random_bytes[i] );

    listener = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if (listener == INVALID_SOCKET || bind( listener, (struct sockaddr *)&address, sizeof(address) ) ||
        listen( listener, 8 ) || getsockname( listener, (struct sockaddr *)&address, &length ))
    {
        ERR( "the service could not listen: %d\n", WSAGetLastError() );
        return FALSE;
    }
    one_window = CreateMutexW( NULL, FALSE, ONE_WINDOW_AT_A_TIME );
    write_session_file( ntohs( address.sin_port ) );
    TRACE( "listening on 127.0.0.1:%u\n", ntohs( address.sin_port ) );
    return TRUE;
}

static DWORD WINAPI control_handler( DWORD control, DWORD type, void *data, void *context )
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    {
        SOCKET sock = listener;

        set_state( SERVICE_STOP_PENDING );
        listener = INVALID_SOCKET;
        if (sock != INVALID_SOCKET) closesocket( sock ); /* the accept loop ends with it */
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain( DWORD argc, WCHAR *argv[] )
{
    status_handle = RegisterServiceCtrlHandlerExW( L"Appinfo", control_handler, NULL );
    if (!status_handle) return;
    set_state( SERVICE_START_PENDING );
    if (!start_listening())
    {
        status.dwWin32ExitCode = ERROR_SERVICE_START_HANG;
        set_state( SERVICE_STOPPED );
        return;
    }
    set_state( SERVICE_RUNNING );

    for (;;)
    {
        SOCKET sock = accept( listener, NULL, NULL );
        HANDLE thread;

        if (sock == INVALID_SOCKET) break; /* the service is stopping */
        if ((thread = CreateThread( NULL, 0, client_thread, (void *)(ULONG_PTR)sock, 0, NULL ))) CloseHandle( thread );
        else closesocket( sock );
    }
    if (one_window) CloseHandle( one_window );
    set_state( SERVICE_STOPPED );
}

void WINAPI SvchostPushServiceGlobals( void *globals )
{
}
