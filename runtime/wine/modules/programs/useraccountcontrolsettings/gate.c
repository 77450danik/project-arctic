/*
 * The Linux gate's settings, over the protocol of arctic-lxss
 *
 * The same frames wsl.exe speaks (runtime/wine/modules/programs/wsl/main.c),
 * with REQ_GATE and the words "status", "password" and "require". A request
 * is one connection, as every wsl.exe command is.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gate.h"

/* the protocol, shared with host/init/arctic-lxss.c */
#define MAGIC "ARCTLXS1"
#define TOKEN_LEN 32
enum { F_STDIN, F_STDIN_EOF, F_STDOUT, F_STDERR, F_EXIT, F_ERROR, F_SIGNAL, F_DENIED };
#define REQ_GATE 8

static int send_all( SOCKET sock, const void *data, int len )
{
    const char *p = data;

    while (len > 0)
    {
        int n = send( sock, p, len, 0 );

        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all( SOCKET sock, void *data, int len )
{
    char *p = data;

    while (len > 0)
    {
        int n = recv( sock, p, len, 0 );

        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void put32( char *at, unsigned int v )
{
    at[0] = v & 0xff;
    at[1] = (v >> 8) & 0xff;
    at[2] = (v >> 16) & 0xff;
    at[3] = (v >> 24) & 0xff;
}

/* C:\ProgramData\Arctic\Lxss\session: "port token" */
static SOCKET connect_service( char *token )
{
    struct sockaddr_in addr = { 0 };
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    unsigned int port = 0;
    SOCKET sock;
    WSADATA wsa;
    FILE *f;

    if (!GetEnvironmentVariableW( L"ProgramData", dir, ARRAY_SIZE(dir) )) wcscpy( dir, L"C:\\ProgramData" );
    swprintf( path, ARRAY_SIZE(path), L"%s\\Arctic\\Lxss\\session", dir );
    if (!(f = _wfopen( path, L"rb" ))) return INVALID_SOCKET;
    if (fscanf( f, "%u %32s", &port, token ) != 2 || !port)
    {
        fclose( f );
        return INVALID_SOCKET;
    }
    fclose( f );

    WSAStartup( MAKEWORD(2, 2), &wsa );
    sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( (unsigned short)port );
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        closesocket( sock );
        return INVALID_SOCKET;
    }
    return sock;
}

/* One request: the words of it, and what it wrote back (UTF-8) */
static enum gate_result gate_call( const char *const *words, unsigned int count, char *reply, size_t reply_size )
{
    char head[8 + TOKEN_LEN + 12 + 12], token[TOKEN_LEN + 1] = "", data[4096];
    enum gate_result result = GATE_ERROR;
    size_t got = 0;
    SOCKET sock;

    if (reply && reply_size) reply[0] = 0;
    if ((sock = connect_service( token )) == INVALID_SOCKET) return GATE_ERROR;

    memcpy( head, MAGIC, 8 );
    memcpy( head + 8, token, TOKEN_LEN );
    put32( head + 8 + TOKEN_LEN, REQ_GATE );
    put32( head + 12 + TOKEN_LEN, count );   /* the words */
    put32( head + 16 + TOKEN_LEN, 0 );       /* no environment */
    put32( head + 20 + TOKEN_LEN, 0 );       /* the distribution: none */
    put32( head + 24 + TOKEN_LEN, 0 );       /* the user: none */
    put32( head + 28 + TOKEN_LEN, 0 );       /* the folder: none */
    if (send_all( sock, head, sizeof(head) )) goto done;
    for (unsigned int i = 0; i < count; i++)
    {
        char length[4];

        put32( length, (unsigned int)strlen( words[i] ) );
        if (send_all( sock, length, 4 ) || send_all( sock, words[i], (int)strlen( words[i] ) )) goto done;
    }

    for (;;)
    {
        unsigned char frame[5];
        unsigned int len;

        if (recv_all( sock, frame, 5 )) break;
        len = frame[1] | frame[2] << 8 | frame[3] << 16 | (unsigned int)frame[4] << 24;
        if (len > sizeof(data) || (len && recv_all( sock, data, (int)len ))) break;
        if (frame[0] == F_STDOUT || frame[0] == F_ERROR)
        {
            if (reply && got + len < reply_size)
            {
                memcpy( reply + got, data, len );
                got += len;
                reply[got] = 0;
            }
            if (frame[0] == F_ERROR) break;
        }
        else if (frame[0] == F_DENIED)
        {
            result = GATE_DENIED;
            break;
        }
        else if (frame[0] == F_EXIT)
        {
            result = len >= 4 && !data[0] && !data[1] ? GATE_OK : GATE_ERROR;
            break;
        }
    }

done:
    closesocket( sock );
    return result;
}

enum gate_result gate_password_required( BOOL *required )
{
    static const char *const words[] = { "status" };
    char reply[256];
    enum gate_result result = gate_call( words, 1, reply, sizeof(reply) );

    if (result == GATE_OK) *required = !strncmp( reply, "password=on", 11 );
    return result;
}

enum gate_result gate_set_password( const WCHAR *password )
{
    const char *words[2] = { "password", NULL };
    char utf8[512];
    enum gate_result result;

    if (!WideCharToMultiByte( CP_UTF8, 0, password, -1, utf8, sizeof(utf8), NULL, NULL )) return GATE_ERROR;
    words[1] = utf8;
    result = gate_call( words, 2, NULL, 0 );
    SecureZeroMemory( utf8, sizeof(utf8) );
    return result;
}

enum gate_result gate_require_password( BOOL required )
{
    const char *words[2] = { "require", NULL };

    words[1] = required ? "on" : "off";
    return gate_call( words, 2, NULL, 0 );
}
