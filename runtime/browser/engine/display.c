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
    list->link_count = 0;
    list->anchor_count = 0;
    list->control_count = 0;
    list->layer = 0;
    list->absolute = 0;
    list->background = 0xffffffff;
    list->content_height = 0;
}

void display_list_free( struct display_list *list )
{
    display_list_reset( list );
    free( list->items );
    free( list->strings );
    free( list->links );
    free( list->anchors );
    free( list->controls );
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
    item->layer = list->layer;
    item->absolute = list->absolute;
    item->owner = list->owner;
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

void display_list_link( struct display_list *list, const RECT *rect, lxb_dom_element_t *element )
{
    struct link_area *area;

    /* the words of one link on one line touch: one area for them */
    if (list->link_count)
    {
        area = &list->links[list->link_count - 1];
        if (area->element == element && area->rect.top == rect->top && area->rect.bottom == rect->bottom &&
            area->rect.right >= rect->left - 1)
        {
            area->rect.right = max( area->rect.right, rect->right );
            return;
        }
    }
    if (list->link_count == list->link_capacity)
    {
        unsigned int capacity = list->link_capacity ? list->link_capacity * 2 : 64;
        struct link_area *grown = realloc( list->links, capacity * sizeof(*grown) );

        if (!grown) return;
        list->links = grown;
        list->link_capacity = capacity;
    }
    area = &list->links[list->link_count++];
    area->rect = *rect;
    area->element = element;
}

void display_list_anchor( struct display_list *list, int y, lxb_dom_element_t *element )
{
    if (list->anchor_count == list->anchor_capacity)
    {
        unsigned int capacity = list->anchor_capacity ? list->anchor_capacity * 2 : 64;
        struct anchor *grown = realloc( list->anchors, capacity * sizeof(*grown) );

        if (!grown) return;
        list->anchors = grown;
        list->anchor_capacity = capacity;
    }
    list->anchors[list->anchor_count].y = y;
    list->anchors[list->anchor_count++].element = element;
}

struct control_area *display_list_control( struct display_list *list )
{
    struct control_area *control;

    if (list->control_count == list->control_capacity)
    {
        unsigned int capacity = list->control_capacity ? list->control_capacity * 2 : 32;
        struct control_area *grown = realloc( list->controls, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        list->controls = grown;
        list->control_capacity = capacity;
    }
    control = &list->controls[list->control_count++];
    memset( control, 0, sizeof(*control) );
    return control;
}

void display_list_mark( const struct display_list *list, struct list_mark *mark )
{
    mark->items = list->count;
    mark->links = list->link_count;
    mark->anchors = list->anchor_count;
    mark->controls = list->control_count;
}

void display_list_shift( struct display_list *list, const struct list_mark *from, const struct list_mark *to,
                         int dx, int dy )
{
    for (unsigned int i = from->items; i < to->items; i++)
    {
        struct item *item = &list->items[i];

        OffsetRect( &item->rect, dx, dy );
        OffsetRect( &item->clip, dx, dy );
        item->x += dx;
        item->baseline += dy;
    }
    for (unsigned int i = from->links; i < to->links; i++) OffsetRect( &list->links[i].rect, dx, dy );
    for (unsigned int i = from->anchors; i < to->anchors; i++) list->anchors[i].y += dy;
    for (unsigned int i = from->controls; i < to->controls; i++)
    {
        OffsetRect( &list->controls[i].rect, dx, dy );
        list->controls[i].text_x += dx;
    }
}

lxb_dom_element_t *display_list_hit( const struct display_list *list, int x, int y )
{
    POINT point = { x, y };

    /* what is drawn over the flow first, then the flow; the last drawn of each on top */
    for (int layer = 1; layer >= 0; layer--)
        for (unsigned int i = list->count; i--; )
        {
            const struct item *item = &list->items[i];

            if (item->layer != layer || !item->owner || !PtInRect( &item->rect, point )) continue;
            if (item->clipped && !PtInRect( &item->clip, point )) continue;
            return item->owner;
        }
    return NULL;
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
