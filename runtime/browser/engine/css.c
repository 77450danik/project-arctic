/*
 * Arctic web engine: the CSS shorthands lexbor has no parser for
 *
 * lexbor parses the longhands - background-color, font-size, font-family -
 * but drops background and font, which pages write all the time, and drops
 * them before any code of ours sees them. So the text of a sheet is
 * rewritten before lexbor parses it: after each such shorthand go the
 * longhands it stands for, in the same rule, with the same !important. As
 * CSS wants, a longhand the shorthand leaves out is reset: background:
 * url(x) makes the colour transparent, font: 14px Arial makes the weight
 * normal.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "arctichtml_private.h"

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

/* the next token of a value: a word, a function with its parentheses, a
 * string, or a run of the family list's commas */
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

static BOOL word_is( const char *s, size_t len, const char *word )
{
    return strlen( word ) == len && !_strnicmp( s, word, len );
}

/* --- background --- */

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
    if (!_strnicmp( s, "url(", 4 ) || !_strnicmp( s, "linear-gradient", 15 ) ||
        !_strnicmp( s, "radial-gradient", 15 ) || !_strnicmp( s, "conic-gradient", 14 ) ||
        !_strnicmp( s, "repeating-", 10 ) || !_strnicmp( s, "image-set", 9 ) || !_strnicmp( s, "var(", 4 ))
        return FALSE;
    for (unsigned int i = 0; i < ARRAY_SIZE(not_colors); i++) if (word_is( s, len, not_colors[i] )) return FALSE;
    return TRUE;    /* a name, rgb(), hsl(), transparent, currentcolor: lexbor tells if it is one */
}

static void expand_background( struct text *out, const char *value, const char *end, BOOL important )
{
    const char *color = NULL, *start, *s = value;
    size_t color_len = 0;

    /* the colour belongs to the last layer: after the last comma that is not inside rgb(...) */
    for (const char *p = value, *depth_check = value; p < end; p++)
    {
        int depth = 0;

        if (*p != ',') continue;
        for (depth_check = value; depth_check < p; depth_check++)
            depth += *depth_check == '(' ? 1 : *depth_check == ')' ? -1 : 0;
        if (!depth) s = p + 1;
    }
    if (end - value == 7 && !_strnicmp( value, "inherit", 7 )) return;
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

/* --- font --- */

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
        /* normal, small-caps, condensed...: nothing of ours yet */
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

/* --- declarations --- */

/* a declaration name: value, without its terminator; what to add after it */
static void expand_declaration( struct text *out, const char *decl, const char *end )
{
    const char *colon = memchr( decl, ':', end - decl ), *name = decl, *name_end, *value, *value_end;
    BOOL important = FALSE;

    if (!colon) return;
    name_end = colon;
    trim( &name, &name_end );
    value = colon + 1;
    value_end = end;
    trim( &value, &value_end );
    for (const char *p = value; p + 1 < value_end; p++)
    {
        if (*p != '!') continue;
        const char *word = p + 1;

        while (word < value_end && is_blank( *word )) word++;
        if (value_end - word >= 9 && !_strnicmp( word, "important", 9 ))
        {
            important = TRUE;
            value_end = p;
            trim( &value, &value_end );
        }
        break;
    }
    if (word_is( name, name_end - name, "background" )) expand_background( out, value, value_end, important );
    else if (word_is( name, name_end - name, "font" )) expand_font( out, value, value_end, important );
}

/* the end of what goes on until ; { or } outside strings, comments and parentheses */
static const char *scan_item( const char *s, const char *end )
{
    int depth = 0;
    char quote = 0;

    while (s < end)
    {
        if (quote)
        {
            if (*s == '\\' && s + 1 < end) s++;
            else if (*s == quote) quote = 0;
        }
        else if (*s == '"' || *s == '\'') quote = *s;
        else if (*s == '/' && s + 1 < end && s[1] == '*')
        {
            const char *close = s + 2;

            while (close + 1 < end && !(close[0] == '*' && close[1] == '/')) close++;
            s = close + 1 < end ? close + 1 : end - 1;
        }
        else if (*s == '(') depth++;
        else if (*s == ')') depth--;
        else if (depth <= 0 && (*s == ';' || *s == '{' || *s == '}')) return s;
        s++;
    }
    return end;
}

char *css_rewrite( const char *css, size_t len, size_t *out_len )
{
    struct text out = { 0 };
    const char *s = css, *end = css + len;
    int depth = 0;

    while (s < end)
    {
        const char *item_end = scan_item( s, end );
        char term = item_end < end ? *item_end : 0;

        put( &out, s, item_end - s );
        /* inside a block, an item ending in ; or } is a declaration; one ending in { a selector */
        if (depth > 0 && term != '{') expand_declaration( &out, s, item_end );
        if (term)
        {
            put( &out, item_end, 1 );
            if (term == '{') depth++;
            else if (term == '}' && depth > 0) depth--;
        }
        s = item_end + (term ? 1 : 0);
    }
    if (out.failed)
    {
        free( out.data );
        return NULL;
    }
    *out_len = out.len;
    return out.data ? out.data : _strdup( "" );
}

/* the longhands for the shorthands of an inline style="", and only them */
char *css_expand_inline( const char *style, size_t len, size_t *out_len )
{
    struct text out = { 0 };
    const char *s = style, *end = style + len;

    while (s < end)
    {
        const char *item_end = scan_item( s, end );

        expand_declaration( &out, s, item_end );
        s = item_end + (item_end < end ? 1 : 0);
    }
    if (out.failed || !out.len)
    {
        free( out.data );
        return NULL;
    }
    *out_len = out.len;
    return out.data;
}
