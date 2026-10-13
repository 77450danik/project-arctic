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

enum atom_type { ATOM_TEXT, ATOM_SPACE, ATOM_BREAK, ATOM_IMAGE, ATOM_BOX };

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
    lxb_dom_element_t  *link;       /* the <a href> it is inside, or NULL */
    lxb_dom_element_t  *anchors[2]; /* inlines with an id that begin with it: #fragment goes to its line */
    lxb_dom_element_t  *owner;      /* the element it is the text or the image of */
    /* ATOM_BOX: an inline-block or a form control, laid out at the origin - its
     * items, links and controls in the list, moved to the line when it is placed */
    struct list_mark    box_from, box_to;
    int                 baseline;   /* from its top */
};

/* an absolute or fixed box, laid out when its containing block is done */
struct deferred
{
    lxb_dom_element_t  *element;
    const struct style *style;
    float               static_x, static_y;     /* where it would have stood in the flow */
    lxb_dom_element_t  *link;
};

struct layout
{
    struct document     *doc;
    struct display_list *list;
    lxb_dom_element_t   *link;      /* the <a href> being laid out, or NULL */
    lxb_dom_element_t   *anchors[2];    /* inlines with an id waiting for their first atom */
    struct style       **styles;    /* every computed style, freed at the end */
    unsigned int         style_count, style_capacity;
    struct atom         *atoms;     /* the inline content being gathered */
    unsigned int         atom_count, atom_capacity;
    WCHAR               *text;
    size_t               text_len, text_capacity;
    float                line_width;    /* what the lines being gathered may fill */
    float                static_x, static_y;    /* where a box in the flow would go now */
    struct deferred     *deferred;      /* positioned boxes waiting for their containing block */
    unsigned int         deferred_count, deferred_capacity;
    float                viewport_w, viewport_h;
    float                last_width;    /* the border box of the last block laid out, */
    float                last_margin[2];    /* and its left and right margins as they came out */
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

/* ARCTIC_HTML_DEBUG_ID=<id>: the computed style of that element in the log,
 * to see why a page looks as it does */
static void debug_style( lxb_dom_element_t *element, const struct style *style )
{
    static char wanted[128] = "?";
    const char *id;
    size_t len;

    if (wanted[0] == '?' && !GetEnvironmentVariableA( "ARCTIC_HTML_DEBUG_ID", wanted, sizeof(wanted) )) wanted[0] = 0;
    if (!wanted[0]) return;
    if (wanted[0] == '.')   /* .name: the first element with that class */
    {
        static BOOL seen;
        if (seen || !(id = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"class", 5, &len )))
            return;
        {
            char *copy = malloc( len + 1 ), *token, *next = NULL;
            BOOL found = FALSE;
            if (!copy) return;
            memcpy( copy, id, len );
            copy[len] = 0;
            for (token = strtok_s( copy, " \t\n", &next ); token && !found; token = strtok_s( NULL, " \t\n", &next ))
                found = !strcmp( token, wanted + 1 );
            free( copy );
            if (!found) return;
        }
        seen = TRUE;
    }
    else if (!(id = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"id", 2, &len )) ||
             len != strlen( wanted ) || memcmp( id, wanted, len ))
        return;
    ERR( "#%s: display %d font %s %.1fpx/%d weight %d line %.1f color %08lx bg %08lx width %d:%.1f "
         "margin %.1f %.1f %.1f %.1f padding %.1f %.1f %.1f %.1f invisible %d transparent %d out_of_flow %d\n",
         wanted, style->display, debugstr_w(style->font.face), style->font_size, style->font.px, style->font.weight,
         style->line_height, style->color, style->background, style->width.unit, style->width.value,
         style->margin[0].value, style->margin[1].value, style->margin[2].value, style->margin[3].value,
         style->padding[0], style->padding[1], style->padding[2], style->padding[3],
         style->invisible, style->transparent, style->out_of_flow );
}

/* ARCTIC_HTML_OUTLINE=1: every block box taller than a line, with its place, in the log */
static void debug_outline( lxb_dom_element_t *element, const struct style *style, float x, float y, float width,
                           float height )
{
    static int enabled = -1;
    const char *class;
    size_t tag_len, class_len = 0;
    const lxb_char_t *tag;

    if (enabled < 0) enabled = GetEnvironmentVariableA( "ARCTIC_HTML_OUTLINE", NULL, 0 ) > 0;
    if (!enabled || height < 24) return;
    tag = lxb_dom_element_local_name( element, &tag_len );
    if (!(class = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"class", 5, &class_len )))
        class = "";
    ERR( "%.*s.%.*s at %.0f,%.0f %.0fx%.0f font %.1f\n", (int)tag_len, tag, (int)min( class_len, 60 ), class,
         x, y, width, height, style->font_size );
}

static const struct style *compute( struct layout *ctx, const struct style *parent, lxb_dom_element_t *element )
{
    struct style *style = new_style( ctx );

    if (!style) return NULL;
    style_compute( parent, element, style );
    debug_style( element, style );
    return style;
}

/* --- marks in the list, and the positioned boxes waiting --- */

static void mark_at( struct layout *ctx, struct list_mark *mark )
{
    display_list_mark( ctx->list, mark );
    mark->deferred = ctx->deferred_count;
}

/* what was laid out since a mark goes: a pass that only measured */
static void truncate_list( struct layout *ctx, const struct list_mark *mark )
{
    ctx->list->count = mark->items;
    ctx->list->link_count = mark->links;
    ctx->list->anchor_count = mark->anchors;
    ctx->list->control_count = mark->controls;
    ctx->deferred_count = mark->deferred;
}

static void shift_box( struct layout *ctx, const struct list_mark *from, const struct list_mark *to, int dx, int dy )
{
    display_list_shift( ctx->list, from, to, dx, dy );
    for (unsigned int i = from->deferred; i < to->deferred && i < ctx->deferred_count; i++)
    {
        ctx->deferred[i].static_x += dx;
        ctx->deferred[i].static_y += dy;
    }
}

static void defer( struct layout *ctx, lxb_dom_element_t *element, const struct style *style )
{
    struct deferred *entry;

    if (ctx->deferred_count == ctx->deferred_capacity)
    {
        unsigned int capacity = ctx->deferred_capacity ? ctx->deferred_capacity * 2 : 32;
        struct deferred *grown = realloc( ctx->deferred, capacity * sizeof(*grown) );

        if (!grown) return;
        ctx->deferred = grown;
        ctx->deferred_capacity = capacity;
    }
    entry = &ctx->deferred[ctx->deferred_count++];
    entry->element = element;
    entry->style = style;
    entry->static_x = ctx->static_x;
    entry->static_y = ctx->static_y;
    entry->link = ctx->link;
}

/* an input, a select, a textarea: drawn by the engine, whatever display the page gives it */
static BOOL is_field( lxb_dom_element_t *element )
{
    lxb_tag_id_t tag = lxb_dom_element_tag_id( element );

    return tag == LXB_TAG_INPUT || tag == LXB_TAG_SELECT || tag == LXB_TAG_TEXTAREA;
}

static BOOL is_block( const struct style *style )
{
    return style->display == DISPLAY_BLOCK || style->display == DISPLAY_LIST_ITEM;
}

/* visibility: hidden and opacity: 0 keep their place and draw nothing */
static BOOL paints( const struct style *style )
{
    return !style->invisible && !style->transparent;
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
    atom->link = ctx->link;
    atom->owner = ctx->list->owner;
    memcpy( atom->anchors, ctx->anchors, sizeof(atom->anchors) );
    memset( ctx->anchors, 0, sizeof(ctx->anchors) );
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
    w = style->width.unit == LEN_PX ? style->width.value : (float)attribute_number( element, "width" );
    h = style->height.unit == LEN_PX ? style->height.value : (float)attribute_number( element, "height" );
    if (!image)
    {
        /* an image of a size the page gave keeps its place, as in browsers;
         * one of no size says in words what it is */
        if (w > 0 && h > 0)
        {
            if (!(atom = add_atom( ctx, ATOM_IMAGE, style ))) return;
            atom->width = (int)(w + 0.5f);
            atom->height = (int)(h + 0.5f);
        }
        else if ((alt = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"alt", 3, &len )) && len)
            add_text( ctx, style, alt, len );
        return;
    }
    if (!w && !h) { w = (float)image->width; h = (float)image->height; }
    else if (!w) w = h * image->width / image->height;
    else if (!h) h = w * image->height / image->width;

    if (!(atom = add_atom( ctx, ATOM_IMAGE, style ))) return;
    atom->image = image;
    atom->width = (int)(w + 0.5f);
    atom->height = (int)(h + 0.5f);
}

static void gather_inline( struct layout *ctx, lxb_dom_node_t *node, const struct style *style );
static float layout_block( struct layout *ctx, lxb_dom_element_t *element, const struct style *style,
                           float x, float y, float containing, int list_index );

/* --- form controls --- */

static void emit_frame( struct layout *ctx, const struct style *style, float left, float top, float right,
                        float bottom )
{
    if (!paints( style )) return;
    emit_rect( ctx, left, top, right, bottom, style->background );
    if (style->border[0] > 0) emit_rect( ctx, left, top, right, top + style->border[0], style->border_color[0] );
    if (style->border[2] > 0) emit_rect( ctx, left, bottom - style->border[2], right, bottom, style->border_color[2] );
    if (style->border[3] > 0) emit_rect( ctx, left, top, left + style->border[3], bottom, style->border_color[3] );
    if (style->border[1] > 0) emit_rect( ctx, right - style->border[1], top, right, bottom, style->border_color[1] );
}

static void emit_text( struct layout *ctx, const struct font *font, DWORD color, const WCHAR *text, int length,
                       float x, float baseline, const RECT *clip )
{
    struct item *item;

    if (!length || !(item = display_list_add( ctx->list, ITEM_RUN ))) return;
    item->x = (int)(x + 0.5f);
    item->baseline = (int)(baseline + 0.5f);
    SetRect( &item->rect, item->x, item->baseline - font->ascent, item->x + font_measure( font, text, length ),
             item->baseline + font->descent );
    item->text = display_list_string( ctx->list, text, length );
    item->length = length;
    item->font = font;
    item->color = color;
    if (clip)
    {
        item->clipped = TRUE;
        item->clip = *clip;
        IntersectRect( &item->rect, &item->rect, clip );
    }
}

/* an input, a select, a textarea at the origin: its frame and what it
 * shows; the size of its margin box and the baseline of its text come back */
static void layout_control( struct layout *ctx, lxb_dom_element_t *element, const struct style *style, float avail,
                            float *out_width, float *out_height, float *out_baseline )
{
    enum control_kind kind = control_kind_of( element );
    lxb_tag_id_t tag = lxb_dom_element_tag_id( element );
    const struct font *font = font_get( &style->font );
    float bl = style->border[3], br = style->border[1], bt = style->border[0], bb = style->border[2];
    float frame_h = bl + style->padding[3] + style->padding[1] + br;
    float frame_v = bt + style->padding[0] + style->padding[2] + bb;
    float ml = length_px( &style->margin[3], avail ), mr = length_px( &style->margin[1], avail );
    float mt = length_px( &style->margin[0], avail ), mb = length_px( &style->margin[2], avail );
    float line = line_height_of( style, font ), content_w, content_h, left, top, cx, cy, baseline, arrow = 0;
    BOOL focused = ctx->doc && ctx->doc->focus == element && kind == CONTROL_TEXT;
    BOOL placeholder = FALSE;
    WCHAR *value = NULL, digit = '0';
    struct control_area *area;
    const char *attr;
    size_t len;
    int rows = 1;

    /* what it shows */
    switch (kind)
    {
    case CONTROL_TEXT:
        value = control_value( element );
        if (value && !value[0] &&
            (attr = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"placeholder", 11, &len )))
        {
            free( value );
            value = utf8_to_wide( attr, len, NULL );
            placeholder = TRUE;
        }
        else if (value && control_is_password( element ))
            for (WCHAR *p = value; *p; p++) *p = 0x2022;
        break;
    case CONTROL_SUBMIT:
    case CONTROL_BUTTON:
        if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"value", 5 )) value = control_value( element );
        else if (kind == CONTROL_SUBMIT) value = _wcsdup( L"Надіслати" );
        else if ((attr = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"type", 4, &len )) &&
                 len == 5 && !_strnicmp( attr, "reset", 5 ))
            value = _wcsdup( L"Скинути" );
        else if (attr && len == 4 && !_strnicmp( attr, "file", 4 )) value = _wcsdup( L"Вибрати файл" );
        break;
    case CONTROL_SELECT:
        value = control_value( element );
        arrow = 16;
        break;
    default:
        break;
    }
    if (!value) value = _wcsdup( L"" );

    /* its size: the page's, or the one the control has of itself */
    if (style->width.unit != LEN_AUTO)
        content_w = length_px( &style->width, avail ) - (style->border_box ? frame_h : 0);
    else switch (kind)
    {
    case CONTROL_TEXT:
        attr = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)
                                                            (tag == LXB_TAG_TEXTAREA ? "cols" : "size"),
                                                            tag == LXB_TAG_TEXTAREA ? 4 : 4, &len );
        content_w = (float)((attr && atoi( attr ) > 0) ? atoi( attr ) : 20) * font_measure( font, &digit, 1 );
        break;
    case CONTROL_SELECT:
    {
        /* as wide as its longest option */
        lxb_dom_node_t *root = lxb_dom_interface_node( element ), *node = lxb_dom_node_first_child( root );
        float widest = 0;

        while (node)
        {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                lxb_dom_element_tag_id( lxb_dom_interface_element( node ) ) == LXB_TAG_OPTION)
            {
                WCHAR *text = element_text( lxb_dom_interface_element( node ) );

                if (text) widest = max( widest, (float)font_measure( font, text, (int)wcslen( text ) ) );
                free( text );
            }
            if (lxb_dom_node_first_child( node ) && node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                lxb_dom_element_tag_id( lxb_dom_interface_element( node ) ) == LXB_TAG_OPTGROUP)
                node = lxb_dom_node_first_child( node );
            else if (lxb_dom_node_next( node )) node = lxb_dom_node_next( node );
            else
            {
                node = lxb_dom_node_parent( node );
                node = node && node != root ? lxb_dom_node_next( node ) : NULL;
            }
        }
        content_w = widest + arrow + 4;
        break;
    }
    default:
        content_w = (float)font_measure( font, value, (int)wcslen( value ) );
        break;
    }
    if (style->max_width.unit != LEN_AUTO)
        content_w = min( content_w, length_px( &style->max_width, avail ) - (style->border_box ? frame_h : 0) );
    if (style->min_width.unit != LEN_AUTO)
        content_w = max( content_w, length_px( &style->min_width, avail ) - (style->border_box ? frame_h : 0) );
    content_w = max( content_w, 0 );

    if (tag == LXB_TAG_TEXTAREA)
    {
        attr = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"rows", 4, &len );
        rows = attr && atoi( attr ) > 0 ? atoi( attr ) : 2;
    }
    if (style->height.unit == LEN_PX)
        content_h = max( 0, style->height.value - (style->border_box ? frame_v : 0) );
    else content_h = line * rows;

    /* the frame */
    left = ml;
    top = mt;
    emit_frame( ctx, style, left, top, left + frame_h + content_w, top + frame_v + content_h );
    cx = left + bl + style->padding[3];
    cy = top + bt + style->padding[0];
    baseline = cy + (min( content_h, line ) - (font->ascent + font->descent)) / 2 + font->ascent;
    if (kind == CONTROL_TEXT && tag != LXB_TAG_TEXTAREA)  /* one line, in the middle of a taller field */
        baseline = cy + (content_h - (font->ascent + font->descent)) / 2 + font->ascent;

    if ((kind == CONTROL_CHECKBOX || kind == CONTROL_RADIO) && paints( style ) &&
        lxb_dom_element_has_attribute( element, (const lxb_char_t *)"checked", 7 ))
    {
        float inset = kind == CONTROL_RADIO ? 3 : 1;

        emit_rect( ctx, left + bl + inset, top + bt + inset, left + frame_h + content_w - br - inset,
                   top + frame_v + content_h - bb - inset, 0xff0075ff );
        baseline = top + frame_v + content_h;
    }
    else if (kind == CONTROL_CHECKBOX || kind == CONTROL_RADIO) baseline = top + frame_v + content_h;

    if (paints( style ) && kind != CONTROL_CHECKBOX && kind != CONTROL_RADIO)
    {
        RECT clip;
        float scroll = 0, tx = cx;
        int value_len = (int)wcslen( value );
        DWORD color = placeholder ? 0xff757575 : style->color;

        SetRect( &clip, (int)floorf( cx ), (int)floorf( cy ), (int)ceilf( cx + content_w - arrow ),
                 (int)ceilf( cy + content_h ) );
        if (kind == CONTROL_SUBMIT || kind == CONTROL_BUTTON)
            tx = cx + max( 0, (content_w - font_measure( font, value, value_len )) / 2 );

        if (tag == LXB_TAG_TEXTAREA)
        {
            /* its lines as typed; the long ones are cut at the edge until stage 3 wraps them */
            const WCHAR *start = value;
            float y = baseline;
            int at = 0, caret_line = 0, caret_col = 0;

            for (const WCHAR *p = value; ; p++)
            {
                if (*p == '\n' || !*p)
                {
                    if (focused && ctx->doc->caret >= at && ctx->doc->caret <= at + (int)(p - start))
                    {
                        caret_line = (int)((y - baseline) / line + 0.5f);
                        caret_col = ctx->doc->caret - at;
                        emit_rect( ctx, cx + font_measure( font, start, caret_col ), y - font->ascent,
                                   cx + font_measure( font, start, caret_col ) + 1, y + font->descent, style->color );
                    }
                    emit_text( ctx, font, color, start, (int)(p - start), cx, y, &clip );
                    at += (int)(p - start) + 1;
                    y += line;
                    if (!*p) break;
                    start = p + 1;
                }
            }
            (void)caret_line;
        }
        else
        {
            if (focused)
            {
                int caret = min( max( ctx->doc->caret, 0 ), placeholder ? 0 : value_len );
                float caret_x = (float)(placeholder ? 0 : font_measure( font, value, caret ));

                if (caret_x > content_w - 2) scroll = caret_x - (content_w - 2);
                tx -= scroll;
                emit_text( ctx, font, color, value, value_len, tx, baseline, &clip );
                emit_rect( ctx, cx + caret_x - scroll, baseline - font->ascent, cx + caret_x - scroll + 1,
                           baseline + font->descent, style->color );
            }
            else emit_text( ctx, font, color, value, value_len, tx, baseline, &clip );
        }
        if (arrow)
        {
            static const WCHAR down = 0x25bc;    /* ▼ */
            struct font_key key = style->font;
            const struct font *small;

            key.px = max( 6, key.px * 2 / 3 );
            small = font_get( &key );
            emit_text( ctx, small, 0xff505050, &down, 1, cx + content_w - arrow + 3, baseline, NULL );
        }
        if (focused)    /* the focus ring */
        {
            float right = left + frame_h + content_w, bottom = top + frame_v + content_h;

            emit_rect( ctx, left - 1, top - 1, right + 1, top + 1, 0xff005fcc );
            emit_rect( ctx, left - 1, bottom - 1, right + 1, bottom + 1, 0xff005fcc );
            emit_rect( ctx, left - 1, top - 1, left + 1, bottom + 1, 0xff005fcc );
            emit_rect( ctx, right - 1, top - 1, right + 1, bottom + 1, 0xff005fcc );
        }
        if ((area = display_list_control( ctx->list )))
        {
            SetRect( &area->rect, (int)left, (int)top, (int)ceilf( left + frame_h + content_w ),
                     (int)ceilf( top + frame_v + content_h ) );
            area->element = element;
            area->kind = kind;
            area->text_x = (int)(tx + 0.5f);
            area->font = font;
        }
    }
    else if ((area = display_list_control( ctx->list )))
    {
        SetRect( &area->rect, (int)left, (int)top, (int)ceilf( left + frame_h + content_w ),
                 (int)ceilf( top + frame_v + content_h ) );
        area->element = element;
        area->kind = kind;
        area->font = font;
    }
    free( value );

    *out_width = ml + frame_h + content_w + mr;
    *out_height = mt + frame_v + content_h + mb;
    *out_baseline = baseline;
}

/* --- inline-blocks --- */

/* An inline-block, a button, a form control: laid out as a block of its own
 * at the origin, as wide as its content up to the line, and placed on the
 * line as one atom. The inline content gathered around it waits meanwhile. */
static void add_box( struct layout *ctx, lxb_dom_element_t *element, const struct style *style )
{
    struct atom *saved_atoms = ctx->atoms, *atom;
    unsigned int saved_count = ctx->atom_count, saved_capacity = ctx->atom_capacity;
    WCHAR *saved_text = ctx->text;
    size_t saved_len = ctx->text_len, saved_text_capacity = ctx->text_capacity;
    lxb_dom_element_t *pending[2];
    float avail = ctx->line_width, width, height, baseline = -1;
    struct list_mark from, to;
    lxb_tag_id_t tag = lxb_dom_element_tag_id( element );

    memcpy( pending, ctx->anchors, sizeof(pending) );
    memset( ctx->anchors, 0, sizeof(ctx->anchors) );
    ctx->atoms = NULL;
    ctx->atom_count = ctx->atom_capacity = 0;
    ctx->text = NULL;
    ctx->text_len = ctx->text_capacity = 0;
    mark_at( ctx, &from );

    if (tag == LXB_TAG_INPUT || tag == LXB_TAG_SELECT || tag == LXB_TAG_TEXTAREA)
        layout_control( ctx, element, style, avail, &width, &height, &baseline );
    else
    {
        float mt = length_px( &style->margin[0], avail ), mb = length_px( &style->margin[2], avail );

        struct style *measure = style->width.unit == LEN_AUTO ? new_style( ctx ) : NULL;

        /* measured with its lines to the left: centred text would reach out to the middle of the line */
        if (measure)
        {
            *measure = *style;
            measure->text_align = ALIGN_LEFT;
        }
        height = layout_block( ctx, element, measure ? measure : style, 0, mt, avail, 0 );
        if (measure)
        {
            /* shrink to fit: as wide as what it holds, laid out again at that width */
            float frame_h = style->border[3] + style->padding[3] + style->padding[1] + style->border[1];
            float inner = ctx->last_margin[0] + style->border[3] + style->padding[3], right = inner;
            struct list_mark now;

            mark_at( ctx, &now );
            for (unsigned int i = from.items; i < now.items; i++)
                if (ctx->list->items[i].type != ITEM_RECT && ctx->list->items[i].absolute == ctx->list->absolute) right = max( right, (float)ctx->list->items[i].rect.right );
            for (unsigned int i = from.controls; i < now.controls; i++)
                right = max( right, (float)ctx->list->controls[i].rect.right );
            /* laid out again, at that width and as the page aligns it */
            {
                struct style *fitted = new_style( ctx );

                if (fitted)
                {
                    *fitted = *style;
                    fitted->width.unit = LEN_PX;
                    fitted->width.value = ceilf( min( right - inner, ctx->last_width - frame_h ) );
                    fitted->border_box = FALSE;
                    truncate_list( ctx, &from );
                    height = layout_block( ctx, element, fitted, 0, mt, avail, 0 );
                }
            }
        }
        width = ctx->last_margin[0] + ctx->last_width + ctx->last_margin[1];
        if (tag == LXB_TAG_BUTTON)
        {
            /* a <button> is a box of its own content, and a control the view clicks */
            struct control_area *area = display_list_control( ctx->list );

            if (area)
            {
                SetRect( &area->rect, (int)ctx->last_margin[0], (int)mt,
                         (int)ceilf( ctx->last_margin[0] + ctx->last_width ), (int)ceilf( mt + height ) );
                area->element = element;
                area->kind = control_kind_of( element );
                area->font = font_get( &style->font );
            }
        }
        mark_at( ctx, &to );
        /* its baseline is its last line's; with no line, its bottom margin edge */
        for (unsigned int i = to.items; i-- > from.items; )
            if (ctx->list->items[i].type == ITEM_RUN && !ctx->list->items[i].clipped &&
                ctx->list->items[i].absolute == ctx->list->absolute)
            {
                baseline = (float)ctx->list->items[i].baseline;
                break;
            }
        height += mt + mb;
        if (baseline < 0) baseline = height;
    }
    mark_at( ctx, &to );

    free( ctx->atoms );
    free( ctx->text );
    ctx->atoms = saved_atoms;
    ctx->atom_count = saved_count;
    ctx->atom_capacity = saved_capacity;
    ctx->text = saved_text;
    ctx->text_len = saved_len;
    ctx->text_capacity = saved_text_capacity;
    memcpy( ctx->anchors, pending, sizeof(pending) );

    if (!(atom = add_atom( ctx, ATOM_BOX, style ))) return;
    atom->width = (int)ceilf( width );
    atom->height = (int)ceilf( height );
    atom->baseline = (int)(baseline + 0.5f);
    atom->box_from = from;
    atom->box_to = to;
}

/* an <a href> makes what is inside it a link */
static lxb_dom_element_t *enter_link( struct layout *ctx, lxb_dom_element_t *element )
{
    lxb_dom_element_t *outer = ctx->link;

    if (lxb_dom_element_tag_id( element ) == LXB_TAG_A &&
        lxb_dom_element_has_attribute( element, (const lxb_char_t *)"href", 4 ))
        ctx->link = element;
    return outer;
}

static void gather_styled( struct layout *ctx, lxb_dom_element_t *element, const struct style *style );
static void gather_owned( struct layout *ctx, lxb_dom_element_t *element, const struct style *style );

static void gather_element( struct layout *ctx, lxb_dom_element_t *element, const struct style *parent )
{
    const struct style *style = compute( ctx, parent, element );

    if (!style || style->display == DISPLAY_NONE) return;
    gather_styled( ctx, element, style );
}

static void gather_styled( struct layout *ctx, lxb_dom_element_t *element, const struct style *style )
{
    lxb_dom_element_t *outer_owner = ctx->list->owner;

    ctx->list->owner = element;
    gather_owned( ctx, element, style );
    ctx->list->owner = outer_owner;
}

static void gather_owned( struct layout *ctx, lxb_dom_element_t *element, const struct style *style )
{
    lxb_dom_element_t *outer;

    /* positioned out of a line: placed when its containing block is done */
    if (style->out_of_flow)
    {
        defer( ctx, element, style );
        return;
    }
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
    if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"id", 2 ))
    {
        for (int i = 0; i < ARRAY_SIZE(ctx->anchors); i++)
            if (!ctx->anchors[i]) { ctx->anchors[i] = element; break; }
    }
    /* a block inside an inline: its own lines, until blocks in inlines are split properly */
    outer = enter_link( ctx, element );
    if (style->display == DISPLAY_INLINE_BLOCK || is_field( element ))
    {
        /* a field made a block by the page stands on a line of its own */
        if (is_block( style )) add_break( ctx, style, FALSE );
        add_box( ctx, element, style );
        if (is_block( style )) add_break( ctx, style, FALSE );
        ctx->link = outer;
        return;
    }
    if (is_block( style )) add_break( ctx, style, FALSE );
    gather_inline( ctx, lxb_dom_interface_node( element ), style );
    if (is_block( style )) add_break( ctx, style, FALSE );
    ctx->link = outer;
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

/* where the top of an image or an inline-block goes on its line, by its vertical-align */
static float atomic_top( const struct atom *atom, float own_baseline, float top, float baseline, float line_height,
                         float x_height )
{
    switch (atom->style->valign)
    {
    case VALIGN_MIDDLE: return baseline - x_height / 2 - atom->height / 2.0f;
    case VALIGN_TOP:    return top;
    case VALIGN_BOTTOM: return top + line_height - atom->height;
    default:            return baseline - own_baseline;
    }
}

static float place_line_owned( struct layout *ctx, unsigned int first, unsigned int end, float x, float top,
                               float width, const struct style *block );

/* a line of the block, each run and image of it the element's it came from */
static float place_line( struct layout *ctx, unsigned int first, unsigned int end, float x, float top, float width,
                         const struct style *block )
{
    lxb_dom_element_t *owner = ctx->list->owner;
    float height = place_line_owned( ctx, first, end, x, top, width, block );

    ctx->list->owner = owner;
    return height;
}

static float place_line_owned( struct layout *ctx, unsigned int first, unsigned int end, float x, float top,
                               float width, const struct style *block )
{
    const struct font *strut = font_get( &block->font );
    float strut_half = (line_height_of( block, strut ) - (strut->ascent + strut->descent)) / 2;
    float ascent = strut->ascent + strut_half, descent = strut->descent + strut_half;
    float used = 0, pen, baseline, tall = 0, x_height = strut->ascent * 0.5f;
    WCHAR *run = NULL;
    int run_capacity = 0;

    for (unsigned int i = first; i < end; i++)
        for (int a = 0; a < ARRAY_SIZE(ctx->atoms[i].anchors) && ctx->atoms[i].anchors[a]; a++)
            display_list_anchor( ctx->list, (int)top, ctx->atoms[i].anchors[a] );

    /* no space at either end of a line, unless it is kept */
    while (first < end && ctx->atoms[first].type == ATOM_SPACE && !ctx->atoms[first].keep) first++;
    while (end > first && ctx->atoms[end - 1].type == ATOM_SPACE && !ctx->atoms[end - 1].keep) end--;

    for (unsigned int i = first; i < end; i++)
    {
        const struct atom *atom = &ctx->atoms[i];

        used += atom->width;
        if (atom->type == ATOM_IMAGE || atom->type == ATOM_BOX)
        {
            float h = (float)atom->height, b = atom->type == ATOM_BOX ? (float)atom->baseline : h;

            switch (atom->style->valign)
            {
            case VALIGN_MIDDLE:
                ascent = max( ascent, h / 2 + x_height / 2 );
                descent = max( descent, h / 2 - x_height / 2 );
                break;
            case VALIGN_TOP:
            case VALIGN_BOTTOM:
                tall = max( tall, h );  /* they take the line as it is, and only make it tall enough */
                break;
            default:
                ascent = max( ascent, b );
                descent = max( descent, h - b );
                break;
            }
        }
        else if (atom->type == ATOM_TEXT)
        {
            float half = (line_height_of( atom->style, atom->font ) - (atom->font->ascent + atom->font->descent)) / 2;

            ascent = max( ascent, atom->font->ascent + half );
            descent = max( descent, atom->font->descent + half );
        }
    }
    if (ascent + descent < tall) descent = tall - ascent;
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

        ctx->list->owner = atom->owner;
        if (atom->type == ATOM_IMAGE)
        {
            if (atom->image && paints( atom->style ) && (item = display_list_add( ctx->list, ITEM_IMAGE )))
            {
                int y = (int)atomic_top( atom, (float)atom->height, top, baseline, ascent + descent, x_height );

                SetRect( &item->rect, (int)pen, y, (int)pen + atom->width, y + atom->height );
                item->image = atom->image;
                if (atom->link) display_list_link( ctx->list, &item->rect, atom->link );
            }
            pen += atom->width;
            i++;
            continue;
        }
        if (atom->type == ATOM_BOX)
        {
            shift_box( ctx, &atom->box_from, &atom->box_to, (int)(pen + 0.5f),
                                (int)(atomic_top( atom, (float)atom->baseline, top, baseline, ascent + descent,
                                                  x_height ) + 0.5f) );
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

            while (j < end && ctx->atoms[j].style == style && ctx->atoms[j].link == atom->link &&
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
            if (!paints( style ))
            {
                pen += run_width;
                i = j;
                continue;
            }
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
                if (atom->link) display_list_link( ctx->list, &item->rect, atom->link );
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
        if (!line_empty && used + atom->width > width && i > start &&
            ((ctx->atoms[i - 1].type == ATOM_SPACE && !ctx->atoms[i - 1].keep) ||
             atom->type == ATOM_BOX || atom->type == ATOM_IMAGE ||
             ctx->atoms[i - 1].type == ATOM_BOX || ctx->atoms[i - 1].type == ATOM_IMAGE))
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
    float cursor = 0, pending_margin = 0, outer_width = ctx->line_width;
    int list_index = 0;

    ctx->line_width = width;

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

        ctx->static_x = x;
        ctx->static_y = y + cursor + pending_margin;
        if (child->out_of_flow)
        {
            /* absolute and fixed: placed when the containing block is done, taking no place in the flow */
            defer( ctx, element, child );
            continue;
        }
        if (!is_block( child ) || is_field( element ))
        {
            gather_styled( ctx, element, child );
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
        {
            lxb_dom_element_t *outer = enter_link( ctx, element );

            cursor += layout_block( ctx, element, child, x, y + cursor, width, list_index );
            ctx->link = outer;
        }
        pending_margin = length_px( &child->margin[2], width );
    }
    if (ctx->atom_count)
    {
        float height = flush_inline( ctx, x, y + cursor + pending_margin, width, style );

        if (height) cursor += pending_margin + height, pending_margin = 0;
    }
    ctx->line_width = outer_width;
    return cursor + pending_margin;
}

/* --- flexbox --- */

/* the inline content being gathered, put aside while something is laid out
 * on its own and taken back after */
struct inline_state
{
    struct atom       *atoms;
    unsigned int       count, capacity;
    WCHAR             *text;
    size_t             len, text_capacity;
    lxb_dom_element_t *anchors[2];
};

static void push_inline( struct layout *ctx, struct inline_state *state )
{
    state->atoms = ctx->atoms;
    state->count = ctx->atom_count;
    state->capacity = ctx->atom_capacity;
    state->text = ctx->text;
    state->len = ctx->text_len;
    state->text_capacity = ctx->text_capacity;
    memcpy( state->anchors, ctx->anchors, sizeof(state->anchors) );
    ctx->atoms = NULL;
    ctx->atom_count = ctx->atom_capacity = 0;
    ctx->text = NULL;
    ctx->text_len = ctx->text_capacity = 0;
    memset( ctx->anchors, 0, sizeof(ctx->anchors) );
}

static void pop_inline( struct layout *ctx, const struct inline_state *state )
{
    free( ctx->atoms );
    free( ctx->text );
    ctx->atoms = state->atoms;
    ctx->atom_count = state->count;
    ctx->atom_capacity = state->capacity;
    ctx->text = state->text;
    ctx->text_len = state->len;
    ctx->text_capacity = state->text_capacity;
    memcpy( ctx->anchors, state->anchors, sizeof(ctx->anchors) );
}

struct flex_item
{
    lxb_dom_element_t  *element;    /* NULL for a run of text right in the container */
    lxb_dom_node_t     *text;
    const struct style *style;
    float               margin[4];  /* auto as 0 */
    BOOL                auto_margin[4];
    float               frame_h, frame_v;
    float               hypothetical;   /* the border box's main size before flexing */
    float               main;           /* and after */
    float               cross;          /* the border box's cross size */
    struct list_mark    from, to;
    int                 order, index;
};

/* A flex item at the origin, its margin box's corner at 0,0. main and
 * cross, when not negative, are the border box's width and height it must
 * take; its border box's own width and height come back. */
static float lay_item( struct layout *ctx, struct flex_item *item, float main, float cross, float containing,
                       BOOL measuring, float *width_out )
{
    struct inline_state saved;
    float height, width = 0;

    push_inline( ctx, &saved );
    mark_at( ctx, &item->from );
    if (item->text)
    {
        const lexbor_str_t *data = &lxb_dom_interface_character_data( item->text )->data;
        float avail = main >= 0 ? main : containing;
        struct list_mark now;

        add_text( ctx, item->style, (const char *)data->data, data->length );
        height = flush_inline( ctx, 0, 0, avail, item->style );
        mark_at( ctx, &now );
        for (unsigned int i = item->from.items; i < now.items; i++)
            if (ctx->list->items[i].absolute == ctx->list->absolute)
                width = max( width, (float)ctx->list->items[i].rect.right );
        if (main >= 0) width = main;
    }
    else
    {
        struct style *copy = new_style( ctx );
        const struct style *style = item->style;

        if (copy)
        {
            *copy = *style;
            if (measuring) copy->text_align = ALIGN_LEFT;
            if (main >= 0 || cross >= 0)
            {
                /* sizes given as border boxes: a height of the page's own goes over to that too */
                if (!copy->border_box && copy->height.unit == LEN_PX) copy->height.value += item->frame_v;
                if (!copy->border_box && copy->width.unit == LEN_PX) copy->width.value += item->frame_h;
                copy->border_box = TRUE;
            }
            if (main >= 0)
            {
                copy->width.unit = LEN_PX;
                copy->width.value = main;
                copy->min_width.unit = copy->max_width.unit = LEN_AUTO;
            }
            if (cross >= 0)
            {
                copy->height.unit = LEN_PX;
                copy->height.value = cross;
            }
            style = copy;
        }
        if (is_field( item->element ))
        {
            float w, b;

            layout_control( ctx, item->element, style, containing, &w, &height, &b );
            width = w - item->margin[1] - item->margin[3];
            height -= item->margin[0] + item->margin[2];
        }
        else
        {
            height = layout_block( ctx, item->element, style, 0, item->margin[0],
                                   main >= 0 ? main + item->margin[1] + item->margin[3] : containing, 0 );
            width = ctx->last_width;
            if (measuring && style->width.unit == LEN_AUTO)
            {
                /* as wide as what it holds: the right edge of its text, its images, its fields */
                struct list_mark now;
                float inner = item->margin[3] + style->border[3] + style->padding[3], right = inner;

                mark_at( ctx, &now );
                for (unsigned int i = item->from.items; i < now.items; i++)
                    if (ctx->list->items[i].type != ITEM_RECT && ctx->list->items[i].absolute == ctx->list->absolute)
                        right = max( right, (float)ctx->list->items[i].rect.right );
                for (unsigned int i = item->from.controls; i < now.controls; i++)
                    right = max( right, (float)ctx->list->controls[i].rect.right );
                width = min( width, right - item->margin[3] + style->padding[1] + style->border[1] );
            }
        }
    }
    mark_at( ctx, &item->to );
    pop_inline( ctx, &saved );
    if (width_out) *width_out = width;
    return height;
}

static int compare_items( const void *a, const void *b )
{
    const struct flex_item *x = a, *y = b;

    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return x->index - y->index;
}

static BOOL stretches( const struct style *container, const struct flex_item *item, float line_cross )
{
    enum flex_align align = item->style->align_self != FALIGN_AUTO && !item->text ? item->style->align_self
                                                                                : container->align_items;

    return align == FALIGN_STRETCH && !item->text && item->style->height.unit == LEN_AUTO &&
           !item->auto_margin[0] && !item->auto_margin[2] &&
           line_cross - item->margin[0] - item->margin[2] - item->cross > 0.5f &&
           ((item->style->background >> 24) || item->frame_v > 0);
}

static BOOL is_blank_text( const lexbor_str_t *data )
{
    for (size_t i = 0; i < data->length; i++)
        if (!is_space( data->data[i] )) return FALSE;
    return TRUE;
}

/* The children of a flex container, at x, y in a content box `width` wide;
 * returns the content height. Items are laid out at the origin and moved to
 * their places, like inline-blocks. */
static float layout_flex( struct layout *ctx, lxb_dom_element_t *element, const struct style *style, float x,
                          float y, float width )
{
    BOOL column = style->flex_direction == FLEX_COLUMN || style->flex_direction == FLEX_COLUMN_REVERSE;
    BOOL reverse = style->flex_direction == FLEX_ROW_REVERSE || style->flex_direction == FLEX_COLUMN_REVERSE;
    float fixed_height = style->height.unit == LEN_PX ?
                         style->height.value - (style->border_box ? style->border[0] + style->padding[0] +
                                                style->padding[2] + style->border[2] : 0) : -1;
    struct flex_item *items = NULL;
    unsigned int count = 0, capacity = 0;
    float total = 0;

    /* the items: elements in the flow, and runs of text standing alone */
    for (lxb_dom_node_t *node = lxb_dom_node_first_child( lxb_dom_interface_node( element ) ); node;
         node = lxb_dom_node_next( node ))
    {
        struct flex_item *item;
        const struct style *child = NULL;

        if (node->type == LXB_DOM_NODE_TYPE_TEXT)
        {
            if (is_blank_text( &lxb_dom_interface_character_data( node )->data )) continue;
        }
        else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
            lxb_dom_element_t *child_element = lxb_dom_interface_element( node );
            lxb_dom_element_t *outer;

            if (!(child = compute( ctx, style, child_element )) || child->display == DISPLAY_NONE) continue;
            if (child->out_of_flow)
            {
                (void)outer;
                ctx->static_x = x;
                ctx->static_y = y;
                defer( ctx, child_element, child );
                continue;
            }
        }
        else continue;

        if (count == capacity)
        {
            unsigned int grown_capacity = capacity ? capacity * 2 : 16;
            struct flex_item *grown = realloc( items, grown_capacity * sizeof(*grown) );

            if (!grown) break;
            items = grown;
            capacity = grown_capacity;
        }
        item = &items[count];
        memset( item, 0, sizeof(*item) );
        item->index = count++;
        if (child)
        {
            item->element = lxb_dom_interface_element( node );
            item->style = child;
            item->order = child->order;
            for (int i = 0; i < 4; i++)
            {
                item->auto_margin[i] = child->margin[i].unit == LEN_AUTO;
                item->margin[i] = length_px( &child->margin[i], width );
            }
            item->frame_h = child->border[3] + child->padding[3] + child->padding[1] + child->border[1];
            item->frame_v = child->border[0] + child->padding[0] + child->padding[2] + child->border[2];
        }
        else
        {
            item->text = node;
            item->style = style;
        }
    }
    if (!count)
    {
        free( items );
        return fixed_height > 0 ? fixed_height : 0;
    }
    qsort( items, count, sizeof(*items), compare_items );
    if (reverse)
        for (unsigned int i = 0; i < count / 2; i++)
        {
            struct flex_item swap = items[i];
            items[i] = items[count - 1 - i];
            items[count - 1 - i] = swap;
        }

    if (column)
    {
        /* a column: blocks one under another, as wide as the container unless aligned otherwise */
        float cursor = 0, free_space;

        for (unsigned int i = 0; i < count; i++)
        {
            struct flex_item *item = &items[i];
            enum flex_align align = item->style->align_self != FALIGN_AUTO ? item->style->align_self : style->align_items;
            lxb_dom_element_t *outer = item->element ? enter_link( ctx, item->element ) : ctx->link;
            float w, h, avail = width - item->margin[1] - item->margin[3];

            if (align != FALIGN_STRETCH && !item->text && item->style->width.unit == LEN_AUTO)
            {
                lay_item( ctx, item, -1, -1, width, TRUE, &w );
                truncate_list( ctx, &item->from );
                h = lay_item( ctx, item, min( w, avail ), -1, width, FALSE, &w );
            }
            else h = lay_item( ctx, item, item->style->width.unit == LEN_AUTO && !item->text ? avail : -1, -1,
                               width, FALSE, &w );
            ctx->link = outer;
            item->main = h;
            item->cross = w;
            cursor += item->margin[0] + h + item->margin[2] + (i ? style->gap_row : 0);
        }
        free_space = fixed_height > cursor ? fixed_height - cursor : 0;
        {
            float offset = 0, between = 0;

            switch (style->justify)
            {
            case JUSTIFY_END:     offset = free_space; break;
            case JUSTIFY_CENTER:  offset = free_space / 2; break;
            case JUSTIFY_BETWEEN: between = count > 1 ? free_space / (count - 1) : 0; break;
            case JUSTIFY_AROUND:  between = free_space / count; offset = between / 2; break;
            case JUSTIFY_EVENLY:  between = free_space / (count + 1); offset = between; break;
            default: break;
            }
            for (unsigned int i = 0; i < count; i++)
            {
                struct flex_item *item = &items[i];
                enum flex_align align = item->style->align_self != FALIGN_AUTO ? item->style->align_self : style->align_items;
                float left = item->margin[3], room = width - item->margin[1] - item->margin[3] - item->cross;

                if (item->auto_margin[1] && item->auto_margin[3]) left += room / 2;
                else if (item->auto_margin[3]) left += room;
                else if (align == FALIGN_CENTER) left += room / 2;
                else if (align == FALIGN_END) left += room;
                if (i) offset += style->gap_row;
                shift_box( ctx, &item->from, &item->to, (int)(x + left - item->margin[3] + 0.5f),
                                    (int)(y + offset + 0.5f) );
                offset += item->margin[0] + item->main + item->margin[2] + between;
            }
        }
        free( items );
        return fixed_height >= 0 ? fixed_height : cursor;
    }

    /* a row: each item's own size first */
    for (unsigned int i = 0; i < count; i++)
    {
        struct flex_item *item = &items[i];
        const struct style *s = item->style;
        float w;

        if (!item->text && s->basis.unit != LEN_AUTO && !(s->basis.unit == LEN_PERCENT && width <= 0))
            item->hypothetical = length_px( &s->basis, width ) + (s->border_box ? 0 : item->frame_h);
        else if (!item->text && s->width.unit != LEN_AUTO)
            item->hypothetical = length_px( &s->width, width ) + (s->border_box ? 0 : item->frame_h);
        else
        {
            lay_item( ctx, item, -1, -1, width, TRUE, &w );
            truncate_list( ctx, &item->from );
            item->hypothetical = w;
        }
        if (!item->text)
        {
            if (s->max_width.unit != LEN_AUTO)
                item->hypothetical = min( item->hypothetical, length_px( &s->max_width, width ) +
                                          (s->border_box ? 0 : item->frame_h) );
            if (s->min_width.unit != LEN_AUTO)
                item->hypothetical = max( item->hypothetical, length_px( &s->min_width, width ) +
                                          (s->border_box ? 0 : item->frame_h) );
        }
        item->hypothetical = max( item->hypothetical, item->frame_h );
    }

    /* the lines, each flexed and laid out on its own */
    for (unsigned int first = 0; first < count; )
    {
        unsigned int end = first + 1, autos = 0;
        float used = items[first].hypothetical + items[first].margin[1] + items[first].margin[3];
        float free_space, grow = 0, shrink = 0, line_cross = 0, offset = 0, between = 0;

        if (style->flex_wrap)
            while (end < count && used + style->gap_column + items[end].hypothetical + items[end].margin[1] +
                   items[end].margin[3] <= width + 0.5f)
                used += style->gap_column + items[end].hypothetical + items[end].margin[1] + items[end].margin[3], end++;
        else
            for (; end < count; end++)
                used += style->gap_column + items[end].hypothetical + items[end].margin[1] + items[end].margin[3];

        free_space = width - used;
        for (unsigned int i = first; i < end; i++)
        {
            grow += items[i].text ? 0 : items[i].style->grow;
            shrink += (items[i].text ? 1 : items[i].style->shrink) * items[i].hypothetical;
            autos += items[i].auto_margin[1] + items[i].auto_margin[3];
        }
        for (unsigned int i = first; i < end; i++)
        {
            struct flex_item *item = &items[i];
            float factor = item->text ? 0 : item->style->grow;

            item->main = item->hypothetical;
            if (free_space > 0 && grow > 0)
                item->main += free_space * factor / max( grow, 1 );
            else if (free_space < 0 && shrink > 0)
                item->main += free_space * (item->text ? 1 : item->style->shrink) * item->hypothetical / shrink;
            if (!item->text && item->style->max_width.unit != LEN_AUTO)
                item->main = min( item->main, length_px( &item->style->max_width, width ) +
                                  (item->style->border_box ? 0 : item->frame_h) );
            if (!item->text && item->style->min_width.unit != LEN_AUTO)
                item->main = max( item->main, length_px( &item->style->min_width, width ) +
                                  (item->style->border_box ? 0 : item->frame_h) );
            item->main = max( item->main, item->frame_h );
        }
        /* what is left after growing goes to the auto margins, else to justify-content */
        used = 0;
        for (unsigned int i = first; i < end; i++)
            used += items[i].main + items[i].margin[1] + items[i].margin[3] + (i > first ? style->gap_column : 0);
        free_space = max( 0, width - used );
        if (autos)
        {
            for (unsigned int i = first; i < end; i++)
            {
                if (items[i].auto_margin[3]) items[i].margin[3] = free_space / autos;
                if (items[i].auto_margin[1]) items[i].margin[1] = free_space / autos;
            }
            free_space = 0;
        }
        switch (style->justify)
        {
        case JUSTIFY_END:     offset = free_space; break;
        case JUSTIFY_CENTER:  offset = free_space / 2; break;
        case JUSTIFY_BETWEEN: between = end - first > 1 ? free_space / (end - first - 1) : 0; break;
        case JUSTIFY_AROUND:  between = free_space / (end - first); offset = between / 2; break;
        case JUSTIFY_EVENLY:  between = free_space / (end - first + 1); offset = between; break;
        default: break;
        }
        if (reverse && style->justify == JUSTIFY_START) offset = free_space;    /* row-reverse starts at the right */

        /* each item at its size, then the line as tall as the tallest */
        for (unsigned int i = first; i < end; i++)
        {
            struct flex_item *item = &items[i];
            lxb_dom_element_t *outer = item->element ? enter_link( ctx, item->element ) : ctx->link;

            item->cross = lay_item( ctx, item, item->main, -1, width, FALSE, NULL );
            ctx->link = outer;
            line_cross = max( line_cross, item->margin[0] + item->cross + item->margin[2] );
        }
        if (!style->flex_wrap && fixed_height >= 0) line_cross = max( line_cross, fixed_height );

        /* stretched items take the line's height; the line is laid out again when that shows -
         * an item with a frame or a background */
        {
            BOOL again = FALSE;

            for (unsigned int i = first; i < end; i++)
                if (stretches( style, &items[i], line_cross )) again = TRUE;
            if (again)
            {
                truncate_list( ctx, &items[first].from );
                for (unsigned int i = first; i < end; i++)
                {
                    struct flex_item *item = &items[i];
                    lxb_dom_element_t *outer = item->element ? enter_link( ctx, item->element ) : ctx->link;
                    BOOL stretch = stretches( style, item, line_cross );

                    item->cross = lay_item( ctx, item, item->main,
                                            stretch ? line_cross - item->margin[0] - item->margin[2] : -1,
                                            width, FALSE, NULL );
                    ctx->link = outer;
                }
            }
        }

        for (unsigned int i = first; i < end; i++)
        {
            struct flex_item *item = &items[i];
            enum flex_align align = item->style->align_self != FALIGN_AUTO && !item->text ? item->style->align_self
                                                                                        : style->align_items;
            float room = line_cross - item->margin[0] - item->cross - item->margin[2], top = 0;

            if (item->auto_margin[0] && item->auto_margin[2]) top = room / 2;
            else if (item->auto_margin[0]) top = room;
            else switch (align)
            {
            case FALIGN_CENTER: top = room / 2; break;
            case FALIGN_END:    top = room; break;
            default:            break;
            }
            shift_box( ctx, &item->from, &item->to, (int)(x + offset + 0.5f),
                                (int)(y + total + top + 0.5f) );
            offset += item->margin[3] + item->main + item->margin[1] + between + style->gap_column;
        }
        total += line_cross + (end < count ? style->gap_row : 0);
        first = end;
    }
    free( items );
    return fixed_height >= 0 ? max( fixed_height, total ) : total;
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

/* --- positioned boxes --- */

/* An absolute box against its containing block's padding box: its offsets
 * where it has them, where it would have stood in the flow where it does
 * not; as wide as its content unless both sides or a width say otherwise. */
static void place_absolute( struct layout *ctx, const struct deferred *entry, float cb_x, float cb_y, float cb_w,
                            float cb_h )
{
    const struct style *style = entry->style;
    lxb_dom_element_t *outer = ctx->link;
    struct style *copy = new_style( ctx );
    float ml = length_px( &style->margin[3], cb_w ), mr = length_px( &style->margin[1], cb_w );
    float mt = length_px( &style->margin[0], cb_w ), mb = length_px( &style->margin[2], cb_w );
    float frame_h = style->border[3] + style->padding[3] + style->padding[1] + style->border[1];
    float frame_v = style->border[0] + style->padding[0] + style->padding[2] + style->border[2];
    BOOL has[4];
    float inset[4], height, width, dx, dy;
    struct list_mark from, to;

    if (!copy) return;
    *copy = *style;
    ctx->list->absolute++;
    for (int i = 0; i < 4; i++)
    {
        has[i] = style->inset[i].unit != LEN_AUTO;
        inset[i] = length_px( &style->inset[i], i == 0 || i == 2 ? cb_h : cb_w );
    }
    if (copy->width.unit == LEN_AUTO)
    {
        copy->border_box = FALSE;
        if (copy->height.unit == LEN_PX && style->border_box) copy->height.value -= frame_v;
        copy->width.unit = LEN_PX;
        if (has[1] && has[3]) copy->width.value = max( 0, cb_w - inset[1] - inset[3] - ml - mr - frame_h );
        else
        {
            /* shrink to fit: laid out once with its lines to the left, to see how wide it is */
            struct style *measure = new_style( ctx );
            struct list_mark now;
            float inner, right;

            copy->width.value = max( 0, cb_w - ml - mr - frame_h );
            if (measure)
            {
                *measure = *copy;
                measure->text_align = ALIGN_LEFT;
                mark_at( ctx, &from );
                layout_block( ctx, entry->element, measure, 0, 0, cb_w, 0 );
                mark_at( ctx, &now );
                inner = ml + style->border[3] + style->padding[3];
                right = inner;
                for (unsigned int i = from.items; i < now.items; i++)
                    if (ctx->list->items[i].type != ITEM_RECT && ctx->list->items[i].absolute == ctx->list->absolute)
                        right = max( right, (float)ctx->list->items[i].rect.right );
                for (unsigned int i = from.controls; i < now.controls; i++)
                    right = max( right, (float)ctx->list->controls[i].rect.right );
                truncate_list( ctx, &from );
                copy->width.value = ceilf( min( right - inner, copy->width.value ) );
            }
        }
    }
    if (copy->height.unit == LEN_AUTO && has[0] && has[2])
    {
        copy->height.unit = LEN_PX;
        copy->height.value = max( 0, cb_h - inset[0] - inset[2] - mt - mb - (copy->border_box ? 0 : frame_v) );
    }
    copy->margin[1].unit = copy->margin[3].unit = LEN_PX;  /* no centring by auto margins here */
    copy->margin[1].value = mr;
    copy->margin[3].value = ml;

    ctx->link = entry->link;
    enter_link( ctx, entry->element );
    mark_at( ctx, &from );
    height = layout_block( ctx, entry->element, copy, 0, mt, cb_w, 0 );
    width = ctx->last_width;
    mark_at( ctx, &to );
    ctx->link = outer;
    ctx->list->absolute--;

    if (has[3]) dx = cb_x + inset[3];
    else if (has[1]) dx = cb_x + cb_w - inset[1] - mr - width - ml;
    else dx = entry->static_x;
    if (has[0]) dy = cb_y + inset[0];
    else if (has[2]) dy = cb_y + cb_h - inset[2] - mb - height - mt;
    else dy = entry->static_y;
    shift_box( ctx, &from, &to, (int)floorf( dx + 0.5f ), (int)floorf( dy + 0.5f ) );
}

/* the boxes waiting since first that have this containing block; fixed ones
 * wait for the viewport, at the end, unless the viewport is what this is */
static void place_deferred( struct layout *ctx, unsigned int first, float cb_x, float cb_y, float cb_w, float cb_h,
                            BOOL viewport )
{
    unsigned int kept = first;

    for (unsigned int i = first; i < ctx->deferred_count; i++)
    {
        struct deferred entry = ctx->deferred[i];

        if (entry.style->position == POSITION_FIXED && !viewport)
        {
            ctx->deferred[kept++] = entry;
            continue;
        }
        place_absolute( ctx, &entry, cb_x, cb_y, cb_w, cb_h );
    }
    /* what the placed ones deferred themselves they have placed: only the fixed stay */
    ctx->deferred_count = min( kept, ctx->deferred_count );
}

static float layout_block( struct layout *ctx, lxb_dom_element_t *element, const struct style *style,
                           float x, float y, float containing, int list_index )
{
    unsigned int waiting = ctx->deferred_count;
    int outer_layer = ctx->list->layer;
    lxb_dom_element_t *outer_owner = ctx->list->owner;
    struct list_mark start;
    float ml = length_px( &style->margin[3], containing ), mr = length_px( &style->margin[1], containing );
    float frame_h = style->border[3] + style->padding[3] + style->padding[1] + style->border[1];
    float frame_v_top = style->border[0] + style->padding[0];
    float frame_v_bottom = style->padding[2] + style->border[2];
    float content_w, content_h, box_x, total;
    unsigned int background, borders;

    float sizing = style->border_box ? frame_h : 0;    /* what of the frame a width already counts */
    BOOL fixed = style->width.unit != LEN_AUTO;

    if (!fixed) content_w = containing - ml - mr - frame_h;
    else content_w = length_px( &style->width, containing ) - sizing;
    if (style->max_width.unit != LEN_AUTO && content_w > length_px( &style->max_width, containing ) - sizing)
    {
        content_w = length_px( &style->max_width, containing ) - sizing;
        fixed = TRUE;
    }
    if (style->min_width.unit != LEN_AUTO && content_w < length_px( &style->min_width, containing ) - sizing)
    {
        content_w = length_px( &style->min_width, containing ) - sizing;
        fixed = TRUE;
    }
    if (content_w < 0) content_w = 0;
    if (fixed)
    {
        /* margin: 0 auto centres a block of a set width */
        if (style->margin[1].unit == LEN_AUTO && style->margin[3].unit == LEN_AUTO)
            ml = mr = max( 0, (containing - content_w - frame_h) / 2 );
        else if (style->margin[3].unit == LEN_AUTO)
            ml = max( 0, containing - content_w - frame_h - mr );
    }
    box_x = x + ml;
    if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"id", 2 ))
        display_list_anchor( ctx->list, (int)y, element );
    mark_at( ctx, &start );
    ctx->list->owner = element;
    if (style->position != POSITION_STATIC) ctx->list->layer = 1;   /* over the flow */

    /* the background and the borders are drawn under the content: their places are taken now */
    background = ctx->list->count;
    if (paints( style )) emit_rect( ctx, 0, 0, 1, 1, style->background );
    background = ctx->list->count > background ? background : ~0u;
    borders = ctx->list->count;
    for (int i = 0; i < 4; i++)
        emit_rect( ctx, 0, 0, 1, 1, style->border[i] > 0 && paints( style ) ? style->border_color[i] : 0 );

    if (style->display == DISPLAY_LIST_ITEM && paints( style ))
        emit_marker( ctx, style, box_x + style->border[3] + style->padding[3], y + frame_v_top, list_index );

    if (style->flex)
        content_h = layout_flex( ctx, element, style, box_x + style->border[3] + style->padding[3], y + frame_v_top,
                                 content_w );
    else content_h = flow_children( ctx, lxb_dom_interface_node( element ), style, box_x + style->border[3] + style->padding[3],
                               y + frame_v_top, content_w );
    if (style->height.unit == LEN_PX)
        content_h = max( 0, style->height.value - (style->border_box ? frame_v_top + frame_v_bottom : 0) );
    total = frame_v_top + content_h + frame_v_bottom;
    debug_outline( element, style, box_x, y, content_w, total );
    ctx->last_width = content_w + frame_h;
    ctx->last_margin[0] = ml;
    ctx->last_margin[1] = mr;

    if (background != ~0u)
        SetRect( &ctx->list->items[background].rect, (int)box_x, (int)y, (int)ceilf( box_x + content_w + frame_h ),
                 (int)ceilf( y + total ) );
    for (int i = 0, item = borders; i < 4; i++)
    {
        float right = box_x + content_w + frame_h, bottom = y + total;
        RECT *rect;

        if (!(style->border[i] > 0 && (style->border_color[i] >> 24) && paints( style ))) continue;
        rect = &ctx->list->items[item++].rect;
        switch (i)
        {
        case 0: SetRect( rect, (int)box_x, (int)y, (int)ceilf( right ), (int)ceilf( y + style->border[0] ) ); break;
        case 1: SetRect( rect, (int)(right - style->border[1]), (int)y, (int)ceilf( right ), (int)ceilf( bottom ) ); break;
        case 2: SetRect( rect, (int)box_x, (int)(bottom - style->border[2]), (int)ceilf( right ), (int)ceilf( bottom ) ); break;
        case 3: SetRect( rect, (int)box_x, (int)y, (int)ceilf( box_x + style->border[3] ), (int)ceilf( bottom ) ); break;
        }
    }

    if (style->clips && lxb_dom_element_tag_id( element ) != LXB_TAG_HTML &&
        lxb_dom_element_tag_id( element ) != LXB_TAG_BODY)
    {
        /* overflow: hidden and the like: what its content draws outside its padding box is cut off */
        struct list_mark now;
        RECT box;

        mark_at( ctx, &now );
        SetRect( &box, (int)floorf( box_x + style->border[3] ), (int)floorf( y + style->border[0] ),
                 (int)ceilf( box_x + content_w + frame_h - style->border[1] ), (int)ceilf( y + total - style->border[2] ) );
        for (unsigned int i = start.items; i < now.items; i++)
        {
            struct item *item = &ctx->list->items[i];

            if (item->clipped) IntersectRect( &item->clip, &item->clip, &box );
            else
            {
                item->clip = box;
                item->clipped = TRUE;
            }
        }
        for (unsigned int i = start.links; i < now.links; i++)
            IntersectRect( &ctx->list->links[i].rect, &ctx->list->links[i].rect, &box );
    }
    if (style->position != POSITION_STATIC)
    {
        struct list_mark now;

        /* its absolute descendants go against its padding box */
        place_deferred( ctx, waiting, box_x + style->border[3], y + style->border[0],
                        content_w + style->padding[3] + style->padding[1],
                        content_h + style->padding[0] + style->padding[2], FALSE );
        if (style->position == POSITION_RELATIVE)
        {
            /* moved from where it stands, the flow keeping its place */
            float dx = style->inset[3].unit != LEN_AUTO ? length_px( &style->inset[3], containing ) :
                       style->inset[1].unit != LEN_AUTO ? -length_px( &style->inset[1], containing ) : 0;
            float dy = style->inset[0].unit != LEN_AUTO ? length_px( &style->inset[0], containing ) :
                       style->inset[2].unit != LEN_AUTO ? -length_px( &style->inset[2], containing ) : 0;

            mark_at( ctx, &now );
            if (dx || dy) shift_box( ctx, &start, &now, (int)dx, (int)dy );
        }
        ctx->list->layer = outer_layer;
    }
    ctx->list->owner = outer_owner;
    ctx->last_width = content_w + frame_h;  /* as the box itself came out, not its last descendant */
    ctx->last_margin[0] = ml;
    ctx->last_margin[1] = mr;
    return total;
}

/* --- the document --- */

void layout_document( struct document *doc, struct display_list *list, int width, int height )
{
    struct layout ctx = { .doc = doc, .list = list, .line_width = (float)width };
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

    ctx.viewport_w = (float)width;
    ctx.viewport_h = (float)height;
    content = layout_block( &ctx, root, style, 0, length_px( &style->margin[0], (float)width ), (float)width, 0 );
    list->content_height = (int)ceilf( content + length_px( &style->margin[0], (float)width ) +
                                       length_px( &style->margin[2], (float)width ) );
    /* what has no positioned ancestor goes against the initial containing block: the viewport */
    list->layer = 1;
    place_deferred( &ctx, 0, 0, 0, (float)width, (float)height, TRUE );
    list->layer = 0;
    free( ctx.deferred );

    for (unsigned int i = 0; i < ctx.style_count; i++) free( ctx.styles[i] );
    free( ctx.styles );
    free( ctx.atoms );
    free( ctx.text );
    TRACE( "%u items, %u styles, %d px tall, in %I64u ms\n", list->count, ctx.style_count, list->content_height,
           GetTickCount64() - start );
}
