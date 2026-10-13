/*
 * Arctic web engine: what the engine tells whoever is looking
 *
 * The engine is a plain Windows program that must run on any Windows, so it
 * has no Wine debug channels: messages go to OutputDebugString, which a
 * debugger, DebugView or Arctic's nt.log shows, and to a file when
 * ARCTIC_HTML_LOG names one. ARCTIC_HTML_TRACE=1 lets the TRACE lines out.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTICHTML_LOG_H
#define __ARCTICHTML_LOG_H

#include <windows.h>

enum log_level { LOG_TRACE, LOG_WARN, LOG_ERR };

void log_message( enum log_level level, const char *function, const char *format, ... )
    __attribute__((format(printf, 3, 4)));
BOOL log_tracing(void);
/* a wide string, a rectangle, as text that stays valid until a few more calls */
const char *debugstr_w( const WCHAR *string );
const char *debugstr_rect( const RECT *rect );

#define TRACE(...) do { if (log_tracing()) log_message( LOG_TRACE, __func__, __VA_ARGS__ ); } while (0)
#define WARN(...)  log_message( LOG_WARN, __func__, __VA_ARGS__ )
#define ERR(...)   log_message( LOG_ERR, __func__, __VA_ARGS__ )

#endif /* __ARCTICHTML_LOG_H */
