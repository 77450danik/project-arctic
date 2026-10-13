/*
 * Arctic web engine: forms
 *
 * What a page asks for with <form>: the values of its controls - kept in
 * their own attributes, value="" and checked, which is what the page sends
 * when it has no scripts to read them otherwise - and the submission, as
 * HTML has it: the controls in tree order, the clicked button among them,
 * application/x-www-form-urlencoded in UTF-8, GET into the query or POST
 * as the body.
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

static const char *attribute( lxb_dom_element_t *element, const char *name, size_t *len )
{
    return (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)name, strlen( name ), len );
}

static BOOL has_attribute( lxb_dom_element_t *element, const char *name )
{
    return lxb_dom_element_has_attribute( element, (const lxb_char_t *)name, strlen( name ) );
}

static BOOL attribute_is( lxb_dom_element_t *element, const char *name, const char *value )
{
    size_t len;
    const char *text = attribute( element, name, &len );

    return text && len == strlen( value ) && !_strnicmp( text, value, len );
}

/* the type of an <input>, lower case; "text" for none or one HTML does not know */
static const char *input_type( lxb_dom_element_t *element )
{
    static const char *const types[] =
    {
        "hidden", "text", "search", "tel", "url", "email", "password", "number", "date", "month", "week",
        "time", "datetime-local", "color", "range", "checkbox", "radio", "file", "submit", "image", "reset",
        "button",
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(types); i++)
        if (attribute_is( element, "type", types[i] )) return types[i];
    return "text";
}

enum control_kind control_kind_of( lxb_dom_element_t *element )
{
    const char *type;

    switch (lxb_dom_element_tag_id( element ))
    {
    case LXB_TAG_TEXTAREA: return CONTROL_TEXT;
    case LXB_TAG_SELECT:   return CONTROL_SELECT;
    case LXB_TAG_BUTTON:
        return attribute_is( element, "type", "button" ) || attribute_is( element, "type", "reset" ) ?
               CONTROL_BUTTON : CONTROL_SUBMIT;
    default:
        type = input_type( element );
        if (!strcmp( type, "checkbox" )) return CONTROL_CHECKBOX;
        if (!strcmp( type, "radio" )) return CONTROL_RADIO;
        if (!strcmp( type, "submit" ) || !strcmp( type, "image" )) return CONTROL_SUBMIT;
        if (!strcmp( type, "button" ) || !strcmp( type, "reset" ) || !strcmp( type, "file" ) ||
            !strcmp( type, "color" ) || !strcmp( type, "range" ))
            return CONTROL_BUTTON;
        return CONTROL_TEXT;
    }
}

BOOL control_is_password( lxb_dom_element_t *element )
{
    return lxb_dom_element_tag_id( element ) == LXB_TAG_INPUT && !strcmp( input_type( element ), "password" );
}

static void append_text( lxb_dom_node_t *node, WCHAR **text, size_t *len, size_t *capacity )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT)
        {
            const lexbor_str_t *data = &lxb_dom_interface_character_data( node )->data;
            int count;
            WCHAR *wide = utf8_to_wide( (const char *)data->data, data->length, &count );

            if (!wide) continue;
            if (*len + count + 1 > *capacity)
            {
                size_t grown_capacity = max( *capacity * 2, *len + count + 64 );
                WCHAR *grown = realloc( *text, grown_capacity * sizeof(WCHAR) );

                if (!grown) { free( wide ); return; }
                *text = grown;
                *capacity = grown_capacity;
            }
            memcpy( *text + *len, wide, count * sizeof(WCHAR) );
            *len += count;
            (*text)[*len] = 0;
            free( wide );
        }
        else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) append_text( node, text, len, capacity );
    }
}

static WCHAR *raw_text( lxb_dom_element_t *element )
{
    WCHAR *text = NULL;
    size_t len = 0, capacity = 0;

    append_text( lxb_dom_interface_node( element ), &text, &len, &capacity );
    return text ? text : _wcsdup( L"" );
}

WCHAR *element_text( lxb_dom_element_t *element )
{
    WCHAR *text = raw_text( element ), *in, *out;
    BOOL space = TRUE;

    if (!text) return NULL;
    for (in = out = text; *in; in++)
    {
        if (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r' || *in == '\f')
        {
            if (!space) *out++ = ' ';
            space = TRUE;
        }
        else
        {
            *out++ = *in;
            space = FALSE;
        }
    }
    if (out > text && out[-1] == ' ') out--;
    *out = 0;
    return text;
}

/* the elements under a node, in tree order */
static lxb_dom_node_t *next_in_tree( lxb_dom_node_t *node, lxb_dom_node_t *root )
{
    if (lxb_dom_node_first_child( node )) return lxb_dom_node_first_child( node );
    while (node && node != root)
    {
        if (lxb_dom_node_next( node )) return lxb_dom_node_next( node );
        node = lxb_dom_node_parent( node );
    }
    return NULL;
}

lxb_dom_element_t *select_chosen( lxb_dom_element_t *select )
{
    lxb_dom_node_t *root = lxb_dom_interface_node( select ), *node;
    lxb_dom_element_t *first = NULL;

    for (node = next_in_tree( root, root ); node; node = next_in_tree( node, root ))
    {
        lxb_dom_element_t *element;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        if (lxb_dom_element_tag_id( element ) != LXB_TAG_OPTION) continue;
        if (has_attribute( element, "selected" )) return element;
        if (!first && !has_attribute( element, "disabled" )) first = element;
    }
    return first;
}

WCHAR *control_value( lxb_dom_element_t *element )
{
    const char *value;
    size_t len;

    switch (lxb_dom_element_tag_id( element ))
    {
    case LXB_TAG_SELECT:
    {
        lxb_dom_element_t *option = select_chosen( element );
        return option ? element_text( option ) : _wcsdup( L"" );
    }
    case LXB_TAG_TEXTAREA:
        /* what the user typed is kept in value="", the text inside is where it began */
        if (!has_attribute( element, "value" ))
        {
            WCHAR *text = raw_text( element );

            if (text && text[0] == '\n') memmove( text, text + 1, wcslen( text ) * sizeof(WCHAR) );
            return text;
        }
        /* fall through */
    default:
        if (!(value = attribute( element, "value", &len ))) return _wcsdup( L"" );
        return utf8_to_wide( value, len, NULL );
    }
}

void control_set_value( lxb_dom_element_t *element, const WCHAR *value )
{
    int len = WideCharToMultiByte( CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL );
    char *utf8 = len > 0 ? malloc( len ) : NULL;

    if (!utf8) return;
    WideCharToMultiByte( CP_UTF8, 0, value, -1, utf8, len, NULL, NULL );
    lxb_dom_element_set_attribute( element, (const lxb_char_t *)"value", 5, (const lxb_char_t *)utf8, len - 1 );
    free( utf8 );
}

lxb_dom_element_t *control_form( lxb_dom_element_t *element )
{
    for (lxb_dom_node_t *node = lxb_dom_interface_node( element ); node; node = lxb_dom_node_parent( node ))
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_element_tag_id( lxb_dom_interface_element( node ) ) == LXB_TAG_FORM)
            return lxb_dom_interface_element( node );
    return NULL;
}

lxb_dom_element_t *form_default_button( lxb_dom_element_t *form )
{
    lxb_dom_node_t *root = lxb_dom_interface_node( form ), *node;

    for (node = next_in_tree( root, root ); node; node = next_in_tree( node, root ))
    {
        lxb_dom_element_t *element;
        lxb_tag_id_t tag;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        tag = lxb_dom_element_tag_id( element );
        if ((tag == LXB_TAG_INPUT || tag == LXB_TAG_BUTTON) && control_kind_of( element ) == CONTROL_SUBMIT)
            return element;
    }
    return NULL;
}

/* --- the data set --- */

struct encoded
{
    char   *data;
    size_t  len, capacity;
};

static void put( struct encoded *out, const char *s, size_t len )
{
    if (out->len + len + 1 > out->capacity)
    {
        size_t capacity = max( out->capacity * 2, out->len + len + 256 );
        char *grown = realloc( out->data, capacity );

        if (!grown) return;
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy( out->data + out->len, s, len );
    out->len += len;
    out->data[out->len] = 0;
}

/* application/x-www-form-urlencoded: UTF-8, space as +, the rest of what is not plain as %XX */
static void put_encoded( struct encoded *out, const char *s, size_t len )
{
    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = s[i];
        char escaped[3];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' ||
            c == '.' || c == '_')
            put( out, (const char *)&c, 1 );
        else if (c == ' ') put( out, "+", 1 );
        else
        {
            escaped[0] = '%';
            escaped[1] = hex[c >> 4];
            escaped[2] = hex[c & 15];
            put( out, escaped, 3 );
        }
    }
}

static void put_pair( struct encoded *out, const char *name, size_t name_len, const WCHAR *value )
{
    int len = WideCharToMultiByte( CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL );
    char *utf8 = len > 0 ? malloc( len ) : NULL;

    if (out->len) put( out, "&", 1 );
    put_encoded( out, name, name_len );
    put( out, "=", 1 );
    if (!utf8) return;
    WideCharToMultiByte( CP_UTF8, 0, value, -1, utf8, len, NULL, NULL );
    /* a newline goes as CR LF, as HTML normalises it */
    for (char *p = utf8; *p; p++)
    {
        if (*p == '\n' && (p == utf8 || p[-1] != '\r')) put_encoded( out, "\r\n", 2 );
        else if (*p == '\r' && p[1] != '\n') put_encoded( out, "\r\n", 2 );
        else put_encoded( out, p, 1 );
    }
    free( utf8 );
}

static void put_control( struct encoded *out, lxb_dom_element_t *element, lxb_dom_element_t *submitter )
{
    lxb_tag_id_t tag = lxb_dom_element_tag_id( element );
    const char *name;
    size_t name_len;
    WCHAR *value;

    if (tag != LXB_TAG_INPUT && tag != LXB_TAG_SELECT && tag != LXB_TAG_TEXTAREA && tag != LXB_TAG_BUTTON) return;
    if (has_attribute( element, "disabled" )) return;
    if (!(name = attribute( element, "name", &name_len )) || !name_len)
    {
        if (element == submitter && tag == LXB_TAG_INPUT && !strcmp( input_type( element ), "image" ))
            put( out, out->len ? "&x=0&y=0" : "x=0&y=0", out->len ? 8 : 7 );
        return;
    }

    if (tag == LXB_TAG_INPUT)
    {
        const char *type = input_type( element );

        if (!strcmp( type, "checkbox" ) || !strcmp( type, "radio" ))
        {
            if (!has_attribute( element, "checked" )) return;
            value = has_attribute( element, "value" ) ? control_value( element ) : _wcsdup( L"on" );
        }
        else if (!strcmp( type, "image" ))
        {
            char coordinate[300];

            if (element != submitter || name_len > 256) return;
            snprintf( coordinate, sizeof(coordinate), "%.*s.x", (int)name_len, name );
            put_pair( out, coordinate, strlen( coordinate ), L"0" );
            snprintf( coordinate, sizeof(coordinate), "%.*s.y", (int)name_len, name );
            put_pair( out, coordinate, strlen( coordinate ), L"0" );
            return;
        }
        else if (!strcmp( type, "submit" ) || !strcmp( type, "button" ) || !strcmp( type, "reset" ))
        {
            if (element != submitter) return;
            value = control_value( element );
        }
        else if (!strcmp( type, "file" )) value = _wcsdup( L"" );
        else value = control_value( element );
    }
    else if (tag == LXB_TAG_BUTTON)
    {
        if (element != submitter) return;
        value = control_value( element );
    }
    else if (tag == LXB_TAG_SELECT)
    {
        lxb_dom_element_t *option = select_chosen( element );
        const char *option_value;
        size_t len;

        if (!option) return;
        if ((option_value = attribute( option, "value", &len ))) value = utf8_to_wide( option_value, len, NULL );
        else value = element_text( option );
    }
    else value = control_value( element );

    if (value) put_pair( out, name, name_len, value );
    free( value );
}

BOOL form_submission( struct document *doc, lxb_dom_element_t *form, lxb_dom_element_t *submitter,
                      char **url, struct post_data *post )
{
    lxb_dom_node_t *root = lxb_dom_interface_node( form ), *node;
    struct encoded data = { 0 };
    const char *action;
    size_t action_len;
    BOOL is_post;
    char *target;

    *url = NULL;
    memset( post, 0, sizeof(*post) );
    if (!(submitter && (action = attribute( submitter, "formaction", &action_len ))) &&
        !(action = attribute( form, "action", &action_len )))
        action_len = 0;
    is_post = submitter && has_attribute( submitter, "formmethod" ) ? attribute_is( submitter, "formmethod", "post" )
                                                                    : attribute_is( form, "method", "post" );

    for (node = next_in_tree( root, root ); node; node = next_in_tree( node, root ))
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) put_control( &data, lxb_dom_interface_element( node ), submitter );

    target = action_len ? document_resolve( doc, action, action_len ) : _strdup( doc->address );
    if (!target)
    {
        free( data.data );
        return FALSE;
    }
    TRACE( "%s %s: %s\n", is_post ? "POST" : "GET", target, data.data ? data.data : "" );

    if (is_post && (!_strnicmp( target, "http:", 5 ) || !_strnicmp( target, "https:", 6 )))
    {
        *url = target;
        post->body = data.data ? data.data : _strdup( "" );
        post->length = data.len;
        post->referrer = _strdup( doc->address );
        return TRUE;
    }
    /* GET: the data is the query, in place of the one the action had */
    {
        size_t base = strcspn( target, "?#" );
        char *full = malloc( base + data.len + 2 );

        if (full)
        {
            memcpy( full, target, base );
            full[base] = '?';
            memcpy( full + base + 1, data.data ? data.data : "", data.len );
            full[base + 1 + data.len] = 0;
        }
        free( target );
        free( data.data );
        *url = full;
        return full != NULL;
    }
}
