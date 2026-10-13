/*
 * Arctic web engine: what CSS needs before lexbor sees it
 *
 * lexbor parses selectors and longhands well, but its style module leaves
 * out what nearly every site of today is written with, and drops it before
 * any code of ours sees it:
 *
 * - @media: rules inside a media query are never applied. Here the queries
 *   are evaluated for the viewport the page opens in - screen, widths with
 *   calc(), ranges, prefers-color-scheme: light, orientation - and the rules
 *   of the ones that hold move to the top; the rest go. @supports and
 *   @layer open as if they held; @font-face, @keyframes and the like go.
 * - var(): custom properties. The ones a page sets for the whole document
 *   (:root, html, body), in the media that hold, are collected from all its
 *   sheets first; then every var(--x, fallback) is replaced by its value.
 *   One set on a class takes the document's value - a limit of this stage.
 * - the shorthands background and font: the longhands they stand for go
 *   after them, with the same !important, the omitted ones reset as CSS
 *   wants (background: url(x) makes the colour transparent).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arctichtml_private.h"

#define MAX_VAR_DEPTH 8

struct text
{
    char   *data;
    size_t  len, size;
    BOOL    failed;
};

static void put( struct text *out, const char *s, size_t len )
{
    if (out->failed) return;
    if (out->len + len + 1 > out->size)
    {
        size_t size = max( out->size * 2, out->len + len + 256 );
        char *grown = realloc( out->data, size );

        if (!grown) { out->failed = TRUE; return; }
        out->data = grown;
        out->size = size;
    }
    memcpy( out->data + out->len, s, len );
    out->len += len;
    out->data[out->len] = 0;
}

static void puts_( struct text *out, const char *s )
{
    put( out, s, strlen( s ) );
}

static BOOL is_blank( char c )
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void trim( const char **s, const char **end )
{
    while (*s < *end && is_blank( **s )) (*s)++;
    while (*end > *s && is_blank( (*end)[-1] )) (*end)--;
}

static BOOL word_is( const char *s, size_t len, const char *word )
{
    return strlen( word ) == len && !_strnicmp( s, word, len );
}

/* past a comment, a string, or one character */
static const char *skip_one( const char *s, const char *end )
{
    if (*s == '/' && s + 1 < end && s[1] == '*')
    {
        const char *close = s + 2;

        while (close + 1 < end && !(close[0] == '*' && close[1] == '/')) close++;
        return close + 1 < end ? close + 2 : end;
    }
    if (*s == '"' || *s == '\'')
    {
        char quote = *s++;

        while (s < end && *s != quote)
        {
            if (*s == '\\' && s + 1 < end) s++;
            s++;
        }
        return s < end ? s + 1 : end;
    }
    return s + 1;
}

/* the end of what goes on until one of stops at depth 0 of parentheses */
static const char *scan_to( const char *s, const char *end, const char *stops )
{
    int depth = 0;

    while (s < end)
    {
        /* a stop counts at depth 0 - the ) that closes what s is inside too */
        if (depth <= 0 && strchr( stops, *s )) return s;
        if (*s == '(') depth++;
        else if (*s == ')') depth--;
        s = skip_one( s, end );
    }
    return end;
}

/* the } that closes the { at s */
static const char *matching_brace( const char *s, const char *end )
{
    int depth = 0;

    while (s < end)
    {
        if (*s == '{') depth++;
        else if (*s == '}' && --depth == 0) return s;
        s = skip_one( s, end );
    }
    return end;
}

/* --- the context: the viewport and the document's custom properties --- */

struct css_var
{
    char *name, *value;
};

void css_context_init( struct css_context *ctx, int width, int height )
{
    memset( ctx, 0, sizeof(*ctx) );
    ctx->width = width > 0 ? width : 1024;
    ctx->height = height > 0 ? height : 768;
}

void css_context_free( struct css_context *ctx )
{
    for (unsigned int i = 0; i < ctx->var_count; i++)
    {
        free( ctx->vars[i].name );
        free( ctx->vars[i].value );
    }
    free( ctx->vars );
    memset( ctx, 0, sizeof(*ctx) );
}

static const struct css_var *find_var( const struct css_context *ctx, const char *name, size_t len )
{
    /* the last one set wins, as in the cascade of equal selectors */
    for (unsigned int i = ctx->var_count; i--; )
        if (strlen( ctx->vars[i].name ) == len && !memcmp( ctx->vars[i].name, name, len )) return &ctx->vars[i];
    return NULL;
}

static void set_var( struct css_context *ctx, const char *name, size_t name_len, const char *value, size_t value_len )
{
    struct css_var *var;

    if (ctx->var_count == ctx->var_capacity)
    {
        unsigned int capacity = ctx->var_capacity ? ctx->var_capacity * 2 : 256;
        struct css_var *grown = realloc( ctx->vars, capacity * sizeof(*grown) );

        if (!grown) return;
        ctx->vars = grown;
        ctx->var_capacity = capacity;
    }
    var = &ctx->vars[ctx->var_count];
    if (!(var->name = malloc( name_len + 1 )) || !(var->value = malloc( value_len + 1 )))
    {
        free( var->name );
        return;
    }
    memcpy( var->name, name, name_len );
    var->name[name_len] = 0;
    memcpy( var->value, value, value_len );
    var->value[value_len] = 0;
    ctx->var_count++;
}

/* --- media queries --- */

/* a length of a media feature in px: 600px, 40em, calc(1120px - 1px) */
static BOOL media_length( const char *s, const char *end, float *px )
{
    char buffer[64], *p;
    size_t len;
    float total = 0, sign = 1;

    trim( &s, &end );
    if (end - s > 5 && !_strnicmp( s, "calc(", 5 ) && end[-1] == ')')
    {
        s += 5;
        end--;
    }
    len = min( (size_t)(end - s), sizeof(buffer) - 1 );
    memcpy( buffer, s, len );
    buffer[len] = 0;
    for (p = buffer; *p; )
    {
        float value;
        char *unit;

        while (is_blank( *p )) p++;
        if (*p == '+') { sign = 1; p++; continue; }
        if (*p == '-' && (p[1] == ' ' || p[1] == '\t')) { sign = -1; p++; continue; }
        value = strtof( p, &unit );
        if (unit == p) return FALSE;
        if (!_strnicmp( unit, "em", 2 ) || !_strnicmp( unit, "rem", 3 )) value *= 16;
        total += sign * value;
        sign = 1;
        p = unit;
        while (*p && !is_blank( *p ) && *p != '+') p++;
    }
    *px = total;
    return TRUE;
}

/* one (feature: value) or (width >= 600px) */
static BOOL media_feature( const struct css_context *ctx, const char *s, const char *end )
{
    const char *colon, *name_end, *value;
    float px, have;

    trim( &s, &end );
    if (s < end && *s == '(') s++;
    if (end > s && end[-1] == ')') end--;
    trim( &s, &end );

    /* the range syntax: width >= 600px, 600px <= width */
    for (const char *p = s; p < end; p++)
    {
        BOOL less = *p == '<', greater = *p == '>';
        BOOL equal = p + 1 < end && p[1] == '=';
        const char *left_end = p, *right = p + (equal ? 2 : 1);
        const char *left = s;

        if (!less && !greater) continue;
        trim( &left, &left_end );
        if (word_is( left, left_end - left, "width" ) || word_is( left, left_end - left, "height" ))
        {
            have = word_is( left, left_end - left, "width" ) ? (float)ctx->width : (float)ctx->height;
            if (!media_length( right, end, &px )) return FALSE;
        }
        else
        {
            const char *feature = right, *feature_end = end;

            trim( &feature, &feature_end );
            if (!word_is( feature, feature_end - feature, "width" ) && !word_is( feature, feature_end - feature, "height" ))
                return FALSE;
            have = word_is( feature, feature_end - feature, "width" ) ? (float)ctx->width : (float)ctx->height;
            if (!media_length( left, left_end, &px )) return FALSE;
            /* 600px <= width: the same as width >= 600px */
            less = !less;
            greater = !greater;
        }
        if (less) return equal ? have <= px : have < px;
        return equal ? have >= px : have > px;
    }

    if (!(colon = memchr( s, ':', end - s )))
    {
        /* (hover), (color): what this screen has */
        return word_is( s, end - s, "hover" ) || word_is( s, end - s, "color" ) || word_is( s, end - s, "pointer" );
    }
    name_end = colon;
    value = colon + 1;
    trim( &s, &name_end );
    trim( &value, &end );

#define NAME(n) word_is( s, name_end - s, n )
#define VALUE(v) word_is( value, end - value, v )
    if (NAME( "min-width" )) return media_length( value, end, &px ) && ctx->width >= px;
    if (NAME( "max-width" )) return media_length( value, end, &px ) && ctx->width <= px;
    if (NAME( "width" )) return media_length( value, end, &px ) && ctx->width == (int)px;
    if (NAME( "min-height" )) return media_length( value, end, &px ) && ctx->height >= px;
    if (NAME( "max-height" )) return media_length( value, end, &px ) && ctx->height <= px;
    if (NAME( "prefers-color-scheme" )) return VALUE( "light" );
    if (NAME( "prefers-reduced-motion" ) || NAME( "prefers-reduced-transparency" ) || NAME( "prefers-contrast" ) ||
        NAME( "forced-colors" ) || NAME( "inverted-colors" ))
        return VALUE( "no-preference" ) || VALUE( "none" );
    if (NAME( "orientation" )) return VALUE( ctx->width >= ctx->height ? "landscape" : "portrait" );
    if (NAME( "hover" ) || NAME( "any-hover" )) return VALUE( "hover" );
    if (NAME( "pointer" ) || NAME( "any-pointer" )) return VALUE( "fine" );
    if (NAME( "min-resolution" ) || NAME( "-webkit-min-device-pixel-ratio" ) || NAME( "min--moz-device-pixel-ratio" ))
        return strtof( value, NULL ) <= 1.0f;
    if (NAME( "max-resolution" ) || NAME( "-webkit-max-device-pixel-ratio" )) return strtof( value, NULL ) >= 1.0f;
    if (NAME( "display-mode" )) return VALUE( "browser" );
    if (NAME( "scripting" )) return VALUE( "none" );   /* until stage 4 */
#undef NAME
#undef VALUE
    return FALSE;
}

/* one query of a list: [not|only] [type] [and (feature)]... */
static BOOL media_query( const struct css_context *ctx, const char *s, const char *end )
{
    BOOL negate = FALSE, result = TRUE;

    trim( &s, &end );
    if (end - s > 4 && !_strnicmp( s, "not ", 4 )) { negate = TRUE; s += 4; }
    else if (end - s > 5 && !_strnicmp( s, "only ", 5 )) s += 5;
    trim( &s, &end );

    while (s < end && result)
    {
        const char *item_end;

        if (*s == '(')
        {
            int depth = 0;

            for (item_end = s; item_end < end; item_end++)
            {
                if (*item_end == '(') depth++;
                else if (*item_end == ')' && --depth == 0) { item_end++; break; }
            }
            result = media_feature( ctx, s, item_end );
        }
        else
        {
            for (item_end = s; item_end < end && !is_blank( *item_end ) && *item_end != '('; item_end++);
            if (word_is( s, item_end - s, "and" )) { /* between the parts */ }
            else if (word_is( s, item_end - s, "screen" ) || word_is( s, item_end - s, "all" )) { }
            else result = FALSE;   /* print, speech, and what is not known */
        }
        s = item_end;
        while (s < end && is_blank( *s )) s++;
    }
    return negate ? !result : result;
}

static BOOL media_matches( const struct css_context *ctx, const char *s, const char *end )
{
    trim( &s, &end );
    if (s == end) return TRUE;
    while (s < end)
    {
        const char *comma = scan_to( s, end, "," );

        if (media_query( ctx, s, comma )) return TRUE;
        s = comma < end ? comma + 1 : end;
    }
    return FALSE;
}

/* --- var() --- */

static BOOL substitute( const struct css_context *ctx, struct text *out, const char *s, const char *end, int depth )
{
    while (s < end)
    {
        const char *name, *name_end, *close, *comma;
        const struct css_var *var;

        if (end - s < 4 || _strnicmp( s, "var(", 4 ))
        {
            const char *next = skip_one( s, end );

            put( out, s, next - s );
            s = next;
            continue;
        }
        close = scan_to( s + 4, end, ")" );
        comma = scan_to( s + 4, close, "," );
        name = s + 4;
        name_end = comma;
        trim( &name, &name_end );
        if (depth < MAX_VAR_DEPTH && (var = find_var( ctx, name, name_end - name )) && var->value[0])
        {
            if (!substitute( ctx, out, var->value, var->value + strlen( var->value ), depth + 1 )) return FALSE;
        }
        else if (comma < close)
        {
            if (!substitute( ctx, out, comma + 1, close, depth + 1 )) return FALSE;
        }
        else return FALSE;  /* no value and no fallback: the declaration is invalid */
        s = close < end ? close + 1 : end;
    }
    return TRUE;
}

/* --- shorthands --- */

static const char *next_token( const char *s, const char *end, const char **start )
{
    int depth = 0;
    char quote = 0;

    while (s < end && is_blank( *s )) s++;
    *start = s;
    while (s < end)
    {
        if (quote) { if (*s == quote) quote = 0; }
        else if (*s == '"' || *s == '\'') quote = *s;
        else if (*s == '(') depth++;
        else if (*s == ')') depth--;
        else if (!depth && is_blank( *s )) break;
        s++;
    }
    return s;
}

static BOOL contains( const char *s, size_t len, const char *word )
{
    size_t word_len = strlen( word );

    for (size_t i = 0; i + word_len <= len; i++) if (!_strnicmp( s + i, word, word_len )) return TRUE;
    return FALSE;
}

static BOOL looks_like_color( const char *s, size_t len )
{
    static const char *const not_colors[] =
    {
        "none", "repeat", "no-repeat", "repeat-x", "repeat-y", "space", "round", "scroll", "fixed", "local",
        "top", "bottom", "left", "right", "center", "cover", "contain", "auto", "border-box", "padding-box",
        "content-box", "text", "inherit", "initial", "unset",
    };

    if (!len) return FALSE;
    if (s[0] == '#') return TRUE;
    if ((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '.' || s[0] == '/') return FALSE;
    if (!_strnicmp( s, "url(", 4 ) || contains( s, len, "gradient" ) ||
        !_strnicmp( s, "image-set", 9 ) || !_strnicmp( s, "var(", 4 ))
        return FALSE;
    for (unsigned int i = 0; i < ARRAY_SIZE(not_colors); i++) if (word_is( s, len, not_colors[i] )) return FALSE;
    return TRUE;    /* a name, rgb(), hsl(), transparent, currentcolor: lexbor tells if it is one */
}

static void expand_background( struct text *out, const char *value, const char *end, BOOL important )
{
    const char *color = NULL, *start, *s = value;
    size_t color_len = 0;

    if (word_is( value, end - value, "inherit" )) return;
    /* the colour belongs to the last layer: after the last comma outside parentheses */
    for (const char *p = value; p < end; p = skip_one( p, end ))
    {
        if (*p == '(') { p = scan_to( p + 1, end, ")" ); if (p >= end) break; }
        else if (*p == ',') s = p + 1;
    }
    while (s < end)
    {
        s = next_token( s, end, &start );
        if (s > start && looks_like_color( start, s - start ))
        {
            color = start;
            color_len = s - start;
        }
    }
    puts_( out, ";background-color:" );
    if (color) put( out, color, color_len );
    else puts_( out, "transparent" );
    if (important) puts_( out, " !important" );
}

static BOOL is_font_size( const char *s, size_t len )
{
    static const char *const keywords[] =
    {
        "xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large", "smaller", "larger",
    };

    if (len && ((s[0] >= '0' && s[0] <= '9') || s[0] == '.')) return TRUE;
    for (unsigned int i = 0; i < ARRAY_SIZE(keywords); i++)
    {
        size_t k = strlen( keywords[i] );

        if (len >= k && !_strnicmp( s, keywords[i], k ) && (len == k || s[k] == '/')) return TRUE;
    }
    return FALSE;
}

static void expand_font( struct text *out, const char *value, const char *end, BOOL important )
{
    const char *style = "normal", *weight = "normal", *start, *s = value;
    const char *size = NULL, *line_height = NULL, *family = NULL;
    size_t style_len = 6, weight_len = 6, size_len = 0, line_height_len = 0;
    const char *bang = important ? " !important" : "";

    /* the keywords of the system's fonts: what the dialogs of Windows use */
    if (word_is( value, end - value, "caption" ) || word_is( value, end - value, "menu" ) ||
        word_is( value, end - value, "message-box" ) || word_is( value, end - value, "status-bar" ) ||
        word_is( value, end - value, "icon" ) || word_is( value, end - value, "small-caption" ))
    {
        puts_( out, ";font-style:normal;font-weight:normal;font-size:13px;line-height:normal;font-family:system-ui" );
        return;
    }
    if (word_is( value, end - value, "inherit" )) return;

    while (s < end)
    {
        const char *token_end = next_token( s, end, &start );

        if (token_end == start) break;
        if (is_font_size( start, token_end - start ))
        {
            const char *slash = memchr( start, '/', token_end - start ), *peek, *peek_end, *lh, *lh_end;

            size = start;
            size_len = (slash ? slash : token_end) - start;
            s = token_end;
            if (slash && slash + 1 < token_end)                     /* 14px/1.5 */
            {
                line_height = slash + 1;
                line_height_len = token_end - line_height;
            }
            else
            {
                peek_end = next_token( s, end, &peek );
                if (!slash && peek_end - peek > 1 && *peek == '/')  /* 14px /1.5 */
                {
                    line_height = peek + 1;
                    line_height_len = peek_end - line_height;
                    s = peek_end;
                }
                else if (slash || (peek_end > peek && *peek == '/'))  /* 14px/ 1.5, 14px / 1.5 */
                {
                    if (!slash) s = peek_end;
                    if ((lh_end = next_token( s, end, &lh )) > lh)
                    {
                        line_height = lh;
                        line_height_len = lh_end - lh;
                        s = lh_end;
                    }
                }
            }
            while (s < end && is_blank( *s )) s++;
            family = s;     /* everything after the size is the family list */
            break;
        }
        if (word_is( start, token_end - start, "italic" ) || word_is( start, token_end - start, "oblique" ))
        {
            style = start;
            style_len = token_end - start;
        }
        else if (word_is( start, token_end - start, "bold" ) || word_is( start, token_end - start, "bolder" ) ||
                 word_is( start, token_end - start, "lighter" ) || (start[0] >= '1' && start[0] <= '9'))
        {
            weight = start;
            weight_len = token_end - start;
        }
        s = token_end;
    }
    if (!size || !family || family >= end) return;     /* not a valid font: nothing, as CSS says */

    puts_( out, ";font-style:" ); put( out, style, style_len ); puts_( out, bang );
    puts_( out, ";font-weight:" ); put( out, weight, weight_len ); puts_( out, bang );
    puts_( out, ";font-size:" ); put( out, size, size_len ); puts_( out, bang );
    puts_( out, ";line-height:" );
    if (line_height) put( out, line_height, line_height_len ); else puts_( out, "normal" );
    puts_( out, bang );
    puts_( out, ";font-family:" ); put( out, family, end - family ); puts_( out, bang );
}

/* what lexbor has no property for goes under a custom name of ours, which it
 * keeps and cascades like any --property: style.c reads it back by that name */
static const char *const foreign_properties[] =
{
    "list-style-type",
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
};

static BOOL is_foreign( const char *name, size_t len )
{
    for (unsigned int i = 0; i < ARRAY_SIZE(foreign_properties); i++)
        if (word_is( name, len, foreign_properties[i] )) return TRUE;
    return FALSE;
}

/* the shorthands of four sides lexbor does not know: one to four values, as margin */
static BOOL is_side_shorthand( const char *name, size_t len )
{
    return word_is( name, len, "border-width" ) || word_is( name, len, "border-style" ) ||
           word_is( name, len, "border-color" );
}

static void expand_sides_shorthand( struct text *out, const char *name, size_t name_len, const char *value,
                                    const char *end, BOOL important )
{
    static const char *const sides[4] = { "top", "right", "bottom", "left" };
    const char *start[4], *stop[4], *s = value;
    const char *what = name + 7;    /* past "border-" */
    BOOL ours = !word_is( name, name_len, "border-color" );     /* lexbor has the colours of the sides */
    int count = 0;

    while (s < end && count < 4)
    {
        s = next_token( s, end, &start[count] );
        if (s == start[count]) break;
        stop[count++] = s;
    }
    if (!count) return;
    for (int i = count; i < 4; i++)
    {
        int from = i == 3 && count >= 2 ? 1 : 0;    /* left as right, else as top; bottom as top */

        start[i] = start[from];
        stop[i] = stop[from];
    }
    for (int i = 0; i < 4; i++)
    {
        puts_( out, ours ? ";--arctic-border-" : ";border-" );
        puts_( out, sides[i] );
        puts_( out, "-" );
        put( out, what, name_len - 7 );
        puts_( out, ":" );
        put( out, start[i], stop[i] - start[i] );
        if (important) puts_( out, " !important" );
    }
}

/* list-style: its type, the one part we draw */
static void expand_list_style( struct text *out, const char *value, const char *end, BOOL important )
{
    const char *start, *s = value, *type = NULL;
    size_t type_len = 0;

    while (s < end)
    {
        s = next_token( s, end, &start );
        if (s == start) break;
        if (!_strnicmp( start, "url(", 4 ) || word_is( start, s - start, "inside" ) ||
            word_is( start, s - start, "outside" ))
            continue;
        if (!type || !word_is( start, s - start, "none" ))  /* "none url(...)": the image is not drawn either */
        {
            type = start;
            type_len = s - start;
        }
    }
    puts_( out, ";--arctic-list-style-type:" );
    if (type) put( out, type, type_len ); else puts_( out, "disc" );
    if (important) puts_( out, " !important" );
}

/* overflow: lexbor knows only the two axes */
static void expand_overflow( struct text *out, const char *value, const char *end, BOOL important )
{
    const char *x, *y, *s = next_token( value, end, &x ), *x_end = s, *y_end;

    y_end = next_token( s, end, &y );
    if (y_end == y) { y = x; y_end = x_end; }
    puts_( out, ";overflow-x:" ); put( out, x, x_end - x ); if (important) puts_( out, " !important" );
    puts_( out, ";overflow-y:" ); put( out, y, y_end - y ); if (important) puts_( out, " !important" );
}

/* --- declarations --- */

/* one declaration of a block: as it is, var() replaced, the longhands of a
 * shorthand after it; inline: only what lexbor did not have already */
static void rewrite_declaration( const struct css_context *ctx, struct text *out, const char *decl, const char *end,
                                 BOOL inline_only, BOOL masked )
{
    const char *colon = memchr( decl, ':', end - decl ), *name = decl, *name_end, *value, *value_end;
    struct text resolved = { 0 };
    BOOL important = FALSE, has_var, shorthand;

    if (!colon) return;
    name_end = colon;
    trim( &name, &name_end );
    value = colon + 1;
    value_end = end;
    trim( &value, &value_end );
    if (name_end - name >= 2 && name[0] == '-' && name[1] == '-')
    {
        if (!inline_only) { put( out, decl, end - decl ); puts_( out, ";" ); }
        return;
    }
    for (const char *p = value; p + 1 < value_end; p++)
    {
        const char *word;

        if (*p != '!') continue;
        word = p + 1;
        while (word < value_end && is_blank( *word )) word++;
        if (value_end - word >= 9 && !_strnicmp( word, "important", 9 ))
        {
            important = TRUE;
            value_end = p;
            trim( &value, &value_end );
        }
        break;
    }
    /* an icon of mask-image paints its background through the mask: with no
     * masks yet, the colour would be a solid block where an icon belongs */
    if (masked && (word_is( name, name_end - name, "background" ) ||
                   word_is( name, name_end - name, "background-color" )))
        return;
    has_var = strstr( value, "var(" ) && strstr( value, "var(" ) < value_end;
    shorthand = word_is( name, name_end - name, "background" ) || word_is( name, name_end - name, "font" ) ||
                word_is( name, name_end - name, "list-style" ) || word_is( name, name_end - name, "overflow" ) ||
                is_side_shorthand( name, name_end - name ) || is_foreign( name, name_end - name );
    if (inline_only && !has_var && !shorthand) return;

    if (has_var)
    {
        if (!substitute( ctx, &resolved, value, value_end, 0 ) || resolved.failed)
        {
            free( resolved.data );
            return;     /* invalid at computed-value time: as if not written */
        }
        value = resolved.data ? resolved.data : "";
        value_end = value + resolved.len;
    }
    if (is_foreign( name, name_end - name )) puts_( out, "--arctic-" );
    put( out, name, name_end - name );
    puts_( out, ":" );
    put( out, value, value_end - value );
    if (important) puts_( out, " !important" );
    if (word_is( name, name_end - name, "background" )) expand_background( out, value, value_end, important );
    else if (word_is( name, name_end - name, "font" )) expand_font( out, value, value_end, important );
    else if (word_is( name, name_end - name, "list-style" )) expand_list_style( out, value, value_end, important );
    else if (word_is( name, name_end - name, "overflow" )) expand_overflow( out, value, value_end, important );
    else if (is_side_shorthand( name, name_end - name ))
        expand_sides_shorthand( out, name, name_end - name, value, value_end, important );
    puts_( out, ";" );
    free( resolved.data );
}

static void rewrite_declarations( const struct css_context *ctx, struct text *out, const char *s, const char *end,
                                  BOOL inline_only )
{
    BOOL masked = contains( s, end - s, "mask-image" ) || contains( s, end - s, "-webkit-mask" ) ||
                  contains( s, end - s, "mask:" );

    while (s < end)
    {
        const char *item_end = scan_to( s, end, ";{" );

        if (item_end < end && *item_end == '{')
        {
            /* a nested rule of CSS nesting: later; its block goes */
            s = matching_brace( item_end, end );
            s = s < end ? s + 1 : end;
            continue;
        }
        rewrite_declaration( ctx, out, s, item_end, inline_only, masked );
        s = item_end < end ? item_end + 1 : end;
    }
}

/* --- rules --- */

/* a rule that sets custom properties for the whole document: one of its
 * selectors is the root - ":root, .skin-theme-day" counts, ".night" not */
static BOOL is_document_selector( const char *s, const char *end )
{
    while (s < end)
    {
        const char *comma = scan_to( s, end, "," ), *sel = s, *sel_end = comma;

        trim( &sel, &sel_end );
        if (word_is( sel, sel_end - sel, ":root" ) || word_is( sel, sel_end - sel, "html" ) ||
            word_is( sel, sel_end - sel, "body" ) || word_is( sel, sel_end - sel, "*" ) ||
            word_is( sel, sel_end - sel, ":host" ))
            return TRUE;
        s = comma < end ? comma + 1 : end;
    }
    return FALSE;
}

static void collect_declarations( struct css_context *ctx, const char *s, const char *end )
{
    while (s < end)
    {
        const char *item_end = scan_to( s, end, ";{" ), *name = s, *colon, *name_end, *value, *value_end;

        if (item_end < end && *item_end == '{') break;
        if ((colon = memchr( s, ':', item_end - s )))
        {
            name_end = colon;
            trim( &name, &name_end );
            value = colon + 1;
            value_end = item_end;
            trim( &value, &value_end );
            if (name_end - name > 2 && name[0] == '-' && name[1] == '-')
            {
                /* !important on a custom property only orders it: the value is what is before */
                const char *bang = scan_to( value, value_end, "!" );

                value_end = bang;
                trim( &value, &value_end );
                TRACE( "%.*s: %.*s\n", (int)(name_end - name), name, (int)(value_end - value), value );
                set_var( ctx, name, name_end - name, value, value_end - value );
            }
        }
        s = item_end < end ? item_end + 1 : end;
    }
}

/* a list of rules: at the top of a sheet, or inside @media that holds */
static void walk_rules( struct css_context *ctx, struct text *out, const char *s, const char *end, BOOL collect )
{
    while (s < end)
    {
        const char *prelude, *open, *close;

        while (s < end && (is_blank( *s ) || (*s == '/' && s + 1 < end && s[1] == '*')))
            s = skip_one( s, end );
        if (s >= end) break;
        prelude = s;
        open = scan_to( s, end, "{;" );
        if (open >= end) break;
        if (*open == ';')   /* @import, @charset, @namespace: statements of their own */
        {
            s = open + 1;
            continue;
        }
        close = matching_brace( open, end );

        if (*prelude == '@')
        {
            const char *name = prelude + 1, *name_end = name;

            while (name_end < open && !is_blank( *name_end ) && *name_end != '(' && *name_end != '{') name_end++;
            if (word_is( name, name_end - name, "media" ))
            {
                if (media_matches( ctx, name_end, open )) walk_rules( ctx, out, open + 1, close, collect );
            }
            else if (word_is( name, name_end - name, "supports" ) || word_is( name, name_end - name, "layer" ) ||
                     word_is( name, name_end - name, "document" ) || word_is( name, name_end - name, "-moz-document" ))
                walk_rules( ctx, out, open + 1, close, collect );
            /* @font-face, @keyframes, @page, @container...: later */
        }
        else if (collect)
        {
            if (is_document_selector( prelude, open )) collect_declarations( ctx, open + 1, close );
        }
        else
        {
            put( out, prelude, open - prelude );
            puts_( out, "{" );
            rewrite_declarations( ctx, out, open + 1, close, FALSE );
            puts_( out, "}\n" );
        }
        s = close < end ? close + 1 : end;
    }
}

/* the media="" of a <link> or a <style> */
BOOL css_media_matches( const struct css_context *ctx, const char *media, size_t len )
{
    return media_matches( ctx, media, media + len );
}

void css_collect( struct css_context *ctx, const char *css, size_t len )
{
    walk_rules( ctx, NULL, css, css + len, TRUE );
}

char *css_rewrite( const struct css_context *ctx, const char *css, size_t len, size_t *out_len )
{
    struct text out = { 0 };

    walk_rules( (struct css_context *)ctx, &out, css, css + len, FALSE );
    if (out.failed)
    {
        free( out.data );
        return NULL;
    }
    *out_len = out.len;
    return out.data ? out.data : _strdup( "" );
}

/* what lexbor lacks of an inline style="": var() replaced, the longhands of
 * the shorthands, and only those declarations */
char *css_expand_inline( const struct css_context *ctx, const char *style, size_t len, size_t *out_len )
{
    struct text out = { 0 };

    rewrite_declarations( ctx, &out, style, style + len, TRUE );
    if (out.failed || !out.len)
    {
        free( out.data );
        return NULL;
    }
    *out_len = out.len;
    return out.data;
}
