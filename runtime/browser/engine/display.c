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
    list->background = RGB(255, 255, 255);
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
        unsigned int capacity = list->capacity ? list->capacity * 2 : 32;
        struct item *items = realloc( list->items, capacity * sizeof(*items) );

        if (!items) return NULL;
        list->items = items;
        list->capacity = capacity;
    }
    item = &list->items[list->count++];
    memset( item, 0, sizeof(*item) );
    item->type = type;
    item->size = 11;
    item->font = FONT_BODY;
    return item;
}

/* The list keeps its own copy of every string: the document it came from
 * can be gone by the time the window is painted again. */
const WCHAR *display_list_string( struct display_list *list, const WCHAR *text )
{
    WCHAR *copy;
    size_t size;

    if (list->string_count == list->string_capacity)
    {
        unsigned int capacity = list->string_capacity ? list->string_capacity * 2 : 32;
        WCHAR **strings = realloc( list->strings, capacity * sizeof(*strings) );

        if (!strings) return L"";
        list->strings = strings;
        list->string_capacity = capacity;
    }
    size = (wcslen( text ) + 1) * sizeof(WCHAR);
    if (!(copy = malloc( size ))) return L"";
    memcpy( copy, text, size );
    list->strings[list->string_count++] = copy;
    return copy;
}

static struct item *add_text( struct display_list *list, const WCHAR *text, int x, int y, int width,
                              int height, enum item_font font, int size, COLORREF color )
{
    struct item *item = display_list_add( list, ITEM_TEXT );

    if (!item) return NULL;
    SetRect( &item->rect, x, y, x + width, y + height );
    item->text = display_list_string( list, text );
    item->font = font;
    item->size = size;
    item->color = color;
    item->format = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
    return item;
}

static void add_rect( struct display_list *list, int x, int y, int width, int height, COLORREF color )
{
    struct item *item = display_list_add( list, ITEM_RECT );

    if (!item) return;
    SetRect( &item->rect, x, y, x + width, y + height );
    item->color = color;
}

/*
 * The page of stage 0. It is not an HTML document and there is no parser
 * yet: it is the display list written by hand, so that what the stage
 * proves is exactly the way from a display list to the screen. Stage 1
 * replaces this function with the parser, the cascade and the layout, and
 * the rest of the engine does not change.
 */
void build_start_page( struct display_list *list, int width, int height, const WCHAR *url )
{
    static const WCHAR *paragraphs[] =
    {
        L"Цю сторінку намалював рушій Arctic, а не Gecko і не Blink: розмітки "
        L"ще немає, є список відображення, растеризація й шлях на екран через "
        L"Vulkan. Етап 1 замінить цей список парсером, каскадом і версткою.",
        L"Вбудований Internet Explorer не відкривав навіть Google, бо в системі "
        L"не було рушія: Wine mshtml — лише обгортка над wine-gecko, якого в "
        L"образі немає. Тепер рушій свій.",
    };
    int x = 48, y = 40, content = width - 96;
    struct item *item;

    display_list_reset( list );
    list->width = width;
    list->height = height;
    list->background = RGB(255, 255, 255);

    if (content < 160) content = width > 32 ? width - 32 : width;

    /* the heading, in Arial as a browser's default sans-serif */
    add_text( list, L"Arctic", x, y, content, 64, FONT_HEADING, 32, RGB(0x1f, 0x3a, 0x5f) );
    y += 56;
    add_rect( list, x, y, content, 1, RGB(0xd0, 0xd7, 0xde) );
    y += 20;

    for (unsigned int i = 0; i < ARRAY_SIZE(paragraphs); i++)
    {
        add_text( list, paragraphs[i], x, y, content, 96, FONT_BODY, 12, RGB(0x1a, 0x1a, 0x1a) );
        y += 84;
    }

    /* the address, to show that what the frame typed reached the engine */
    add_rect( list, x, y, content, 64, RGB(0xf4, 0xf6, 0xf8) );
    add_rect( list, x, y, 3, 64, RGB(0x1f, 0x3a, 0x5f) );
    add_text( list, L"Адреса, яку дала рамка:", x + 16, y + 10, content - 32, 20, FONT_UI, 10,
              RGB(0x50, 0x5a, 0x64) );
    if ((item = add_text( list, url && *url ? url : L"about:blank", x + 16, y + 30, content - 32, 24,
                          FONT_MONO, 11, RGB(0x1a, 0x1a, 0x1a) )))
        item->format = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    y += 88;

    add_text( list, L"docs/browser.md — план рушія: етапи, донори коду, перевірка.",
              x, y, content, 24, FONT_UI, 10, RGB(0x50, 0x5a, 0x64) );

    TRACE( "%u items for %dx%d\n", list->count, width, height );
}
