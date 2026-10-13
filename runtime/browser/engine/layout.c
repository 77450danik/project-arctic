/*
 * Arctic web engine: layout
 *
 * Stage 1 of docs/browser.md: normal flow. Blocks stack down their
 * container, the margins of neighbours collapse into the larger one, and the
 * inline content between blocks breaks into lines at spaces. Every line is
 * as tall as the tallest thing on it with the block's own font as a strut,
 * so an empty line after <br> still has a height.
 *
 * Inline content is a list of atoms - a word, a space, a forced break, an
 * image - and the line breaker places atoms; neighbours of one style on one
 * line go out as one run of text. Floats, positioning, flexbox and tables
 * are later stages: until then they flow like blocks.
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
#include <math.h>

#include "arctichtml_private.h"

extern float style_viewport_width, style_viewport_height;

enum atom_type { ATOM_TEXT, ATOM_SPACE, ATOM_BREAK, ATOM_IMAGE };

struct atom
{
    enum atom_type      type;
    const struct style *style;
    const struct font  *font;
    size_t              text;       /* offset into the layout's text */
    int                 length;
    int                 width, height;  /* height: images only */
    BOOL                keep;       /* a space that does not collapse or break (pre, nowrap);
                                     * a break of <br> or a newline of pre, which ends even an empty line */
    const struct image *image;
};

struct layout
{
    struct document     *doc;
    struct display_list *list;
    struct style       **styles;    /* every computed style, freed at the end */
    unsigned int         style_count, style_capacity;
    struct atom         *atoms;     /* the inline content being gathered */
    unsigned int         atom_count, atom_capacity;
    WCHAR               *text;
    size_t               text_len, text_capacity;
};

static struct style *new_style( struct layout *ctx )
{
    struct style *style;

    if (ctx->style_count == ctx->style_capacity)
    {
        unsigned int capacity = ctx->style_capacity ? ctx->style_capacity * 2 : 256;
        struct style **grown = realloc( ctx->styles, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        ctx->styles = grown;
        ctx->style_capacity = capacity;
    }
    if (!(style = malloc( sizeof(*style) ))) return NULL;
    ctx->styles[ctx->style_count++] = style;
    return style;
}

static const struct style *compute( struct layout *ctx, const struct style *parent, lxb_dom_element_t *element )
{
    struct style *style = new_style( ctx );

    if (!style) return NULL;
    style_compute( parent, element, style );
    return style;
}

static BOOL is_block( const struct style *style )
{
    return style->display == DISPLAY_BLOCK || style->display == DISPLAY_LIST_ITEM;
}

static float line_height_of( const struct style *style, const struct font *font )
{
    return style->line_height > 0 ? style->line_height : (float)(font->ascent + font->descent);
}

/* --- emitting --- */

static void emit_rect( struct layout *ctx, float left, float top, float right, float bottom, DWORD color )
{
    struct item *item;

    if (!(color >> 24) || right <= left || bottom <= top) return;
    if (!(item = display_list_add( ctx->list, ITEM_RECT ))) return;
    SetRect( &item->rect, (int)floorf( left ), (int)floorf( top ), (int)ceilf( right ), (int)ceilf( bottom ) );
    item->color = color;
}

/* --- gathering inline content --- */

static struct atom *add_atom( struct layout *ctx, enum atom_type type, const struct style *style )
{
    struct atom *atom;

    if (ctx->atom_count == ctx->atom_capacity)
    {
        unsigned int capacity = ctx->atom_capacity ? ctx->atom_capacity * 2 : 512;
        struct atom *grown = realloc( ctx->atoms, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        ctx->atoms = grown;
        ctx->atom_capacity = capacity;
    }
    atom = &ctx->atoms[ctx->atom_count++];
    memset( atom, 0, sizeof(*atom) );
    atom->type = type;
    atom->style = style;
    atom->font = font_get( &style->font );
    return atom;
}

static size_t keep_text( struct layout *ctx, const WCHAR *text, int length )
{
    size_t offset = ctx->text_len;

    if (ctx->text_len + length > ctx->text_capacity)
    {
        size_t capacity = max( ctx->text_capacity * 2, ctx->text_len + length + 4096 );
        WCHAR *grown = realloc( ctx->text, capacity * sizeof(WCHAR) );

        if (!grown) return offset;
        ctx->text = grown;
        ctx->text_capacity = capacity;
    }
    memcpy( ctx->text + ctx->text_len, text, length * sizeof(WCHAR) );
    ctx->text_len += length;
    return offset;
}

static void add_word( struct layout *ctx, const struct style *style, const WCHAR *text, int length )
{
    struct atom *atom;

    if (!length || !(atom = add_atom( ctx, ATOM_TEXT, style ))) return;
    atom->text = keep_text( ctx, text, length );
    atom->length = length;
    atom->width = font_measure( atom->font, text, length );
}

static void add_space( struct layout *ctx, const struct style *style, BOOL keep )
{
    struct atom *atom;

    /* spaces collapse into one, across the borders of elements too */
    if (!keep && ctx->atom_count && ctx->atoms[ctx->atom_count - 1].type == ATOM_SPACE &&
        !ctx->atoms[ctx->atom_count - 1].keep)
        return;
    if (!(atom = add_atom( ctx, ATOM_SPACE, style ))) return;
    atom->width = atom->font->space;
    atom->keep = keep;
}

/* hard: <br>, a newline of pre - it ends a line even an empty one; soft: the
 * edge of a block inside an inline, which only ends a line that has something */
static void add_break( struct layout *ctx, const struct style *style, BOOL hard )
{
    struct atom *atom = add_atom( ctx, ATOM_BREAK, style );

    if (atom) atom->keep = hard;
}

static BOOL is_space( WCHAR c )
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void add_text( struct layout *ctx, const struct style *style, const char *utf8, size_t utf8_len )
{
    BOOL pre = style->white_space == WS_PRE || style->white_space == WS_PRE_WRAP;
    BOOL lines = pre || style->white_space == WS_PRE_LINE;
    BOOL nowrap = style->white_space == WS_NOWRAP || style->white_space == WS_PRE;
    WCHAR *text;
    int len, start = 0;

    if (!(text = utf8_to_wide( utf8, utf8_len, &len ))) return;
    for (int i = 0; i <= len; i++)
    {
        WCHAR c = i < len ? text[i] : 0;

        if (i < len && !is_space( c )) continue;
        add_word( ctx, style, text + start, i - start );
        start = i + 1;
        if (i == len) break;
        if (c == '\r') continue;
        if (c == '\n' && lines) add_break( ctx, style, TRUE );
        else if (pre && c == '\t') for (int t = 0; t < 4; t++) add_space( ctx, style, TRUE );
        else add_space( ctx, style, pre || nowrap );
    }
    free( text );
}

static int attribute_number( lxb_dom_element_t *element, const char *name )
{
    size_t len;
    const char *value = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)name,
                                                                     strlen( name ), &len );
    return value && len ? atoi( value ) : 0;
}

static void add_image( struct layout *ctx, lxb_dom_element_t *element, const struct style *style )
{
    const struct image *image = NULL;
    const char *src, *alt;
    struct atom *atom;
    size_t len;
    float w, h;

    if ((src = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"src", 3, &len )))
        image = document_image( ctx->doc, src, len );
    if (!image)
    {
        /* what the page says the image is, in its place */
        if ((alt = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"alt", 3, &len )) && len)
            add_text( ctx, style, alt, len );
        return;
    }
    w = style->width.unit == LEN_PX ? style->width.value : (float)attribute_number( element, "width" );
    h = style->height.unit == LEN_PX ? style->height.value : (float)attribute_number( element, "height" );
    if (!w && !h) { w = (float)image->width; h = (float)image->height; }
    else if (!w) w = h * image->width / image->height;
    else if (!h) h = w * image->height / image->width;

    if (!(atom = add_atom( ctx, ATOM_IMAGE, style ))) return;
    atom->image = image;
    atom->width = (int)(w + 0.5f);
    atom->height = (int)(h + 0.5f);
}

static void gather_inline( struct layout *ctx, lxb_dom_node_t *node, const struct style *style );

static void gather_element( struct layout *ctx, lxb_dom_element_t *element, const struct style *parent )
{
    const struct style *style = compute( ctx, parent, element );

    if (!style || style->display == DISPLAY_NONE) return;
    switch (lxb_dom_element_tag_id( element ))
    {
    case LXB_TAG_BR:
        add_break( ctx, style, TRUE );
        return;
    case LXB_TAG_IMG:
        add_image( ctx, element, style );
        return;
    default:
        break;
    }
    /* a block inside an inline: its own lines, until blocks in inlines are split properly */
    if (is_block( style )) add_break( ctx, style, FALSE );
    gather_inline( ctx, lxb_dom_interface_node( element ), style );
    if (is_block( style )) add_break( ctx, style, FALSE );
}

static void gather_inline( struct layout *ctx, lxb_dom_node_t *node, const struct style *style )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT)
        {
            const lexbor_str_t *data = &lxb_dom_interface_character_data( node )->data;

            add_text( ctx, style, (const char *)data->data, data->length );
        }
        else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
            gather_element( ctx, lxb_dom_interface_element( node ), style );
    }
}

/* --- breaking into lines --- */

static float place_line( struct layout *ctx, unsigned int first, unsigned int end, float x, float top, float width,
                         const struct style *block )
{
    const struct font *strut = font_get( &block->font );
    float strut_half = (line_height_of( block, strut ) - (strut->ascent + strut->descent)) / 2;
    float ascent = strut->ascent + strut_half, descent = strut->descent + strut_half;
    float used = 0, pen, baseline;
    WCHAR *run = NULL;
    int run_capacity = 0;

    /* no space at either end of a line, unless it is kept */
    while (first < end && ctx->atoms[first].type == ATOM_SPACE && !ctx->atoms[first].keep) first++;
    while (end > first && ctx->atoms[end - 1].type == ATOM_SPACE && !ctx->atoms[end - 1].keep) end--;

    for (unsigned int i = first; i < end; i++)
    {
        const struct atom *atom = &ctx->atoms[i];

        used += atom->width;
        if (atom->type == ATOM_IMAGE)
        {
            ascent = max( ascent, (float)atom->height );
        }
        else if (atom->type == ATOM_TEXT)
        {
            float half = (line_height_of( atom->style, atom->font ) - (atom->font->ascent + atom->font->descent)) / 2;

            ascent = max( ascent, atom->font->ascent + half );
            descent = max( descent, atom->font->descent + half );
        }
    }
    baseline = top + ascent;

    switch (block->text_align)
    {
    case ALIGN_CENTER: pen = x + max( 0, (width - used) / 2 ); break;
    case ALIGN_RIGHT:  pen = x + max( 0, width - used ); break;
    default:           pen = x; break;
    }

    for (unsigned int i = first; i < end; )
    {
        const struct atom *atom = &ctx->atoms[i];
        struct item *item;

        if (atom->type == ATOM_IMAGE)
        {
            if ((item = display_list_add( ctx->list, ITEM_IMAGE )))
            {
                SetRect( &item->rect, (int)pen, (int)(baseline - atom->height), (int)pen + atom->width, (int)baseline );
                item->image = atom->image;
            }
            pen += atom->width;
            i++;
            continue;
        }
        if (atom->type == ATOM_BREAK) { i++; continue; }

        /* words and spaces of one style, one after another: one run */
        {
            const struct style *style = atom->style;
            unsigned int j = i;
            int length = 0, run_width = 0;

            while (j < end && ctx->atoms[j].style == style &&
                   (ctx->atoms[j].type == ATOM_TEXT || ctx->atoms[j].type == ATOM_SPACE))
            {
                length += ctx->atoms[j].type == ATOM_TEXT ? ctx->atoms[j].length : 1;
                j++;
            }
            if (length > run_capacity)
            {
                WCHAR *grown = realloc( run, length * sizeof(WCHAR) );

                if (!grown) break;
                run = grown;
                run_capacity = length;
            }
            length = 0;
            for (unsigned int k = i; k < j; k++)
            {
                const struct atom *a = &ctx->atoms[k];

                if (a->type == ATOM_TEXT)
                {
                    memcpy( run + length, ctx->text + a->text, a->length * sizeof(WCHAR) );
                    length += a->length;
                }
                else run[length++] = ' ';
            }
            /* measured whole: the widths of its words, rounded one by one, add up to more */
            run_width = font_measure( atom->font, run, length );
            emit_rect( ctx, pen, top, pen + run_width, top + ascent + descent, style->background );
            if ((item = display_list_add( ctx->list, ITEM_RUN )))
            {
                SetRect( &item->rect, (int)pen, (int)top, (int)ceilf( pen + run_width ), (int)ceilf( top + ascent + descent ) );
                item->text = display_list_string( ctx->list, run, length );
                item->length = length;
                item->font = atom->font;
                item->color = style->color;
                item->x = (int)(pen + 0.5f);
                item->baseline = (int)(baseline + 0.5f);
            }
            pen += run_width;
            i = j;
        }
    }
    free( run );
    return ascent + descent;
}

/* the gathered atoms, broken into lines from top down; returns their height */
static float flush_inline( struct layout *ctx, float x, float top, float width, const struct style *block )
{
    unsigned int start = 0, count = ctx->atom_count;
    float y = top, used = 0;
    BOOL content = FALSE;

    for (unsigned int i = 0; i < count; i++)
        if (ctx->atoms[i].type != ATOM_SPACE) { content = TRUE; break; }
    if (!content)
    {
        ctx->atom_count = 0;
        ctx->text_len = 0;
        return 0;
    }

    for (unsigned int i = 0; i < count; i++)
    {
        struct atom *atom = &ctx->atoms[i];
        BOOL line_empty = TRUE;

        for (unsigned int k = start; k < i; k++)
            if (ctx->atoms[k].type != ATOM_SPACE) { line_empty = FALSE; break; }

        if (atom->type == ATOM_BREAK)
        {
            if (!atom->keep && line_empty)
            {
                start = i + 1;  /* a soft break with nothing before it */
                used = 0;
                continue;
            }
            y += place_line( ctx, start, i, x, y, width, block );
            start = i + 1;
            used = 0;
            continue;
        }
        if (atom->type == ATOM_SPACE)
        {
            if (!line_empty || atom->keep) used += atom->width;
            continue;
        }
        /* a word that does not fit starts a new line, if there is a space to break at */
        if (!line_empty && used + atom->width > width && i > start && ctx->atoms[i - 1].type == ATOM_SPACE &&
            !ctx->atoms[i - 1].keep)
        {
            y += place_line( ctx, start, i, x, y, width, block );
            start = i;
            used = 0;
        }
        used += atom->width;
    }
    if (start < count) y += place_line( ctx, start, count, x, y, width, block );

    ctx->atom_count = 0;
    ctx->text_len = 0;
    return y - top;
}

/* --- blocks --- */

static float layout_block( struct layout *ctx, lxb_dom_element_t *element, const struct style *style,
                           float x, float y, float containing, int list_index );

/* the children of a block container, from top down; returns their height */
static float flow_children( struct layout *ctx, lxb_dom_node_t *parent, const struct style *style,
                            float x, float y, float width )
{
    float cursor = 0, pending_margin = 0;
    int list_index = 0;

    for (lxb_dom_node_t *node = lxb_dom_node_first_child( parent ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        const struct style *child;
        float margin_top;

        if (node->type == LXB_DOM_NODE_TYPE_TEXT)
        {
            const lexbor_str_t *data = &lxb_dom_interface_character_data( node )->data;

            add_text( ctx, style, (const char *)data->data, data->length );
            continue;
        }
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        if (!(child = compute( ctx, style, element )) || child->display == DISPLAY_NONE) continue;

        if (!is_block( child ))
        {
            switch (lxb_dom_element_tag_id( element ))
            {
            case LXB_TAG_BR:  add_break( ctx, child, TRUE ); break;
            case LXB_TAG_IMG: add_image( ctx, element, child ); break;
            default:          gather_inline( ctx, node, child ); break;
            }
            continue;
        }

        /* the lines before this block are a box of their own */
        if (ctx->atom_count)
        {
            float height = flush_inline( ctx, x, y + cursor + pending_margin, width, style );

            if (height)
            {
                cursor += pending_margin + height;
                pending_margin = 0;
            }
        }
        margin_top = length_px( &child->margin[0], width );
        cursor += max( pending_margin, margin_top );    /* neighbouring margins collapse */
        if (child->display == DISPLAY_LIST_ITEM) list_index++;
        cursor += layout_block( ctx, element, child, x, y + cursor, width, list_index );
        pending_margin = length_px( &child->margin[2], width );
    }
    if (ctx->atom_count)
    {
        float height = flush_inline( ctx, x, y + cursor + pending_margin, width, style );

        if (height) cursor += pending_margin + height, pending_margin = 0;
    }
    return cursor + pending_margin;
}

static void emit_marker( struct layout *ctx, const struct style *style, float content_x, float content_y,
                         int index )
{
    const struct font *font = font_get( &style->font );
    WCHAR marker[16];
    struct item *item;
    int width, len;

    if (style->list == LIST_DECIMAL) len = wsprintfW( marker, L"%d.", index );
    else if (style->list == LIST_DISC) { marker[0] = 0x2022; len = 1; }    /* • */
    else return;

    width = font_measure( font, marker, len );
    if (!(item = display_list_add( ctx->list, ITEM_RUN ))) return;
    item->x = (int)(content_x - width - font->space * 2);
    item->baseline = (int)(content_y + (line_height_of( style, font ) - (font->ascent + font->descent)) / 2 +
                           font->ascent + 0.5f);
    SetRect( &item->rect, item->x, (int)content_y, item->x + width, item->baseline + font->descent );
    item->text = display_list_string( ctx->list, marker, len );
    item->length = len;
    item->font = font;
    item->color = style->color;
}

static float layout_block( struct layout *ctx, lxb_dom_element_t *element, const struct style *style,
                           float x, float y, float containing, int list_index )
{
    float ml = length_px( &style->margin[3], containing ), mr = length_px( &style->margin[1], containing );
    float frame_h = style->border[3] + style->padding[3] + style->padding[1] + style->border[1];
    float frame_v_top = style->border[0] + style->padding[0];
    float frame_v_bottom = style->padding[2] + style->border[2];
    float content_w, content_h, box_x, total;
    unsigned int background, borders;

    if (style->width.unit == LEN_AUTO) content_w = containing - ml - mr - frame_h;
    else
    {
        content_w = length_px( &style->width, containing );
        /* margin: 0 auto centres a block of a set width */
        if (style->margin[1].unit == LEN_AUTO && style->margin[3].unit == LEN_AUTO)
            ml = mr = max( 0, (containing - content_w - frame_h) / 2 );
        else if (style->margin[3].unit == LEN_AUTO)
            ml = max( 0, containing - content_w - frame_h - mr );
    }
    if (content_w < 0) content_w = 0;
    box_x = x + ml;

    /* the background and the borders are drawn under the content: their places are taken now */
    background = ctx->list->count;
    emit_rect( ctx, 0, 0, 1, 1, style->background );
    background = ctx->list->count > background ? background : ~0u;
    borders = ctx->list->count;
    for (int i = 0; i < 4; i++) emit_rect( ctx, 0, 0, 1, 1, style->border[i] > 0 ? style->border_color[i] : 0 );

    if (style->display == DISPLAY_LIST_ITEM)
        emit_marker( ctx, style, box_x + style->border[3] + style->padding[3], y + frame_v_top, list_index );

    content_h = flow_children( ctx, lxb_dom_interface_node( element ), style, box_x + style->border[3] + style->padding[3],
                               y + frame_v_top, content_w );
    if (style->height.unit == LEN_PX) content_h = style->height.value;
    total = frame_v_top + content_h + frame_v_bottom;

    if (background != ~0u)
        SetRect( &ctx->list->items[background].rect, (int)box_x, (int)y, (int)ceilf( box_x + content_w + frame_h ),
                 (int)ceilf( y + total ) );
    for (int i = 0, item = borders; i < 4; i++)
    {
        float right = box_x + content_w + frame_h, bottom = y + total;
        RECT *rect;

        if (!(style->border[i] > 0 && (style->border_color[i] >> 24))) continue;
        rect = &ctx->list->items[item++].rect;
        switch (i)
        {
        case 0: SetRect( rect, (int)box_x, (int)y, (int)ceilf( right ), (int)ceilf( y + style->border[0] ) ); break;
        case 1: SetRect( rect, (int)(right - style->border[1]), (int)y, (int)ceilf( right ), (int)ceilf( bottom ) ); break;
        case 2: SetRect( rect, (int)box_x, (int)(bottom - style->border[2]), (int)ceilf( right ), (int)ceilf( bottom ) ); break;
        case 3: SetRect( rect, (int)box_x, (int)y, (int)ceilf( box_x + style->border[3] ), (int)ceilf( bottom ) ); break;
        }
    }
    return total;
}

/* --- the document --- */

void layout_document( struct document *doc, struct display_list *list, int width, int height )
{
    struct layout ctx = { .doc = doc, .list = list };
    lxb_dom_element_t *root;
    const struct style *style, *body_style = NULL;
    ULONGLONG start = GetTickCount64();
    float content;

    display_list_reset( list );
    list->width = width;
    list->height = height;
    style_viewport_width = (float)width;
    style_viewport_height = (float)height;
    if (!doc || !doc->html || !(root = lxb_dom_document_element( lxb_dom_interface_document( doc->html ) )))
        return;

    if (!(style = compute( &ctx, NULL, root ))) return;
    /* the canvas takes the root's background, else the body's */
    if (style->background >> 24) list->background = style->background | 0xff000000;
    else if (doc->html->body && (body_style = compute( &ctx, style, lxb_dom_interface_element( doc->html->body ) )) &&
             (body_style->background >> 24))
        list->background = body_style->background | 0xff000000;

    content = layout_block( &ctx, root, style, 0, length_px( &style->margin[0], (float)width ), (float)width, 0 );
    list->content_height = (int)ceilf( content + length_px( &style->margin[0], (float)width ) +
                                       length_px( &style->margin[2], (float)width ) );

    for (unsigned int i = 0; i < ctx.style_count; i++) free( ctx.styles[i] );
    free( ctx.styles );
    free( ctx.atoms );
    free( ctx.text );
    TRACE( "%u items, %u styles, %d px tall, in %I64u ms\n", list->count, ctx.style_count, list->content_height,
           GetTickCount64() - start );
}
