/*
 * Arctic web engine: the display list
 *
 * Everything the engine draws goes through this list: the layout fills it,
 * the rasterizer reads it, and nothing of the document is left in it. That
 * split is what lets the rasterizer move from the processor to the GPU
 * without the layout knowing (docs/browser.md).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "arctichtml_private.h"

void display_list_reset( struct display_list *list )
{
    for (unsigned int i = 0; i < list->string_count; i++) free( list->strings[i] );
    list->string_count = 0;
    list->count = 0;
    list->background = 0xffffffff;
    list->content_height = 0;
}

void display_list_free( struct display_list *list )
{
    display_list_reset( list );
    free( list->items );
    free( list->strings );
    memset( list, 0, sizeof(*list) );
}

struct item *display_list_add( struct display_list *list, enum item_type type )
{
    struct item *item;

    if (list->count == list->capacity)
    {
        unsigned int capacity = list->capacity ? list->capacity * 2 : 256;
        struct item *items = realloc( list->items, capacity * sizeof(*items) );

        if (!items) return NULL;
        list->items = items;
        list->capacity = capacity;
    }
    item = &list->items[list->count++];
    memset( item, 0, sizeof(*item) );
    item->type = type;
    return item;
}

/* The list keeps its own copy of every string: the document it came from
 * can be gone by the time the window is painted again. */
const WCHAR *display_list_string( struct display_list *list, const WCHAR *text, int length )
{
    WCHAR *copy;

    if (length < 0) length = (int)wcslen( text );
    if (list->string_count == list->string_capacity)
    {
        unsigned int capacity = list->string_capacity ? list->string_capacity * 2 : 256;
        WCHAR **strings = realloc( list->strings, capacity * sizeof(*strings) );

        if (!strings) return L"";
        list->strings = strings;
        list->string_capacity = capacity;
    }
    if (!(copy = malloc( (length + 1) * sizeof(WCHAR) ))) return L"";
    memcpy( copy, text, length * sizeof(WCHAR) );
    copy[length] = 0;
    list->strings[list->string_count++] = copy;
    return copy;
}

WCHAR *utf8_to_wide( const char *text, size_t len, int *out_len )
{
    int count = len ? MultiByteToWideChar( CP_UTF8, 0, text, (int)len, NULL, 0 ) : 0;
    WCHAR *wide = malloc( (count + 1) * sizeof(WCHAR) );

    if (!wide) return NULL;
    if (count) MultiByteToWideChar( CP_UTF8, 0, text, (int)len, wide, count );
    wide[count] = 0;
    if (out_len) *out_len = count;
    return wide;
}
