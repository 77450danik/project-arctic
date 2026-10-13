/*
 * Arctic web engine: the log
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>

#include "arctichtml_private.h"

static INIT_ONCE log_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION log_section;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static BOOL tracing;

static BOOL CALLBACK log_init( INIT_ONCE *once, void *param, void **context )
{
    WCHAR path[MAX_PATH], value[8];

    InitializeCriticalSection( &log_section );
    if (GetEnvironmentVariableW( L"ARCTIC_HTML_TRACE", value, ARRAY_SIZE(value) ) && value[0] != '0')
        tracing = TRUE;
    if (GetEnvironmentVariableW( L"ARCTIC_HTML_LOG", path, ARRAY_SIZE(path) ))
        log_file = CreateFileW( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    return TRUE;
}

BOOL log_tracing(void)
{
    InitOnceExecuteOnce( &log_once, log_init, NULL, NULL );
    return tracing;
}

void log_message( enum log_level level, const char *function, const char *format, ... )
{
    static const char *const names[] = { "trace", "warn", "err" };
    char line[1024];
    va_list args;
    int len;

    InitOnceExecuteOnce( &log_once, log_init, NULL, NULL );

    len = snprintf( line, sizeof(line), "%04lx:arctichtml:%s:%s ", GetCurrentThreadId(), names[level], function );
    if (len < 0 || len >= (int)sizeof(line)) len = 0;
    va_start( args, format );
    vsnprintf( line + len, sizeof(line) - len, format, args );
    va_end( args );

    EnterCriticalSection( &log_section );
    OutputDebugStringA( line );
    if (log_file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile( log_file, line, (DWORD)strlen( line ), &written, NULL );
    }
    LeaveCriticalSection( &log_section );
}

/* a ring of buffers, so a message can show a few strings at once */
static char *next_buffer(void)
{
    static char buffers[8][512];
    static LONG next;

    return buffers[(unsigned)InterlockedIncrement( &next ) % ARRAY_SIZE(buffers)];
}

const char *debugstr_w( const WCHAR *string )
{
    char *buffer = next_buffer();
    int len;

    if (!string) return "(null)";
    buffer[0] = '"';
    len = WideCharToMultiByte( CP_UTF8, 0, string, -1, buffer + 1, 512 - 3, NULL, NULL );
    if (!len) len = 1;  /* an empty or unconvertible string: "" */
    buffer[len] = '"';
    buffer[len + 1] = 0;
    return buffer;
}

const char *debugstr_rect( const RECT *rect )
{
    char *buffer = next_buffer();

    if (!rect) return "(null)";
    snprintf( buffer, 512, "(%ld,%ld)-(%ld,%ld)", rect->left, rect->top, rect->right, rect->bottom );
    return buffer;
}
