/*
 * Arctic web engine: the scripts of a page (docs/browser.md, stage 4)
 *
 * QuickJS-ng runs them. What the page sees of the browser - window,
 * document, the elements, location, timers, XMLHttpRequest - is written in
 * JavaScript (dom.js) over the few things only the engine can do, which are
 * here: the nodes of lexbor's tree as objects, one per node, reading and
 * changing the tree, the cookies, the network, navigation.
 *
 * A page's scripts run while it loads, on the loader's thread: in document
 * order, then DOMContentLoaded and load, then the timers and promises they
 * set, in virtual time - a timeout of two seconds fires at once - until
 * there are none or the budget is spent. A navigation they ask for loads in
 * place of the page.
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
#include "../net/net.h"

#include <quickjs.h>
#include <lexbor/html/serialize.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/selectors/selectors.h>

#include "dom_js.h"

#define SCRIPT_BUDGET_MS    4000    /* all the scripts of a page, with their timers */
#define MAX_TICKS           1500    /* timers fired while loading */

struct wrapper
{
    lxb_dom_node_t *node;
    JSValue         object;
};

struct message
{
    char           *json;
    char           *origin;         /* of the page that sent it */
    lxb_dom_node_t *source;         /* the <iframe> it came from, or NULL from the parent */
    int             port_id;        /* to a port of this window rather than to the window */
    int             ports[4];       /* ports it brings: their ids on this side */
    unsigned int    port_count;
    char           *target_origin;  /* in the outbox: who may read it */
    lxb_dom_node_t *to_frame;       /* in the outbox: the <iframe> it goes to, or NULL to the parent */
    char           *sibling;        /* in the outbox: the name of a frame of the parent it goes to */
};

struct script_env
{
    JSRuntime         *rt;
    JSContext         *ctx;
    struct document   *doc;
    JSClassID          node_class;
    JSValue            proto_for;   /* dom.js: the prototype for a node */
    JSValue            run_timers;  /* dom.js: fires the next timer, says when the one after is due */
    JSValue            fire_load;   /* dom.js: DOMContentLoaded and load */
    JSValue            add_import_map, resolve_import;  /* dom.js: <script type=importmap> */
    struct wrapper    *wrappers;    /* node -> its object, open addressing */
    unsigned int       wrapper_count, wrapper_capacity;
    lxb_dom_node_t    *current_script;
    lxb_selectors_t   *selectors;
    lxb_css_parser_t  *css_parser;
    ULONGLONG          deadline;
    unsigned int       executed, ticks;
    unsigned int       mutations;   /* changes of the tree: the view lays the page out again */
    HWND               window;      /* the view's, once the page shows: its timers go there */
    struct message    *messages;    /* postMessage to this page, waiting for its next turn */
    unsigned int       message_count, message_capacity;
    struct message    *outbox;      /* what it sent while it loaded, sent once it shows */
    unsigned int       outbox_count, outbox_capacity;
    JSValue            deliver;     /* dom.js: a message as an event of window */
    JSValue            go_live, next_timer, dispatch;   /* dom.js: real time, the next timer, events */
};

static struct script_env *env_of( JSContext *ctx )
{
    return JS_GetContextOpaque( ctx );
}

/* --- nodes as objects, the same object for the same node --- */

static unsigned int pointer_hash( const void *p )
{
    UINT_PTR v = (UINT_PTR)p;

    v ^= v >> 17;
    v *= 0x9e3779b97f4a7c15ull;
    return (unsigned int)(v >> 32);
}

static struct wrapper *find_slot( struct script_env *env, lxb_dom_node_t *node )
{
    unsigned int mask = env->wrapper_capacity - 1, i;

    for (i = pointer_hash( node ) & mask; env->wrappers[i].node; i = (i + 1) & mask)
        if (env->wrappers[i].node == node) return &env->wrappers[i];
    return &env->wrappers[i];
}

static BOOL grow_wrappers( struct script_env *env )
{
    struct wrapper *old = env->wrappers;
    unsigned int old_capacity = env->wrapper_capacity;

    env->wrapper_capacity = old_capacity ? old_capacity * 2 : 1024;
    if (!(env->wrappers = calloc( env->wrapper_capacity, sizeof(*env->wrappers) )))
    {
        env->wrappers = old;
        env->wrapper_capacity = old_capacity;
        return FALSE;
    }
    for (unsigned int i = 0; i < old_capacity; i++)
        if (old[i].node) *find_slot( env, old[i].node ) = old[i];
    free( old );
    return TRUE;
}

static JSValue wrap( JSContext *ctx, lxb_dom_node_t *node )
{
    struct script_env *env = env_of( ctx );
    struct wrapper *slot;
    JSValue proto, object, type;

    if (!node) return JS_NULL;
    if ((env->wrapper_count + 1) * 2 > env->wrapper_capacity && !grow_wrappers( env )) return JS_NULL;
    slot = find_slot( env, node );
    if (slot->node) return JS_DupValue( ctx, slot->object );

    type = JS_NewInt32( ctx, node->type );
    {
        const lxb_char_t *name = NULL;
        size_t len = 0;
        JSValue args[2];

        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) name = lxb_dom_element_local_name( lxb_dom_interface_element( node ), &len );
        args[0] = type;
        args[1] = name ? JS_NewStringLen( ctx, (const char *)name, len ) : JS_NULL;
        proto = JS_IsFunction( ctx, env->proto_for ) ? JS_Call( ctx, env->proto_for, JS_UNDEFINED, 2, args ) : JS_NULL;
        JS_FreeValue( ctx, args[1] );
    }
    if (JS_IsException( proto )) proto = JS_NULL;
    object = JS_NewObjectProtoClass( ctx, proto, env->node_class );
    JS_FreeValue( ctx, proto );
    if (JS_IsException( object )) return object;
    JS_SetOpaque( object, node );
    slot->node = node;
    slot->object = JS_DupValue( ctx, object );
    env->wrapper_count++;
    return object;
}

static lxb_dom_node_t *unwrap( JSContext *ctx, JSValueConst value )
{
    return JS_GetOpaque( value, env_of( ctx )->node_class );
}

static lxb_dom_element_t *unwrap_element( JSContext *ctx, JSValueConst value )
{
    lxb_dom_node_t *node = unwrap( ctx, value );

    return node && node->type == LXB_DOM_NODE_TYPE_ELEMENT ? lxb_dom_interface_element( node ) : NULL;
}

static lxb_dom_document_t *dom_document( JSContext *ctx )
{
    return lxb_dom_interface_document( env_of( ctx )->doc->html );
}

static JSValue string_value( JSContext *ctx, const void *data, size_t len )
{
    return data ? JS_NewStringLen( ctx, data, len ) : JS_NULL;
}

/* --- the natives dom.js stands on --- */

#define NATIVE(name) static JSValue name( JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv )

NATIVE(n_type)
{
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );
    return node ? JS_NewInt32( ctx, node->type ) : JS_NULL;
}

NATIVE(n_tag)
{
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    const lxb_char_t *name;
    size_t len;

    if (!element) return JS_NULL;
    name = lxb_dom_element_local_name( element, &len );
    return string_value( ctx, name, len );
}

NATIVE(n_parent)   { lxb_dom_node_t *n = unwrap( ctx, argv[0] ); return n ? wrap( ctx, n->parent ) : JS_NULL; }
NATIVE(n_first)    { lxb_dom_node_t *n = unwrap( ctx, argv[0] ); return n ? wrap( ctx, n->first_child ) : JS_NULL; }
NATIVE(n_last)     { lxb_dom_node_t *n = unwrap( ctx, argv[0] ); return n ? wrap( ctx, n->last_child ) : JS_NULL; }
NATIVE(n_next)     { lxb_dom_node_t *n = unwrap( ctx, argv[0] ); return n ? wrap( ctx, n->next ) : JS_NULL; }
NATIVE(n_prev)     { lxb_dom_node_t *n = unwrap( ctx, argv[0] ); return n ? wrap( ctx, n->prev ) : JS_NULL; }

NATIVE(n_attr)
{
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    const lxb_char_t *value;
    const char *name;
    size_t name_len, len;
    JSValue result;

    if (!element || !(name = JS_ToCStringLen( ctx, &name_len, argv[1] ))) return JS_NULL;
    value = lxb_dom_element_get_attribute( element, (const lxb_char_t *)name, name_len, &len );
    result = value ? JS_NewStringLen( ctx, (const char *)value, len ) :
             lxb_dom_element_has_attribute( element, (const lxb_char_t *)name, name_len ) ? JS_NewString( ctx, "" ) : JS_NULL;
    JS_FreeCString( ctx, name );
    return result;
}

NATIVE(n_set_attr)
{
    env_of( ctx )->mutations++;
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    const char *name, *value;
    size_t name_len, len;

    if (!element || !(name = JS_ToCStringLen( ctx, &name_len, argv[1] ))) return JS_UNDEFINED;
    if ((value = JS_ToCStringLen( ctx, &len, argv[2] )))
    {
        lxb_dom_element_set_attribute( element, (const lxb_char_t *)name, name_len, (const lxb_char_t *)value, len );
        JS_FreeCString( ctx, value );
    }
    JS_FreeCString( ctx, name );
    return JS_UNDEFINED;
}

NATIVE(n_remove_attr)
{
    env_of( ctx )->mutations++;
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    const char *name;
    size_t name_len;

    if (!element || !(name = JS_ToCStringLen( ctx, &name_len, argv[1] ))) return JS_UNDEFINED;
    lxb_dom_element_remove_attribute( element, (const lxb_char_t *)name, name_len );
    JS_FreeCString( ctx, name );
    return JS_UNDEFINED;
}

NATIVE(n_attrs)
{
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    JSValue list = JS_NewArray( ctx );
    uint32_t i = 0;

    if (!element) return list;
    for (lxb_dom_attr_t *attr = lxb_dom_element_first_attribute( element ); attr; attr = lxb_dom_element_next_attribute( attr ))
    {
        size_t name_len, len;
        const lxb_char_t *name = lxb_dom_attr_local_name( attr, &name_len ), *value = lxb_dom_attr_value( attr, &len );
        JSValue pair = JS_NewArray( ctx );

        JS_SetPropertyUint32( ctx, pair, 0, string_value( ctx, name, name_len ) );
        JS_SetPropertyUint32( ctx, pair, 1, value ? JS_NewStringLen( ctx, (const char *)value, len ) : JS_NewString( ctx, "" ) );
        JS_SetPropertyUint32( ctx, list, i++, pair );
    }
    return list;
}

NATIVE(n_text)
{
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );
    lxb_char_t *text;
    size_t len;
    JSValue result;

    if (!node) return JS_NULL;
    if (!(text = lxb_dom_node_text_content( node, &len ))) return JS_NewString( ctx, "" );
    result = JS_NewStringLen( ctx, (const char *)text, len );
    lxb_dom_document_destroy_text( node->owner_document, text );
    return result;
}

NATIVE(n_set_text)
{
    env_of( ctx )->mutations++;
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );
    const char *text;
    size_t len;

    if (!node || !(text = JS_ToCStringLen( ctx, &len, argv[1] ))) return JS_UNDEFINED;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && !len)
    {
        while (node->first_child) lxb_dom_node_remove( node->first_child );
    }
    else lxb_dom_node_text_content_set( node, (const lxb_char_t *)text, len );
    JS_FreeCString( ctx, text );
    return JS_UNDEFINED;
}

NATIVE(n_append)
{
    env_of( ctx )->mutations++;
    lxb_dom_node_t *parent = unwrap( ctx, argv[0] ), *child = unwrap( ctx, argv[1] );

    if (!parent || !child) return JS_ThrowTypeError( ctx, "not a node" );
    if (child->parent) lxb_dom_node_remove( child );
    lxb_dom_node_insert_child( parent, child );
    return JS_UNDEFINED;
}

NATIVE(n_insert_before)
{
    env_of( ctx )->mutations++;
    lxb_dom_node_t *parent = unwrap( ctx, argv[0] ), *child = unwrap( ctx, argv[1] ), *ref = unwrap( ctx, argv[2] );

    if (!parent || !child) return JS_ThrowTypeError( ctx, "not a node" );
    if (child == ref) return JS_UNDEFINED;
    if (child->parent) lxb_dom_node_remove( child );
    if (ref && ref->parent == parent) lxb_dom_node_insert_before( ref, child );
    else lxb_dom_node_insert_child( parent, child );
    return JS_UNDEFINED;
}

NATIVE(n_remove)
{
    env_of( ctx )->mutations++;
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );

    if (node && node->parent) lxb_dom_node_remove( node );
    return JS_UNDEFINED;
}

NATIVE(n_create)
{
    const char *tag;
    size_t len;
    lxb_dom_element_t *element;

    if (!(tag = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    element = lxb_dom_document_create_element( dom_document( ctx ), (const lxb_char_t *)tag, len, NULL );
    JS_FreeCString( ctx, tag );
    return element ? wrap( ctx, lxb_dom_interface_node( element ) ) : JS_NULL;
}

NATIVE(n_create_text)
{
    const char *text;
    size_t len;
    lxb_dom_text_t *node;

    if (!(text = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    node = lxb_dom_document_create_text_node( dom_document( ctx ), (const lxb_char_t *)text, len );
    JS_FreeCString( ctx, text );
    return node ? wrap( ctx, lxb_dom_interface_node( node ) ) : JS_NULL;
}

NATIVE(n_create_comment)
{
    const char *text;
    size_t len;
    lxb_dom_comment_t *node;

    if (!(text = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    node = lxb_dom_document_create_comment( dom_document( ctx ), (const lxb_char_t *)text, len );
    JS_FreeCString( ctx, text );
    return node ? wrap( ctx, lxb_dom_interface_node( node ) ) : JS_NULL;
}

NATIVE(n_create_fragment)
{
    lxb_dom_document_fragment_t *fragment = lxb_dom_document_create_document_fragment( dom_document( ctx ) );

    return fragment ? wrap( ctx, lxb_dom_interface_node( fragment ) ) : JS_NULL;
}

NATIVE(n_clone)
{
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );

    return node ? wrap( ctx, lxb_dom_node_clone( node, JS_ToBool( ctx, argv[1] ) ) ) : JS_NULL;
}

struct serialized
{
    char   *data;
    size_t  len, capacity;
};

static lxb_status_t collect( const lxb_char_t *data, size_t len, void *ctx )
{
    struct serialized *out = ctx;

    if (out->len + len + 1 > out->capacity)
    {
        size_t capacity = max( out->capacity * 2, out->len + len + 1024 );
        char *grown = realloc( out->data, capacity );

        if (!grown) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy( out->data + out->len, data, len );
    out->len += len;
    return LXB_STATUS_OK;
}

static JSValue serialize( JSContext *ctx, JSValueConst value, BOOL outer )
{
    lxb_dom_node_t *node = unwrap( ctx, value );
    struct serialized out = { 0 };
    JSValue result;

    if (!node) return JS_NULL;
    if (outer) lxb_html_serialize_tree_cb( node, collect, &out );
    else lxb_html_serialize_deep_cb( node, collect, &out );
    result = JS_NewStringLen( ctx, out.data ? out.data : "", out.len );
    free( out.data );
    return result;
}

NATIVE(n_inner) { return serialize( ctx, argv[0], FALSE ); }
NATIVE(n_outer) { return serialize( ctx, argv[0], TRUE ); }

NATIVE(n_set_inner)
{
    env_of( ctx )->mutations++;
    lxb_dom_element_t *element = unwrap_element( ctx, argv[0] );
    const char *html;
    size_t len;

    if (!element || !(html = JS_ToCStringLen( ctx, &len, argv[1] ))) return JS_UNDEFINED;
    lxb_html_element_inner_html_set( lxb_html_interface_element( element ), (const lxb_char_t *)html, len );
    JS_FreeCString( ctx, html );
    return JS_UNDEFINED;
}

struct found
{
    JSContext *ctx;
    JSValue    list;
    uint32_t   count;
    BOOL       first_only;
    lxb_dom_node_t *root;
};

static lxb_status_t found_node( lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx )
{
    struct found *found = ctx;

    if (node == found->root) return LXB_STATUS_OK;
    JS_SetPropertyUint32( found->ctx, found->list, found->count++, wrap( found->ctx, node ) );
    return found->first_only ? LXB_STATUS_STOP : LXB_STATUS_OK;
}

static lxb_css_selector_list_t *parse_selector( JSContext *ctx, JSValueConst value )
{
    struct script_env *env = env_of( ctx );
    lxb_css_selector_list_t *list;
    lxb_css_parser_t *parser;
    lxb_status_t status;
    const char *text;
    size_t len;

    if (!env->selectors)
    {
        env->selectors = lxb_selectors_create();
        if (lxb_selectors_init( env->selectors ) != LXB_STATUS_OK) return NULL;
    }
    /* a parser for each: the list lives in the parser's memory, and that
     * memory goes with the list - a parser kept would point into it */
    if (!(text = JS_ToCStringLen( ctx, &len, value ))) return NULL;
    if (!(parser = lxb_css_parser_create()) || lxb_css_parser_init( parser, NULL ) != LXB_STATUS_OK)
    {
        if (parser) lxb_css_parser_destroy( parser, true );
        JS_FreeCString( ctx, text );
        return NULL;
    }
    list = lxb_css_selectors_parse( parser, (const lxb_char_t *)text, len );
    status = parser->status;
    lxb_css_parser_destroy( parser, true );
    JS_FreeCString( ctx, text );
    if (status != LXB_STATUS_OK && list)
    {
        lxb_css_selector_list_destroy_memory( list );
        list = NULL;
    }
    return list;
}
NATIVE(n_select)
{
    lxb_dom_node_t *root = unwrap( ctx, argv[0] );
    struct found found = { ctx, JS_NewArray( ctx ), 0, JS_ToBool( ctx, argv[2] ), root };
    lxb_css_selector_list_t *list;

    if (!root) return found.list;
    if (!(list = parse_selector( ctx, argv[1] )))
    {
        JS_FreeValue( ctx, found.list );
        return JS_ThrowSyntaxError( ctx, "not a valid selector" );
    }
    lxb_selectors_opt_set_noi( env_of( ctx )->selectors,
                               LXB_SELECTORS_OPT_MATCH_FIRST * found.first_only );
    lxb_selectors_find( env_of( ctx )->selectors, root, list, found_node, &found );
    lxb_css_selector_list_destroy_memory( list );
    return found.list;
}

static lxb_status_t matched( lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx )
{
    *(BOOL *)ctx = TRUE;
    return LXB_STATUS_STOP;
}

NATIVE(n_matches)
{
    lxb_dom_node_t *node = unwrap( ctx, argv[0] );
    lxb_css_selector_list_t *list;
    BOOL result = FALSE;

    if (!node) return JS_FALSE;
    if (!(list = parse_selector( ctx, argv[1] ))) return JS_ThrowSyntaxError( ctx, "not a valid selector" );
    lxb_selectors_match_node( env_of( ctx )->selectors, node, list, matched, &result );
    lxb_css_selector_list_destroy_memory( list );
    return JS_NewBool( ctx, result );
}

NATIVE(n_document)
{
    return wrap( ctx, lxb_dom_interface_node( env_of( ctx )->doc->html ) );
}

NATIVE(n_url)
{
    const char *address = env_of( ctx )->doc->address;
    return JS_NewString( ctx, address ? address : "about:blank" );
}

NATIVE(n_resolve)
{
    const char *base = NULL, *ref;
    size_t len;
    char *url;
    JSValue result;

    if (argc > 1 && !JS_IsUndefined( argv[1] ) && !JS_IsNull( argv[1] )) base = JS_ToCString( ctx, argv[1] );
    if (!(ref = JS_ToCStringLen( ctx, &len, argv[0] )))
    {
        JS_FreeCString( ctx, base );
        return JS_EXCEPTION;
    }
    url = base ? url_resolve( base, ref, len ) : document_resolve( env_of( ctx )->doc, ref, len );
    result = url ? JS_NewString( ctx, url ) : JS_NULL;
    free( url );
    JS_FreeCString( ctx, ref );
    if (base) JS_FreeCString( ctx, base );
    return result;
}

NATIVE(n_cookie)
{
    struct url_parts parts;
    const char *address = env_of( ctx )->doc->address;
    char *cookies;
    JSValue result;

    if (!address || !url_parse( address, &parts ) || !(cookies = cookies_get( &parts ))) return JS_NewString( ctx, "" );
    result = JS_NewString( ctx, cookies );
    free( cookies );
    return result;
}

NATIVE(n_set_cookie)
{
    struct url_parts parts;
    const char *address = env_of( ctx )->doc->address, *value;
    size_t len;

    if (!address || !url_parse( address, &parts ) || !(value = JS_ToCStringLen( ctx, &len, argv[0] )))
        return JS_UNDEFINED;
    cookies_set( &parts, value, len );
    JS_FreeCString( ctx, value );
    return JS_UNDEFINED;
}

NATIVE(n_navigate)
{
    struct document *doc = env_of( ctx )->doc;
    const char *url;
    size_t len;

    if (!(url = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    free( doc->redirect );
    doc->redirect = document_resolve( doc, url, len );
    TRACE( "the page goes to %s\n", doc->redirect );
    JS_FreeCString( ctx, url );
    return JS_UNDEFINED;
}

/* form.submit(): sent as a click on its button would, by the view, with no submit event */
NATIVE(n_submit_form)
{
    struct document *doc = env_of( ctx )->doc;
    lxb_dom_node_t *form = unwrap( ctx, argv[0] );

    if (form && form->type == LXB_DOM_NODE_TYPE_ELEMENT)
    {
        doc->submitting = lxb_dom_interface_element( form );
        TRACE( "a script sends a form of %s\n", doc->address );
    }
    return JS_UNDEFINED;
}

/* the bytes of a body a script sends: a string as UTF-8, an ArrayBuffer or
 * a typed array as they are; JS_FreeCString is for the string only */
static const char *body_bytes( JSContext *ctx, JSValueConst value, size_t *len, BOOL *is_string )
{
    uint8_t *bytes;

    *is_string = FALSE;
    *len = 0;
    if (JS_IsUndefined( value ) || JS_IsNull( value )) return NULL;
    if ((bytes = JS_GetArrayBuffer( ctx, len, value ))) return (const char *)bytes;
    JS_FreeValue( ctx, JS_GetException( ctx ) );
    if ((bytes = JS_GetUint8Array( ctx, len, value ))) return (const char *)bytes;
    JS_FreeValue( ctx, JS_GetException( ctx ) );
    {
        size_t offset, byte_length, element;
        JSValue buffer = JS_GetTypedArrayBuffer( ctx, value, &offset, &byte_length, &element );

        if (!JS_IsException( buffer ))
        {
            size_t size;
            uint8_t *data = JS_GetArrayBuffer( ctx, &size, buffer );

            JS_FreeValue( ctx, buffer );
            if (data && offset + byte_length <= size)
            {
                *len = byte_length;
                return (const char *)data + offset;
            }
        }
        JS_FreeValue( ctx, JS_GetException( ctx ) );
    }
    *is_string = TRUE;
    return JS_ToCStringLen( ctx, len, value );
}

/* XMLHttpRequest and fetch, done there and then: (method, url, body, headers, binary) */
NATIVE(n_fetch)
{
    struct document *doc = env_of( ctx )->doc;
    struct response response;
    const char *method, *url, *body, *headers = NULL;
    size_t body_len = 0;
    BOOL body_string;
    char *absolute;
    JSValue result;
    BOOL ok;

    if (!(method = JS_ToCString( ctx, argv[0] ))) return JS_EXCEPTION;
    if (!(url = JS_ToCString( ctx, argv[1] )))
    {
        JS_FreeCString( ctx, method );
        return JS_EXCEPTION;
    }
    body = argc > 2 ? body_bytes( ctx, argv[2], &body_len, &body_string ) : (body_string = FALSE, NULL);
    if (argc > 3 && JS_IsString( argv[3] )) headers = JS_ToCString( ctx, argv[3] );
    absolute = document_resolve( doc, url, strlen( url ) );
    if (absolute && !_stricmp( method, "GET" ) && !headers)
        ok = net_fetch( absolute, doc->address, REQUEST_OTHER, &response );
    else ok = net_request( method, absolute, doc->address, headers && *headers ? headers : NULL, body, body_len, &response );
    TRACE( "%s %s: %d, %Iu bytes\n", method, absolute, response.status, response.length );

    result = JS_NewObject( ctx );
    JS_SetPropertyStr( ctx, result, "status", JS_NewInt32( ctx, ok ? response.status : 0 ) );
    JS_SetPropertyStr( ctx, result, "url", JS_NewString( ctx, response.url ? response.url : absolute ? absolute : url ) );
    JS_SetPropertyStr( ctx, result, "type", JS_NewString( ctx, response.content_type ? response.content_type : "" ) );
    JS_SetPropertyStr( ctx, result, "text", JS_NewStringLen( ctx, response.body ? response.body : "", response.length ) );
    if (argc > 4 && JS_ToBool( ctx, argv[4] ))
        JS_SetPropertyStr( ctx, result, "bytes", JS_NewArrayBufferCopy( ctx, (const uint8_t *)(response.body ? response.body : ""),
                                                                        response.length ) );
    JS_SetPropertyStr( ctx, result, "blocked", JS_NewBool( ctx, response.blocked ) );
    if (response.blocked) doc->blocked++;
    response_free( &response );
    free( absolute );
    JS_FreeCString( ctx, method );
    JS_FreeCString( ctx, url );
    if (body && body_string) JS_FreeCString( ctx, body );
    if (headers) JS_FreeCString( ctx, headers );
    return result;
}
NATIVE(n_log)
{
    const char *text = JS_ToCString( ctx, argv[0] );

    if (text)
    {
        TRACE( "console: %s\n", text );
        JS_FreeCString( ctx, text );
    }
    return JS_UNDEFINED;
}

NATIVE(n_now)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter( &counter );
    QueryPerformanceFrequency( &frequency );
    return JS_NewFloat64( ctx, (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart );
}

static void report_exception( JSContext *ctx, const char *where )
{
    JSValue exception = JS_GetException( ctx ), stack;
    const char *text = JS_ToCString( ctx, exception ), *trace = NULL;

    stack = JS_GetPropertyStr( ctx, exception, "stack" );
    if (JS_IsString( stack )) trace = JS_ToCString( ctx, stack );
    WARN( "%s: %s %s\n", where, text ? text : "(?)", trace ? trace : "" );
    if (text) JS_FreeCString( ctx, text );
    if (trace) JS_FreeCString( ctx, trace );
    JS_FreeValue( ctx, stack );
    JS_FreeValue( ctx, exception );
}

/* the words of the string literals of a script: a class it gives an element later is one */
static BOOL is_word_char( char c )
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void keep_words( struct document *doc, const char *code, size_t len )
{
    for (size_t i = 0; i < len; i++)
    {
        char quote = code[i];
        size_t start;

        if (quote != '"' && quote != '\'' && quote != '`') continue;
        for (start = ++i; i < len && code[i] != quote && code[i] != '\n'; i++)
        {
            if (code[i] == '\\') { i++; continue; }
        }
        /* each run of word characters in it */
        for (size_t k = start; k < i; )
        {
            size_t word = k;

            while (k < i && is_word_char( code[k] )) k++;
            if (k - word >= 2 && k - word <= 64)
            {
                if (doc->script_words_len + (k - word) + 1 > doc->script_words_size)
                {
                    size_t size = max( doc->script_words_size * 2, doc->script_words_len + 65536 );
                    char *grown = realloc( doc->script_words, size );

                    if (!grown) return;
                    doc->script_words = grown;
                    doc->script_words_size = size;
                }
                memcpy( doc->script_words + doc->script_words_len, code + word, k - word );
                doc->script_words_len += k - word;
                doc->script_words[doc->script_words_len++] = 0;
            }
            while (k < i && !is_word_char( code[k] )) k++;
        }
    }
}

static void evaluate( JSContext *ctx, const char *code, size_t len, const char *name )
{
    keep_words( env_of( ctx )->doc, code, len );
    ULONGLONG start = GetTickCount64();
    JSValue result;

    env_of( ctx )->executed++;
    result = JS_Eval( ctx, code, len, name, JS_EVAL_TYPE_GLOBAL );
    if (JS_IsException( result )) report_exception( ctx, name );
    JS_FreeValue( ctx, result );
    if (GetTickCount64() - start > 100) TRACE( "%s took %I64u ms\n", name, GetTickCount64() - start );
}

/* --- modules: import and export, fetched as the page names them --- */

static char *normalize_module( JSContext *ctx, const char *base, const char *name, void *opaque )
{
    struct script_env *env = opaque;
    char *url, *result;
    BOOL bare = !(name[0] == '/' || !strncmp( name, "./", 2 ) || !strncmp( name, "../", 3 ) || strstr( name, "://" ));

    if (bare && JS_IsFunction( ctx, env->resolve_import ))
    {
        /* a bare name: the page's import map says what it is */
        JSValue arg = JS_NewString( ctx, name ), mapped = JS_Call( ctx, env->resolve_import, JS_UNDEFINED, 1, &arg );
        const char *target = JS_IsString( mapped ) ? JS_ToCString( ctx, mapped ) : NULL;

        JS_FreeValue( ctx, arg );
        JS_FreeValue( ctx, mapped );
        if (target)
        {
            result = js_strdup( ctx, target );
            JS_FreeCString( ctx, target );
            return result;
        }
    }
    url = url_resolve( base && strstr( base, "://" ) ? base : env->doc->address, name, strlen( name ) );
    if (!url) return NULL;
    result = js_strdup( ctx, url );
    free( url );
    return result;
}

static JSModuleDef *load_module( JSContext *ctx, const char *name, void *opaque )
{
    struct script_env *env = opaque;
    struct response response;
    JSModuleDef *module = NULL;
    JSValue compiled;
    BOOL fetched = net_fetch( name, env->doc->address, REQUEST_SCRIPT, &response );

    if (!fetched || response.status >= 400 || !response.body)
    {
        JS_ThrowReferenceError( ctx, "could not load the module %s", name );
        response_free( &response );
        return NULL;
    }
    compiled = JS_Eval( ctx, response.body, response.length, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY );
    response_free( &response );
    if (JS_IsException( compiled )) return NULL;
    module = JS_VALUE_GET_PTR( compiled );
    JS_FreeValue( ctx, compiled );
    return module;
}

static void evaluate_module( JSContext *ctx, const char *code, size_t len, const char *name )
{
    keep_words( env_of( ctx )->doc, code, len );
    ULONGLONG start = GetTickCount64();
    JSValue result;

    env_of( ctx )->executed++;
    result = JS_Eval( ctx, code, len, name, JS_EVAL_TYPE_MODULE );
    if (JS_IsException( result )) report_exception( ctx, name );
    JS_FreeValue( ctx, result );
    if (GetTickCount64() - start > 100) TRACE( "module %s took %I64u ms\n", name, GetTickCount64() - start );
}

NATIVE(n_eval_module)
{
    const char *code, *name;
    size_t len;

    if (!(code = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    name = JS_ToCString( ctx, argv[1] );
    evaluate_module( ctx, code, len, name ? name : "<module>" );
    JS_FreeCString( ctx, code );
    if (name) JS_FreeCString( ctx, name );
    return JS_UNDEFINED;
}

/* a script dom.js has: one put in the page by a script, or eval of a string by the page */
NATIVE(n_eval)
{
    const char *code, *name;
    size_t len;

    if (!(code = JS_ToCStringLen( ctx, &len, argv[0] ))) return JS_EXCEPTION;
    name = argc > 1 ? JS_ToCString( ctx, argv[1] ) : NULL;
    evaluate( ctx, code, len, name ? name : "<script>" );
    JS_FreeCString( ctx, code );
    if (name) JS_FreeCString( ctx, name );
    return JS_UNDEFINED;
}

/* scheme://host:port of a document, as postMessage names it */
static char *origin_of( const char *address )
{
    const char *host, *end;
    char *origin;

    if (!address || !(host = strstr( address, "://" ))) return _strdup( "null" );
    end = host + 3 + strcspn( host + 3, "/?#" );
    if (!(origin = malloc( end - address + 1 ))) return NULL;
    memcpy( origin, address, end - address );
    origin[end - address] = 0;
    return origin;
}

/* an origin as browsers compare them: lower case, without the scheme's own port or a path */
static void normalize_origin( const char *in, char *out, size_t size )
{
    const char *host = strstr( in, "://" );
    size_t len;
    char *p;

    lstrcpynA( out, in, (int)size );
    if (!host) return;
    host += 3;
    len = (host - in) + strcspn( host, "/?#" );
    if (len < size) out[len] = 0;
    CharLowerA( out );
    if (!strncmp( out, "https://", 8 ) && (p = strrchr( out + 8, ':' )) && !strcmp( p, ":443" )) *p = 0;
    if (!strncmp( out, "http://", 7 ) && (p = strrchr( out + 7, ':' )) && !strcmp( p, ":80" )) *p = 0;
}

static void queue_message( struct document *target, const char *json, size_t len, const char *origin,
                           lxb_dom_node_t *source, const char *target_origin, const int *ports, unsigned int port_count,
                           int port_id )
{
    struct script_env *env = target ? target->script : NULL;
    struct message *message;
    char *own;

    if (!env) return;
    /* the sender names who may read it: another origin gets nothing */
    if (target_origin && strcmp( target_origin, "*" ) && strcmp( target_origin, "/" ) && (own = origin_of( target->address )))
    {
        char a[300], b[300];

        normalize_origin( own, a, sizeof(a) );
        normalize_origin( target_origin, b, sizeof(b) );
        free( own );
        if (strcmp( a, b )) { TRACE( "not for %s: dropped\n", a ); return; }
    }
    if (env->message_count == env->message_capacity)
    {
        unsigned int capacity = env->message_capacity ? env->message_capacity * 2 : 8;
        struct message *grown = realloc( env->messages, capacity * sizeof(*grown) );

        if (!grown) return;
        env->messages = grown;
        env->message_capacity = capacity;
    }
    message = &env->messages[env->message_count++];
    message->json = malloc( len + 1 );
    if (message->json) { memcpy( message->json, json, len ); message->json[len] = 0; }
    message->origin = _strdup( origin );
    message->source = source;
    message->port_id = port_id;
    message->port_count = min( port_count, ARRAY_SIZE(message->ports) );
    for (unsigned int i = 0; i < message->port_count; i++) message->ports[i] = ports[i];
    if (env->window) SetTimer( env->window, (UINT_PTR)env, 0, NULL );
}

/* --- ports of MessageChannel passed between windows: each end an id, the id its document --- */

struct port_entry
{
    int              id, peer;
    struct document *doc;
};

static struct port_entry ports_table[1024];
static unsigned int port_entries;
static int next_port_id = 1;

static struct port_entry *port_entry( int id )
{
    for (unsigned int i = 0; i < port_entries; i++) if (ports_table[i].id == id) return &ports_table[i];
    return NULL;
}

static int port_add( struct document *doc )
{
    if (port_entries == ARRAY_SIZE(ports_table)) return 0;
    ports_table[port_entries].id = next_port_id++;
    ports_table[port_entries].doc = doc;
    ports_table[port_entries].peer = 0;
    return ports_table[port_entries++].id;
}

static void ports_forget( struct document *doc )
{
    for (unsigned int i = 0; i < port_entries; )
        if (ports_table[i].doc == doc) ports_table[i] = ports_table[--port_entries];
        else i++;
}

NATIVE(n_frame_name)
{
    const char *name = env_of( ctx )->doc->frame_name;
    return JS_NewString( ctx, name ? name : "" );
}

NATIVE(n_is_frame)
{
    return JS_NewBool( ctx, env_of( ctx )->doc->parent != NULL );
}

NATIVE(n_origin)
{
    char *origin = origin_of( env_of( ctx )->doc->address );
    JSValue result = JS_NewString( ctx, origin ? origin : "null" );

    free( origin );
    return result;
}

/* a message of this page - to its parent, to one of its frames, or by a port - sent now;
 * the ports it brings are ids of this side, linked here to new ones of the other */
static void send_message( struct script_env *env, const char *json, size_t len, const char *target_origin,
                          lxb_dom_node_t *to_frame, int from_port, const int *ports, unsigned int port_count,
                          const char *sibling )
{
    struct document *doc = env->doc, *target = NULL;
    lxb_dom_node_t *source = NULL;
    char *origin = origin_of( doc->address );
    int linked[4];
    unsigned int count = 0;

    if (from_port)
    {
        struct port_entry *entry = port_entry( from_port ), *peer = entry ? port_entry( entry->peer ) : NULL;

        if (peer) queue_message( peer->doc, json, len, origin ? origin : "null", NULL, "*", NULL, 0, peer->id );
        free( origin );
        return;
    }
    if (to_frame)
    {
        struct frame *frame = document_frame( doc, lxb_dom_interface_element( to_frame ), FALSE );

        if (frame) target = frame->doc;
    }
    else if (sibling && doc->parent)
    {
        /* a frame of the same page, by its name: parent.frames[name] */
        for (unsigned int i = 0; i < doc->parent->frame_count; i++)
        {
            size_t name_len;
            const char *name = (const char *)lxb_dom_element_get_attribute( doc->parent->frames[i]->element,
                                                                            (const lxb_char_t *)"name", 4, &name_len );

            if (name && name_len == strlen( sibling ) && !memcmp( name, sibling, name_len ) && doc->parent->frames[i]->doc)
                target = doc->parent->frames[i]->doc;
        }
    }
    else if (doc->parent)
    {
        for (unsigned int i = 0; i < doc->parent->frame_count; i++)
            if (doc->parent->frames[i]->doc == doc)
            {
                target = doc->parent;
                source = lxb_dom_interface_node( doc->parent->frames[i]->element );
            }
    }
    if (target)
    {
        for (unsigned int i = 0; i < port_count && count < ARRAY_SIZE(linked); i++)
        {
            struct port_entry *mine = port_entry( ports[i] );
            int theirs = mine ? port_add( target ) : 0;

            if (!theirs) continue;
            mine = port_entry( ports[i] );
            mine->peer = theirs;
            port_entry( theirs )->peer = mine->id;
            linked[count++] = theirs;
        }
        queue_message( target, json, len, origin ? origin : "null", source, target_origin, linked, count, 0 );
    }
    free( origin );
}

/* posted while the page loads on its own thread: kept until it shows */
static void post( JSContext *ctx, JSValueConst json_value, JSValueConst origin_value, lxb_dom_node_t *to_frame,
                  int from_port, JSValueConst ports_value, const char *sibling )
{
    struct script_env *env = env_of( ctx );
    const char *json, *target_origin = NULL;
    int ports[4];
    unsigned int port_count = 0;
    size_t len;

    if (!(json = JS_ToCStringLen( ctx, &len, json_value ))) return;
    if (!JS_IsUndefined( origin_value )) target_origin = JS_ToCString( ctx, origin_value );
    if (JS_IsArray( ports_value ))
    {
        JSValue length = JS_GetPropertyStr( ctx, ports_value, "length" );
        int32_t n = 0;

        JS_ToInt32( ctx, &n, length );
        JS_FreeValue( ctx, length );
        for (int32_t i = 0; i < n && port_count < ARRAY_SIZE(ports); i++)
        {
            JSValue v = JS_GetPropertyUint32( ctx, ports_value, i );
            int32_t id = 0;

            JS_ToInt32( ctx, &id, v );
            JS_FreeValue( ctx, v );
            if (id) ports[port_count++] = id;
        }
    }
    if (env->window) send_message( env, json, len, target_origin, to_frame, from_port, ports, port_count, sibling );
    else if (env->outbox_count < 256)
    {
        if (env->outbox_count == env->outbox_capacity)
        {
            unsigned int capacity = env->outbox_capacity ? env->outbox_capacity * 2 : 8;
            struct message *grown = realloc( env->outbox, capacity * sizeof(*grown) );

            if (grown) { env->outbox = grown; env->outbox_capacity = capacity; }
        }
        if (env->outbox_count < env->outbox_capacity)
        {
            struct message *message = &env->outbox[env->outbox_count++];

            memset( message, 0, sizeof(*message) );
            if ((message->json = malloc( len + 1 ))) { memcpy( message->json, json, len ); message->json[len] = 0; }
            message->target_origin = _strdup( target_origin ? target_origin : "*" );
            message->to_frame = to_frame;
            message->sibling = sibling ? _strdup( sibling ) : NULL;
            message->port_id = from_port;
            message->port_count = port_count;
            memcpy( message->ports, ports, port_count * sizeof(int) );
        }
    }
    JS_FreeCString( ctx, json );
    if (target_origin) JS_FreeCString( ctx, target_origin );
}

/* postMessage to the page this frame is in: (json, origin, [port ids]) */
NATIVE(n_post_parent)
{
    if (env_of( ctx )->doc->parent) post( ctx, argv[0], argv[1], NULL, 0, argc > 2 ? argv[2] : JS_UNDEFINED, NULL );
    return JS_UNDEFINED;
}

/* postMessage to the document of an <iframe> of this page: (iframe, json, origin, [port ids]) */
NATIVE(n_post_frame)
{
    lxb_dom_node_t *frame = unwrap( ctx, argv[0] );

    if (frame) post( ctx, argv[1], argv[2], frame, 0, argc > 3 ? argv[3] : JS_UNDEFINED, NULL );
    return JS_UNDEFINED;
}

/* postMessage to another frame of the page this frame is in: (name, json, origin, [port ids]) */
NATIVE(n_post_sibling)
{
    const char *name;

    if (!env_of( ctx )->doc->parent || !(name = JS_ToCString( ctx, argv[0] ))) return JS_UNDEFINED;
    post( ctx, argv[1], argv[2], NULL, 0, argc > 3 ? argv[3] : JS_UNDEFINED, name );
    JS_FreeCString( ctx, name );
    return JS_UNDEFINED;
}

/* a port of this window others can reach: its id */
NATIVE(n_port_register)
{
    return JS_NewInt32( ctx, port_add( env_of( ctx )->doc ) );
}

/* a message by a port whose other end is in another window: (port id, json) */
NATIVE(n_port_post)
{
    int32_t id = 0;

    JS_ToInt32( ctx, &id, argv[0] );
    if (id) post( ctx, argv[1], JS_UNDEFINED, NULL, id, JS_UNDEFINED, NULL );
    return JS_UNDEFINED;
}NATIVE(n_set_proto_for)
{
    struct script_env *env = env_of( ctx );

    JS_FreeValue( ctx, env->proto_for );
    env->proto_for = JS_DupValue( ctx, argv[0] );
    return JS_UNDEFINED;
}

NATIVE(n_current_script)
{
    return wrap( ctx, env_of( ctx )->current_script );
}

NATIVE(n_set_current_script)
{
    env_of( ctx )->current_script = unwrap( ctx, argv[0] );
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry natives[] =
{
    JS_CFUNC_DEF( "type", 1, n_type ),
    JS_CFUNC_DEF( "tag", 1, n_tag ),
    JS_CFUNC_DEF( "parent", 1, n_parent ),
    JS_CFUNC_DEF( "first", 1, n_first ),
    JS_CFUNC_DEF( "last", 1, n_last ),
    JS_CFUNC_DEF( "next", 1, n_next ),
    JS_CFUNC_DEF( "prev", 1, n_prev ),
    JS_CFUNC_DEF( "attr", 2, n_attr ),
    JS_CFUNC_DEF( "setAttr", 3, n_set_attr ),
    JS_CFUNC_DEF( "removeAttr", 2, n_remove_attr ),
    JS_CFUNC_DEF( "attrs", 1, n_attrs ),
    JS_CFUNC_DEF( "text", 1, n_text ),
    JS_CFUNC_DEF( "setText", 2, n_set_text ),
    JS_CFUNC_DEF( "append", 2, n_append ),
    JS_CFUNC_DEF( "insertBefore", 3, n_insert_before ),
    JS_CFUNC_DEF( "remove", 1, n_remove ),
    JS_CFUNC_DEF( "create", 1, n_create ),
    JS_CFUNC_DEF( "createText", 1, n_create_text ),
    JS_CFUNC_DEF( "createComment", 1, n_create_comment ),
    JS_CFUNC_DEF( "createFragment", 0, n_create_fragment ),
    JS_CFUNC_DEF( "clone", 2, n_clone ),
    JS_CFUNC_DEF( "inner", 1, n_inner ),
    JS_CFUNC_DEF( "outer", 1, n_outer ),
    JS_CFUNC_DEF( "setInner", 2, n_set_inner ),
    JS_CFUNC_DEF( "select", 3, n_select ),
    JS_CFUNC_DEF( "matches", 2, n_matches ),
    JS_CFUNC_DEF( "document", 0, n_document ),
    JS_CFUNC_DEF( "url", 0, n_url ),
    JS_CFUNC_DEF( "resolve", 2, n_resolve ),
    JS_CFUNC_DEF( "cookie", 0, n_cookie ),
    JS_CFUNC_DEF( "setCookie", 1, n_set_cookie ),
    JS_CFUNC_DEF( "navigate", 1, n_navigate ),
    JS_CFUNC_DEF( "submitForm", 1, n_submit_form ),
    JS_CFUNC_DEF( "fetch", 5, n_fetch ),
    JS_CFUNC_DEF( "log", 1, n_log ),
    JS_CFUNC_DEF( "now", 0, n_now ),
    JS_CFUNC_DEF( "eval", 2, n_eval ),
    JS_CFUNC_DEF( "currentScript", 0, n_current_script ),
    JS_CFUNC_DEF( "setProtoFor", 1, n_set_proto_for ),
    JS_CFUNC_DEF( "evalModule", 2, n_eval_module ),
    JS_CFUNC_DEF( "isFrame", 0, n_is_frame ),
    JS_CFUNC_DEF( "frameName", 0, n_frame_name ),
    JS_CFUNC_DEF( "origin", 0, n_origin ),
    JS_CFUNC_DEF( "postParent", 3, n_post_parent ),
    JS_CFUNC_DEF( "postFrame", 4, n_post_frame ),
    JS_CFUNC_DEF( "portRegister", 0, n_port_register ),
    JS_CFUNC_DEF( "postSibling", 4, n_post_sibling ),
    JS_CFUNC_DEF( "portPost", 2, n_port_post ),
    JS_CFUNC_DEF( "setCurrentScript", 1, n_set_current_script ),
};

/* a promise rejected with nobody to catch it: the page's error, in the log */
static void rejected( JSContext *ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void *opaque )
{
    const char *text, *trace = NULL;
    JSValue stack;

    if (is_handled) return;
    text = JS_ToCString( ctx, reason );
    stack = JS_IsObject( reason ) ? JS_GetPropertyStr( ctx, reason, "stack" ) : JS_UNDEFINED;
    if (JS_IsString( stack )) trace = JS_ToCString( ctx, stack );
    WARN( "a promise rejected: %s %s\n", text ? text : "(?)", trace ? trace : "" );
    if (text) JS_FreeCString( ctx, text );
    if (trace) JS_FreeCString( ctx, trace );
    JS_FreeValue( ctx, stack );
}

/* --- running --- */

/* a promise's job; QuickJS writes the context of the job wherever it is told */
static int run_job( struct script_env *env )
{
    JSContext *job_ctx;
    ULONGLONG start = GetTickCount64();
    int ret = JS_ExecutePendingJob( env->rt, &job_ctx );

    if (ret < 0) report_exception( job_ctx, "promise" );
    if (GetTickCount64() - start > 100) TRACE( "a promise job took %I64u ms\n", GetTickCount64() - start );
    return ret;
}

static int interrupt( JSRuntime *rt, void *opaque )
{
    struct script_env *env = opaque;

    return GetTickCount64() > env->deadline;
}

static BOOL is_module( lxb_dom_element_t *element )
{
    size_t len;
    const char *type = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"type", 4, &len );

    return type && len == 6 && !_strnicmp( type, "module", 6 );
}

static BOOL is_import_map( lxb_dom_element_t *element )
{
    size_t len;
    const char *type = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"type", 4, &len );

    return type && len == 9 && !_strnicmp( type, "importmap", 9 );
}

static BOOL is_script_type( lxb_dom_element_t *element )
{
    size_t len;
    const char *type = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"type", 4, &len );
    static const char *const types[] = { "text/javascript", "application/javascript", "text/ecmascript",
                                         "application/x-javascript", "module" };

    if (!type || !len) return TRUE;
    for (unsigned int i = 0; i < ARRAY_SIZE(types); i++)
        if (len == strlen( types[i] ) && !_strnicmp( type, types[i], len )) return TRUE;
    return FALSE;
}

/* the scripts the parser met, in document order, each once */
static void run_scripts( struct script_env *env, lxb_dom_node_t *root )
{
    lxb_dom_node_t *node = root;

    while (node && GetTickCount64() < env->deadline && !env->doc->redirect)
    {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_element_tag_id( lxb_dom_interface_element( node ) ) == LXB_TAG_SCRIPT)
        {
            lxb_dom_element_t *element = lxb_dom_interface_element( node );
            const char *src;
            size_t len;

            lxb_dom_element_set_attribute( element, (const lxb_char_t *)"data-arctic-ran", 15, (const lxb_char_t *)"", 0 );
            env->current_script = node;
            if (is_import_map( element ))
            {
                lxb_char_t *text = lxb_dom_node_text_content( node, &len );

                if (text)
                {
                    JSValue args[2] = { JS_NewStringLen( env->ctx, (const char *)text, len ),
                                        JS_NewString( env->ctx, env->doc->address ? env->doc->address : "" ) };
                    JS_FreeValue( env->ctx, JS_Call( env->ctx, env->add_import_map, JS_UNDEFINED, 2, args ) );
                    JS_FreeValue( env->ctx, args[0] );
                    JS_FreeValue( env->ctx, args[1] );
                    lxb_dom_document_destroy_text( node->owner_document, text );
                }
            }
            else if (!is_script_type( element )) ;
            /* modules run here, so what is only for browsers without them does not */
            else if (!is_module( element ) && lxb_dom_element_has_attribute( element, (const lxb_char_t *)"nomodule", 8 )) ;
            else if ((src = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"src", 3, &len )) && len)
            {
                char *url = document_resolve( env->doc, src, len );
                struct response response;

                if (url && net_fetch( url, env->doc->address, REQUEST_SCRIPT, &response ) && response.status < 400 &&
                    response.body)
                {
                    if (is_module( element )) evaluate_module( env->ctx, response.body, response.length, url );
                    else evaluate( env->ctx, response.body, response.length, url );
                }
                else if (response.blocked) env->doc->blocked++;
                else WARN( "no script %s: %d %s\n", url, response.status, response.error );
                response_free( &response );
                free( url );
            }
            else
            {
                lxb_char_t *text = lxb_dom_node_text_content( node, &len );

                if (text)
                {
                    char name[2200];

                    if (is_module( element ))
                    {
                        /* a module's name is its address: an inline one gets one of its own */
                        snprintf( name, sizeof(name), "%s#module-%u", env->doc->address ? env->doc->address : "", env->executed );
                        evaluate_module( env->ctx, (const char *)text, len, name );
                    }
                    else evaluate( env->ctx, (const char *)text, len, env->doc->address ? env->doc->address : "<inline>" );
                    lxb_dom_document_destroy_text( node->owner_document, text );
                }
            }
            env->current_script = NULL;
            while (run_job( env ) > 0) continue;
        }
        /* next in tree order, through what the scripts changed */
        if (node->first_child) node = node->first_child;
        else
        {
            while (node && node != root && !node->next) node = node->parent;
            node = node && node != root ? node->next : NULL;
        }
    }
}

/* the timers and promises the scripts left, in virtual time */
static void run_events( struct script_env *env )
{
    JSValue result;

    for (unsigned int tick = 0; tick < MAX_TICKS && GetTickCount64() < env->deadline && !env->doc->redirect; tick++)
    {
        env->ticks++;
        double next;

        while (run_job( env ) > 0) continue;
        {
            ULONGLONG start = GetTickCount64();

            result = JS_Call( env->ctx, env->run_timers, JS_UNDEFINED, 0, NULL );
            if (GetTickCount64() - start > 100) TRACE( "a timer took %I64u ms\n", GetTickCount64() - start );
        }
        if (JS_IsException( result ))
        {
            report_exception( env->ctx, "timer" );
            continue;
        }
        if (JS_ToFloat64( env->ctx, &next, result ) || next < 0) break;
        JS_FreeValue( env->ctx, result );
    }
    while (run_job( env ) > 0) continue;
}

static void env_free( struct script_env *env )
{
    for (unsigned int i = 0; i < env->wrapper_capacity; i++)
        if (env->wrappers[i].node) JS_FreeValue( env->ctx, env->wrappers[i].object );
    free( env->wrappers );
    JS_FreeValue( env->ctx, env->proto_for );
    JS_FreeValue( env->ctx, env->run_timers );
    JS_FreeValue( env->ctx, env->fire_load );
    JS_FreeValue( env->ctx, env->add_import_map );
    JS_FreeValue( env->ctx, env->resolve_import );
    JS_FreeValue( env->ctx, env->go_live );
    JS_FreeValue( env->ctx, env->next_timer );
    JS_FreeValue( env->ctx, env->dispatch );
    JS_FreeValue( env->ctx, env->deliver );
    for (unsigned int i = 0; i < env->message_count; i++)
    {
        free( env->messages[i].json );
        free( env->messages[i].origin );
    }
    free( env->messages );
    for (unsigned int i = 0; i < env->outbox_count; i++)
    {
        free( env->outbox[i].json );
        free( env->outbox[i].target_origin );
        free( env->outbox[i].sibling );
    }
    free( env->outbox );
    JS_FreeContext( env->ctx );
    JS_FreeRuntime( env->rt );
    if (env->selectors) lxb_selectors_destroy( env->selectors, true );
    if (env->css_parser) lxb_css_parser_destroy( env->css_parser, true );
    free( env );
}

static void run_document( struct document *doc, int width, int height );

struct run_args
{
    struct document *doc;
    int width, height;
};

static DWORD WINAPI script_thread( void *arg )
{
    struct run_args *args = arg;

    run_document( args->doc, args->width, args->height );
    return 0;
}

/* on a thread of its own with a stack of 16 MB: the scripts of pages recurse
 * deep, and the thread that loads, or htmlrun's, has one of 1 MB */
void script_run_document( struct document *doc, int width, int height )
{
    struct run_args args = { doc, width, height };
    HANDLE thread = CreateThread( NULL, 16 * 1024 * 1024, script_thread, &args, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL );

    if (!thread) return;
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
}

static void run_document( struct document *doc, int width, int height )
{
    struct script_env *env;
    JSClassDef node_def = { "Node" };
    JSValue global, native, prelude, hooks;
    ULONGLONG start = GetTickCount64();

    if (!doc->html || !(env = calloc( 1, sizeof(*env) ))) return;
    env->doc = doc;
    env->deadline = start + SCRIPT_BUDGET_MS;
    if (!(env->rt = JS_NewRuntime()) || !(env->ctx = JS_NewContext( env->rt )))
    {
        if (env->rt) JS_FreeRuntime( env->rt );
        free( env );
        return;
    }
    JS_SetMemoryLimit( env->rt, 512 * 1024 * 1024 );
    JS_SetMaxStackSize( env->rt, 12 * 1024 * 1024 );
    JS_SetInterruptHandler( env->rt, interrupt, env );
    JS_SetModuleLoaderFunc( env->rt, normalize_module, load_module, env );
    JS_SetHostPromiseRejectionTracker( env->rt, rejected, env );
    JS_SetContextOpaque( env->ctx, env );
    JS_NewClassID( env->rt, &env->node_class );
    JS_NewClass( env->rt, env->node_class, &node_def );
    env->proto_for = JS_UNDEFINED;
    env->run_timers = JS_UNDEFINED;
    env->fire_load = JS_UNDEFINED;
    env->add_import_map = env->resolve_import = JS_UNDEFINED;
    env->go_live = env->next_timer = env->dispatch = env->deliver = JS_UNDEFINED;

    global = JS_GetGlobalObject( env->ctx );
    native = JS_NewObject( env->ctx );
    JS_SetPropertyFunctionList( env->ctx, native, natives, ARRAY_SIZE(natives) );
    JS_SetPropertyStr( env->ctx, native, "width", JS_NewInt32( env->ctx, width > 0 ? width : 1024 ) );
    JS_SetPropertyStr( env->ctx, native, "height", JS_NewInt32( env->ctx, height > 0 ? height : 768 ) );
    JS_SetPropertyStr( env->ctx, native, "userAgent", JS_NewString( env->ctx, USER_AGENT ) );

    /* dom.js makes window of the global object and gives back its hooks */
    prelude = JS_Eval( env->ctx, dom_js, sizeof(dom_js) - 1, "dom.js", JS_EVAL_TYPE_GLOBAL );
    if (JS_IsException( prelude ))
    {
        report_exception( env->ctx, "dom.js" );
        JS_FreeValue( env->ctx, native );
        JS_FreeValue( env->ctx, global );
        env_free( env );
        return;
    }
    hooks = JS_Call( env->ctx, prelude, global, 1, &native );
    JS_FreeValue( env->ctx, prelude );
    JS_FreeValue( env->ctx, native );
    if (JS_IsException( hooks ))
    {
        report_exception( env->ctx, "dom.js" );
        JS_FreeValue( env->ctx, global );
        env_free( env );
        return;
    }

    env->run_timers = JS_GetPropertyStr( env->ctx, hooks, "runTimers" );
    env->fire_load = JS_GetPropertyStr( env->ctx, hooks, "fireLoad" );
    env->add_import_map = JS_GetPropertyStr( env->ctx, hooks, "addImportMap" );
    env->resolve_import = JS_GetPropertyStr( env->ctx, hooks, "resolveImport" );
    env->go_live = JS_GetPropertyStr( env->ctx, hooks, "goLive" );
    env->next_timer = JS_GetPropertyStr( env->ctx, hooks, "nextTimer" );
    env->dispatch = JS_GetPropertyStr( env->ctx, hooks, "dispatch" );
    env->deliver = JS_GetPropertyStr( env->ctx, hooks, "deliverMessage" );
    JS_FreeValue( env->ctx, hooks );
    JS_FreeValue( env->ctx, global );

    run_scripts( env, lxb_dom_interface_node( doc->html ) );
    if (!doc->redirect)
    {
        JSValue result = JS_Call( env->ctx, env->fire_load, JS_UNDEFINED, 0, NULL );

        if (JS_IsException( result )) report_exception( env->ctx, "load" );
        JS_FreeValue( env->ctx, result );
        run_events( env );
    }
    TRACE( "%u scripts and %u timers in %I64u ms%s%s\n", env->executed, env->ticks, GetTickCount64() - start,
           doc->redirect ? ", then to " : "", doc->redirect ? doc->redirect : "" );
    /* the page lives on with its scripts: the view drives them once it shows it */
    doc->script = env;
}

/* --- the page alive: timers in real time, the user's events --- */

#define LIVE_BUDGET_MS  2000    /* one timer, one event: a page that hangs is stopped */

/* the view's thread runs the scripts from now on, with a stack of its own */
static void enter( struct script_env *env )
{
    JS_UpdateStackTop( env->rt );
    env->deadline = GetTickCount64() + LIVE_BUDGET_MS;
}

/* after a script ran: its promises, and when the next timer is due */
static void leave( struct script_env *env )
{
    JSValue result;
    double next = -1;

    while (run_job( env ) > 0) continue;
    if (!env->window) return;
    result = JS_Call( env->ctx, env->next_timer, JS_UNDEFINED, 0, NULL );
    if (JS_IsException( result )) report_exception( env->ctx, "timers" );
    else JS_ToFloat64( env->ctx, &next, result );
    JS_FreeValue( env->ctx, result );
    if (env->message_count) next = 0;
    if (next >= 0) SetTimer( env->window, (UINT_PTR)env, (UINT)max( 1, min( next, 0x7fffffff ) ), NULL );
    else KillTimer( env->window, (UINT_PTR)env );
}

void script_attach( struct document *doc, HWND window )
{
    struct script_env *env = doc ? doc->script : NULL;
    JSValue result;

    if (!env) return;
    enter( env );
    env->window = window;
    /* what it posted while it loaded goes now */
    for (unsigned int i = 0; i < env->outbox_count; i++)
    {
        struct message *message = &env->outbox[i];

        if (message->json)
            send_message( env, message->json, strlen( message->json ), message->target_origin, message->to_frame,
                          message->port_id, message->ports, message->port_count, message->sibling );
        free( message->json );
        free( message->target_origin );
        free( message->sibling );
    }
    env->outbox_count = 0;
    result = JS_Call( env->ctx, env->go_live, JS_UNDEFINED, 0, NULL );
    if (JS_IsException( result )) report_exception( env->ctx, "live" );
    JS_FreeValue( env->ctx, result );
    leave( env );
}

void script_on_timer( struct document *doc )
{
    struct script_env *env = doc ? doc->script : NULL;
    JSValue result;

    if (!env) return;
    enter( env );
    /* the messages first, each an event of window */
    while (env->message_count)
    {
        struct message message = env->messages[0];
        JSValue args[5];

        memmove( env->messages, env->messages + 1, --env->message_count * sizeof(*env->messages) );
        args[0] = JS_NewString( env->ctx, message.json ? message.json : "null" );
        args[1] = JS_NewString( env->ctx, message.origin ? message.origin : "null" );
        args[2] = message.source ? wrap( env->ctx, message.source ) : JS_NULL;
        args[3] = JS_NewArray( env->ctx );
        for (unsigned int i = 0; i < message.port_count; i++)
            JS_SetPropertyUint32( env->ctx, args[3], i, JS_NewInt32( env->ctx, message.ports[i] ) );
        args[4] = JS_NewInt32( env->ctx, message.port_id );
        result = JS_Call( env->ctx, env->deliver, JS_UNDEFINED, 5, args );
        if (JS_IsException( result )) report_exception( env->ctx, "message" );
        JS_FreeValue( env->ctx, result );
        for (int i = 0; i < 5; i++) JS_FreeValue( env->ctx, args[i] );
        free( message.json );
        free( message.origin );
        while (run_job( env ) > 0) continue;
    }
    result = JS_Call( env->ctx, env->run_timers, JS_UNDEFINED, 0, NULL );
    if (JS_IsException( result )) report_exception( env->ctx, "timer" );
    JS_FreeValue( env->ctx, result );
    leave( env );
}

/* an event of the user on an element; TRUE when the page cancelled what it does */
BOOL script_event( struct document *doc, lxb_dom_element_t *target, const char *type, int x, int y )
{
    struct script_env *env = doc ? doc->script : NULL;
    JSValue args[4], result;
    BOOL prevented = FALSE;

    if (!env || !target) return FALSE;
    enter( env );
    args[0] = wrap( env->ctx, lxb_dom_interface_node( target ) );
    args[1] = JS_NewString( env->ctx, type );
    args[2] = JS_NewInt32( env->ctx, x );
    args[3] = JS_NewInt32( env->ctx, y );
    result = JS_Call( env->ctx, env->dispatch, JS_UNDEFINED, 4, args );
    if (JS_IsException( result )) report_exception( env->ctx, type );
    else prevented = JS_ToBool( env->ctx, result );
    JS_FreeValue( env->ctx, result );
    for (int i = 0; i < 4; i++) JS_FreeValue( env->ctx, args[i] );
    leave( env );
    return prevented;
}

/* each page and frame its own timer in the view: the address of its scripts */
UINT_PTR script_timer_id( struct document *doc )
{
    return doc && doc->script ? (UINT_PTR)doc->script : 0;
}

unsigned int script_mutations( struct document *doc )
{
    return doc && doc->script ? doc->script->mutations : 0;
}

void script_free( struct script_env *env )
{
    if (!env) return;
    ports_forget( env->doc );
    if (env->window) KillTimer( env->window, (UINT_PTR)env );
    JS_UpdateStackTop( env->rt );
    env_free( env );
}
