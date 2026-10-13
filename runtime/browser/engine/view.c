/*
 * Arctic web engine: the view, and the window it owns
 *
 * The engine owns its window, as the engine of Windows does: the class is
 * named "Internet Explorer_Server", so that what looks for the window of a
 * page finds it where it is used to. The host only says where the window
 * goes and where to navigate; the drawing, the scrolling and the input are
 * the engine's own.
 *
 * A page is fetched and parsed on a thread of its own (document_load), so
 * the window never waits for the network; the old page stays until the new
 * one is there. Each navigation has a number, and a page that comes back for
 * a navigation already replaced by another is thrown away.
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

static const WCHAR view_class[] = L"Internet Explorer_Server";

#define LINE_STEP   40      /* an arrow key */
#define WHEEL_STEP  100     /* a notch of the wheel, as browsers on Windows scroll */
#define WM_LOADED   (WM_APP + 1)
#define WM_FRAME_LOADED (WM_APP + 2)

struct arctic_html_view
{
    HWND                 window;
    HWND                 host;       /* where the title, the status and the links go */
    struct surface       surface;
    struct display_list  list;
    struct present      *present;
    struct document     *doc;
    WCHAR                url[2084];
    WCHAR                title[256];
    BOOL                 relayout;   /* the list is not for this width any more */
    int                  scroll_y;
    int                  wheel_rest; /* a part of a notch from a precise wheel */
    LONG                 navigation; /* the number of the latest one */
    BOOL                 loading;
    lxb_dom_element_t   *hover;      /* the link under the pointer */
    lxb_dom_element_t   *pressed;    /* the link the button went down on */
    lxb_dom_element_t   *pressed_target;     /* the element the button went down on */
    lxb_dom_element_t   *pressed_control;    /* and the form control, if one */
    unsigned int         mutations;  /* of the tree by scripts, when the page was last laid out */
};

struct load_job
{
    HWND  window;
    LONG  navigation;
    int   width, height;    /* for the page's media queries */
    struct post_data post;  /* a form's, or nothing */
    WCHAR url[2084];
};

static void tell_host( struct arctic_html_view *view, UINT msg, const WCHAR *text )
{
    if (view->host) SendMessageW( view->host, msg, 0, (LPARAM)text );
}

static void set_title( struct arctic_html_view *view, const WCHAR *title )
{
    lstrcpynW( view->title, title, ARRAY_SIZE(view->title) );
    tell_host( view, ARCTIC_HTML_WM_TITLE, view->title );
}

static void show_done( struct arctic_html_view *view )
{
    WCHAR status[128];

    if (view->doc && view->doc->blocked)
        swprintf( status, ARRAY_SIZE(status), L"Готово · фільтр заблокував запитів: %u", view->doc->blocked );
    else lstrcpynW( status, L"Готово", ARRAY_SIZE(status) );
    tell_host( view, ARCTIC_HTML_WM_STATUS, status );
}

static int client_height( struct arctic_html_view *view )
{
    RECT rect;

    GetClientRect( view->window, &rect );
    return rect.bottom;
}

static void update_scrollbar( struct arctic_html_view *view )
{
    SCROLLINFO info = { .cbSize = sizeof(info), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
    int page = client_height( view ), limit = max( 0, view->list.content_height - page );

    view->scroll_y = max( 0, min( view->scroll_y, limit ) );
    info.nMax = view->list.content_height > page ? view->list.content_height - 1 : 0;
    info.nPage = page;
    info.nPos = view->scroll_y;
    SetScrollInfo( view->window, SB_VERT, &info, TRUE );
}

static void scroll_to( struct arctic_html_view *view, int y )
{
    int limit = max( 0, view->list.content_height - client_height( view ) );

    y = max( 0, min( y, limit ) );
    if (y == view->scroll_y) return;
    view->scroll_y = y;
    SetScrollPos( view->window, SB_VERT, y, TRUE );
    InvalidateRect( view->window, NULL, FALSE );
}

static DWORD WINAPI loader( void *arg )
{
    struct load_job *job = arg;
    struct document *doc = document_load( job->url, job->post.body ? &job->post : NULL, job->width, job->height );

    /* a view that is gone, or a page nobody waits for any more, is dropped */
    if (!PostMessageW( job->window, WM_LOADED, job->navigation, (LPARAM)doc )) document_free( doc );
    free( job->post.body );
    free( job->post.referrer );
    free( job );
    return 0;
}

static void start_loading( struct arctic_html_view *view, struct post_data *post )
{
    struct load_job *job = calloc( 1, sizeof(*job) );
    WCHAR status[2200];
    HANDLE thread;
    RECT rect;

    if (!job) return;
    GetClientRect( view->window, &rect );
    job->width = rect.right;
    job->height = rect.bottom;
    job->window = view->window;
    job->navigation = InterlockedIncrement( &view->navigation );
    lstrcpynW( job->url, view->url, ARRAY_SIZE(job->url) );
    if (post) job->post = *post;    /* the job owns it now */
    view->loading = TRUE;
    swprintf( status, ARRAY_SIZE(status), L"Завантаження %s…", view->url );
    tell_host( view, ARCTIC_HTML_WM_STATUS, status );
    if ((thread = CreateThread( NULL, 0, loader, job, 0, NULL ))) CloseHandle( thread );
    else
    {
        free( job->post.body );
        free( job->post.referrer );
        free( job );
    }
}

static void after_script( struct arctic_html_view *view );

/* --- the page and its frames --- */

static void attach_tree( struct document *doc, HWND window )
{
    if (!doc) return;
    script_attach( doc, window );
    for (unsigned int i = 0; i < doc->frame_count; i++) attach_tree( doc->frames[i]->doc, window );
}

static unsigned int tree_mutations( struct document *doc )
{
    unsigned int count;

    if (!doc) return 0;
    count = script_mutations( doc ) + doc->frame_count;
    for (unsigned int i = 0; i < doc->frame_count; i++) count += tree_mutations( doc->frames[i]->doc );
    return count;
}

static struct document *doc_with_timer( struct document *doc, UINT_PTR id )
{
    struct document *found;

    if (!doc) return NULL;
    if (script_timer_id( doc ) == id) return doc;
    for (unsigned int i = 0; i < doc->frame_count; i++)
        if ((found = doc_with_timer( doc->frames[i]->doc, id ))) return found;
    return NULL;
}

static BOOL in_tree( struct document *root, struct document *doc )
{
    if (!root) return FALSE;
    if (root == doc) return TRUE;
    for (unsigned int i = 0; i < root->frame_count; i++) if (in_tree( root->frames[i]->doc, doc )) return TRUE;
    return FALSE;
}

struct frame_job
{
    HWND             window;
    LONG             navigation;
    struct document *parent;
    lxb_dom_element_t *element;
    char            *url;
    int              width, height;
    struct document *doc;
};

static DWORD WINAPI frame_loader( void *arg )
{
    struct frame_job *job = arg;

    job->doc = document_load_frame( job->url, job->parent, job->width, job->height );
    if (!PostMessageW( job->window, WM_FRAME_LOADED, job->navigation, (LPARAM)job ))
    {
        document_free( job->doc );
        free( job->url );
        free( job );
    }
    return 0;
}

/* a frame a script made, or sent somewhere else: its document comes on a thread of its own */
static void load_frame( struct arctic_html_view *view, struct document *parent, struct frame *frame, const char *url )
{
    struct frame_job *job = calloc( 1, sizeof(*job) );
    HANDLE thread;

    if (!job) return;
    job->window = view->window;
    job->navigation = view->navigation;
    job->parent = parent;
    job->element = frame->element;
    job->url = _strdup( url );
    job->width = 300;
    job->height = 150;
    free( frame->url );
    frame->url = _strdup( url );
    frame->loading = TRUE;
    if ((thread = CreateThread( NULL, 0, frame_loader, job, 0, NULL ))) CloseHandle( thread );
    else
    {
        frame->loading = FALSE;
        free( job->url );
        free( job );
    }
}

/* the <iframe>s of a document that have no document yet, or another src now */
static void check_frames( struct arctic_html_view *view, struct document *doc, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        struct frame *frame;
        char *url;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        if (lxb_dom_element_tag_id( element ) != LXB_TAG_IFRAME)
        {
            check_frames( view, doc, node );
            continue;
        }
        if (!(frame = document_frame( doc, element, TRUE )) || frame->loading || !(url = frame_url( doc, element ))) continue;
        if (!frame->doc || !frame->url || strcmp( frame->url, url ))
        {
            if (frame->doc) { document_free( frame->doc ); frame->doc = NULL; }
            load_frame( view, doc, frame, url );
        }
        free( url );
    }
    for (unsigned int i = 0; i < doc->frame_count; i++)
    {
        struct document *child = doc->frames[i]->doc;

        if (!child) continue;
        /* a frame whose scripts navigate it: its next document */
        if (child->redirect)
        {
            char *target = child->redirect;

            child->redirect = NULL;
            document_free( child );
            doc->frames[i]->doc = NULL;
            load_frame( view, doc, doc->frames[i], target );
            free( target );
            continue;
        }
        if (child->html) check_frames( view, child, lxb_dom_interface_node( child->html ) );
    }
}

static void install( struct arctic_html_view *view, struct document *doc )
{
    /* the old list points at fonts of the old page: both go together */
    display_list_reset( &view->list );
    font_cache_clear();
    document_free( view->doc );
    view->doc = doc;
    view->scroll_y = 0;
    view->hover = view->pressed = NULL;
    view->relayout = TRUE;
    view->loading = FALSE;
    if (doc)
    {
        lstrcpynW( view->url, doc->url, ARRAY_SIZE(view->url) );
        tell_host( view, ARCTIC_HTML_WM_LOCATION, view->url );
    }
    set_title( view, doc && doc->title[0] ? doc->title : view->url );
    show_done( view );
    InvalidateRect( view->window, NULL, FALSE );
    if (doc)
    {
        attach_tree( doc, view->window );
        view->mutations = tree_mutations( doc );
        after_script( view );
    }
}

static void layout( struct arctic_html_view *view )
{
    RECT rect;

    GetClientRect( view->window, &rect );
    layout_document( view->doc, &view->list, rect.right, rect.bottom );
    view->relayout = FALSE;
    update_scrollbar( view );
}

static void paint( struct arctic_html_view *view )
{
    RECT rect;
    HDC dc;

    if (view->relayout) layout( view );
    GetClientRect( view->window, &rect );
    if (rect.right <= 0 || rect.bottom <= 0) return;

    if (view->surface.width != rect.right || view->surface.height != rect.bottom)
    {
        surface_destroy( &view->surface );
        if (!surface_create( &view->surface, rect.right, rect.bottom )) return;
    }
    raster_paint( &view->surface, &view->list, view->scroll_y );

    if (present_surface( view->present, &view->surface )) return;

    /* the software path, and the one every failure above falls back to */
    if ((dc = GetDC( view->window )))
    {
        BitBlt( dc, 0, 0, view->surface.width, view->surface.height, view->surface.dc, 0, 0, SRCCOPY );
        ReleaseDC( view->window, dc );
    }
}

static void on_scroll( struct arctic_html_view *view, int code )
{
    SCROLLINFO info = { .cbSize = sizeof(info), .fMask = SIF_TRACKPOS };
    int page = client_height( view );

    switch (code)
    {
    case SB_LINEUP:    scroll_to( view, view->scroll_y - LINE_STEP ); break;
    case SB_LINEDOWN:  scroll_to( view, view->scroll_y + LINE_STEP ); break;
    case SB_PAGEUP:    scroll_to( view, view->scroll_y - max( page - LINE_STEP, LINE_STEP ) ); break;
    case SB_PAGEDOWN:  scroll_to( view, view->scroll_y + max( page - LINE_STEP, LINE_STEP ) ); break;
    case SB_TOP:       scroll_to( view, 0 ); break;
    case SB_BOTTOM:    scroll_to( view, view->list.content_height ); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        if (GetScrollInfo( view->window, SB_VERT, &info )) scroll_to( view, info.nTrackPos );
        break;
    }
}

/* --- links --- */

static lxb_dom_element_t *link_at( struct arctic_html_view *view, int x, int y )
{
    POINT point = { x, y + view->scroll_y };

    if (view->relayout) return NULL;
    for (unsigned int i = view->list.link_count; i--; )
        if (PtInRect( &view->list.links[i].rect, point )) return view->list.links[i].element;
    return NULL;
}

/* the address a link goes to, in full, for the status bar and the host */
static WCHAR *link_target( struct arctic_html_view *view, lxb_dom_element_t *link )
{
    const char *href;
    size_t len;
    char *url;
    WCHAR *wide;

    if (!view->doc || !(href = (const char *)lxb_dom_element_get_attribute( link, (const lxb_char_t *)"href", 4, &len )))
        return NULL;
    if (!(url = document_resolve( view->doc, href, len ))) return NULL;
    wide = utf8_to_wide( url, strlen( url ), NULL );
    free( url );
    return wide;
}

static void hover( struct arctic_html_view *view, int x, int y )
{
    lxb_dom_element_t *link = link_at( view, x, y );
    WCHAR *target;

    if (link == view->hover) return;
    view->hover = link;
    if (link && (target = link_target( view, link )))
    {
        tell_host( view, ARCTIC_HTML_WM_STATUS, target );
        free( target );
    }
    else if (!view->loading) show_done( view );
}

static void follow( struct arctic_html_view *view, lxb_dom_element_t *link )
{
    WCHAR *target = link_target( view, link );

    if (!target) return;
    if (!_wcsnicmp( target, L"javascript:", 11 ))
    {
        tell_host( view, ARCTIC_HTML_WM_STATUS, L"Посилання на JavaScript — рушій виконує JS з етапу 4" );
        free( target );
        return;
    }
    /* the host keeps the history; a host that does not, the view goes itself */
    if (!view->host || !SendMessageW( view->host, ARCTIC_HTML_WM_NAVIGATE, 0, (LPARAM)target ))
        view_navigate( view, target );
    free( target );
}

/* --- forms --- */

static const struct control_area *control_at( struct arctic_html_view *view, int x, int y )
{
    POINT point = { x, y + view->scroll_y };

    if (view->relayout) return NULL;
    for (unsigned int i = view->list.control_count; i--; )
        if (PtInRect( &view->list.controls[i].rect, point )) return &view->list.controls[i];
    return NULL;
}

static void changed( struct arctic_html_view *view )
{
    view->relayout = TRUE;
    InvalidateRect( view->window, NULL, FALSE );
}

static void set_focus( struct arctic_html_view *view, lxb_dom_element_t *element, int caret )
{
    if (!view->doc) return;
    if (view->doc->focus == element && view->doc->caret == caret) return;
    view->doc->focus = element;
    view->doc->caret = caret;
    changed( view );
}

static void submit( struct arctic_html_view *view, lxb_dom_element_t *form, lxb_dom_element_t *submitter )
{
    struct post_data post;
    WCHAR *wide;
    char *url;

    if (!view->doc || !form) return;
    /* the page's scripts may take the form over: then it is theirs to send */
    if (view->doc->script)
    {
        struct document *doc = view->doc;
        BOOL prevented = script_event( doc, form, "submit", 0, 0 );

        after_script( view );
        if (prevented || view->doc != doc) return;
    }
    if (!form_submission( view->doc, form, submitter, &url, &post )) return;
    wide = utf8_to_wide( url, strlen( url ), NULL );
    free( url );
    if (!wide)
    {
        free( post.body );
        free( post.referrer );
        return;
    }
    if (!post.body)
    {
        /* a GET is an address like a link's: the host keeps it in its travel log */
        if (!view->host || !SendMessageW( view->host, ARCTIC_HTML_WM_NAVIGATE, 0, (LPARAM)wide ))
            view_navigate( view, wide );
    }
    else
    {
        /* a POST only the view can make; the host writes the address down */
        if (view->host) SendMessageW( view->host, ARCTIC_HTML_WM_NAVIGATE, ARCTIC_HTML_NAVIGATE_RECORD, (LPARAM)wide );
        lstrcpynW( view->url, wide, ARRAY_SIZE(view->url) );
        start_loading( view, &post );
    }
    free( wide );
}

/* a radio button goes on, the others of its name in its form go off */
static void check_radio( lxb_dom_element_t *radio )
{
    lxb_dom_element_t *form = control_form( radio );
    lxb_dom_node_t *root, *node;
    const char *name;
    size_t name_len;

    if ((name = (const char *)lxb_dom_element_get_attribute( radio, (const lxb_char_t *)"name", 4, &name_len )))
    {
        root = form ? lxb_dom_interface_node( form ) : lxb_dom_interface_node( lxb_dom_interface_node( radio )->owner_document );
        for (node = root; node; )
        {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && node != lxb_dom_interface_node( radio ))
            {
                lxb_dom_element_t *element = lxb_dom_interface_element( node );
                const char *other;
                size_t other_len;

                if (lxb_dom_element_tag_id( element ) == LXB_TAG_INPUT && control_kind_of( element ) == CONTROL_RADIO &&
                    (other = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"name", 4, &other_len )) &&
                    other_len == name_len && !memcmp( other, name, name_len ) && control_form( element ) == form)
                    lxb_dom_element_remove_attribute( element, (const lxb_char_t *)"checked", 7 );
            }
            if (lxb_dom_node_first_child( node )) node = lxb_dom_node_first_child( node );
            else
            {
                while (node && node != root && !lxb_dom_node_next( node )) node = lxb_dom_node_parent( node );
                node = node && node != root ? lxb_dom_node_next( node ) : NULL;
            }
        }
    }
    lxb_dom_element_set_attribute( radio, (const lxb_char_t *)"checked", 7, (const lxb_char_t *)"", 0 );
}

/* a <select> opens as a menu of its options, where the pointer is */
static void choose_option( struct arctic_html_view *view, const struct control_area *control )
{
    lxb_dom_element_t *options[256], *chosen = select_chosen( control->element );
    lxb_dom_node_t *root = lxb_dom_interface_node( control->element ), *node;
    HMENU menu = CreatePopupMenu();
    unsigned int count = 0;
    POINT where = { control->rect.left, control->rect.bottom - view->scroll_y };
    int picked;

    if (!menu) return;
    for (node = lxb_dom_node_first_child( root ); node && count < ARRAY_SIZE(options); )
    {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
            lxb_dom_element_t *element = lxb_dom_interface_element( node );

            if (lxb_dom_element_tag_id( element ) == LXB_TAG_OPTION)
            {
                WCHAR *text = element_text( element );
                UINT flags = MF_STRING | (element == chosen ? MF_CHECKED : 0) |
                             (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"disabled", 8 ) ? MF_GRAYED : 0);

                options[count++] = element;
                AppendMenuW( menu, flags, count, text && text[0] ? text : L" " );
                free( text );
            }
            else if (lxb_dom_element_tag_id( element ) == LXB_TAG_OPTGROUP && lxb_dom_node_first_child( node ))
            {
                node = lxb_dom_node_first_child( node );
                continue;
            }
        }
        while (node && node != root && !lxb_dom_node_next( node )) node = lxb_dom_node_parent( node );
        node = node && node != root ? lxb_dom_node_next( node ) : NULL;
    }
    ClientToScreen( view->window, &where );
    picked = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, where.x, where.y, 0,
                             view->window, NULL );
    DestroyMenu( menu );
    if (picked <= 0 || picked > (int)count) return;
    for (unsigned int i = 0; i < count; i++)
        lxb_dom_element_remove_attribute( options[i], (const lxb_char_t *)"selected", 8 );
    lxb_dom_element_set_attribute( options[picked - 1], (const lxb_char_t *)"selected", 8, (const lxb_char_t *)"", 0 );
    changed( view );
}

/* where in the text a click at x falls */
static int caret_from_x( const struct control_area *control, int x )
{
    WCHAR *value = control_value( control->element );
    int len = value ? (int)wcslen( value ) : 0, best = len, best_distance = INT_MAX;

    if (value && control_is_password( control->element )) for (int i = 0; i < len; i++) value[i] = 0x2022;
    for (int i = 0; i <= len && value; i++)
    {
        int distance = abs( control->text_x + font_measure( control->font, value, i ) - x );

        if (distance < best_distance) { best_distance = distance; best = i; }
    }
    free( value );
    return best;
}

static void click_control( struct arctic_html_view *view, const struct control_area *control, int x )
{
    lxb_dom_element_t *element = control->element;

    if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"disabled", 8 )) return;
    switch (control->kind)
    {
    case CONTROL_TEXT:
        if (lxb_dom_element_tag_id( element ) == LXB_TAG_TEXTAREA)
        {
            WCHAR *value = control_value( element );

            set_focus( view, element, value ? (int)wcslen( value ) : 0 );
            free( value );
        }
        else set_focus( view, element, caret_from_x( control, x ) );
        break;
    case CONTROL_CHECKBOX:
        if (lxb_dom_element_has_attribute( element, (const lxb_char_t *)"checked", 7 ))
            lxb_dom_element_remove_attribute( element, (const lxb_char_t *)"checked", 7 );
        else lxb_dom_element_set_attribute( element, (const lxb_char_t *)"checked", 7, (const lxb_char_t *)"", 0 );
        set_focus( view, NULL, 0 );
        changed( view );
        break;
    case CONTROL_RADIO:
        check_radio( element );
        set_focus( view, NULL, 0 );
        changed( view );
        break;
    case CONTROL_SELECT:
        set_focus( view, NULL, 0 );
        choose_option( view, control );
        break;
    case CONTROL_SUBMIT:
        set_focus( view, NULL, 0 );
        submit( view, control_form( element ), element );
        break;
    case CONTROL_BUTTON:
        break;      /* what it does, the page's scripts did on click */
    }
}


static BOOL is_multiline( lxb_dom_element_t *element )
{
    return lxb_dom_element_tag_id( element ) == LXB_TAG_TEXTAREA;
}

static void edit( struct arctic_html_view *view, const WCHAR *insert, int insert_len, int delete_before,
                  int delete_after )
{
    lxb_dom_element_t *element = view->doc->focus;
    WCHAR *value = control_value( element ), *edited;
    int len = value ? (int)wcslen( value ) : 0, caret = min( max( view->doc->caret, 0 ), len ), limit = INT_MAX;
    const char *max_length;
    size_t attr_len;

    if (!value) return;
    if ((max_length = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"maxlength", 9, &attr_len )) &&
        atoi( max_length ) > 0)
        limit = atoi( max_length );
    delete_before = min( delete_before, caret );
    delete_after = min( delete_after, len - caret );
    insert_len = max( 0, min( insert_len, limit - (len - delete_before - delete_after) ) );
    if (!(edited = malloc( (len - delete_before - delete_after + insert_len + 1) * sizeof(WCHAR) )))
    {
        free( value );
        return;
    }
    memcpy( edited, value, (caret - delete_before) * sizeof(WCHAR) );
    memcpy( edited + caret - delete_before, insert, insert_len * sizeof(WCHAR) );
    memcpy( edited + caret - delete_before + insert_len, value + caret + delete_after,
            (len - caret - delete_after) * sizeof(WCHAR) );
    edited[len - delete_before - delete_after + insert_len] = 0;
    control_set_value( element, edited );
    view->doc->caret = caret - delete_before + insert_len;
    free( edited );
    free( value );
    changed( view );
    if (view->doc->script)
    {
        script_event( view->doc, element, "input", 0, 0 );
        after_script( view );
    }
}

static void paste( struct arctic_html_view *view )
{
    HANDLE data;
    const WCHAR *text;

    if (!OpenClipboard( view->window )) return;
    if ((data = GetClipboardData( CF_UNICODETEXT )) && (text = GlobalLock( data )))
    {
        WCHAR *copy = _wcsdup( text );

        GlobalUnlock( data );
        if (copy)
        {
            int len = 0;

            /* one line takes no newlines: they become spaces */
            for (WCHAR *p = copy; *p; p++)
            {
                if (*p == '\r' && !is_multiline( view->doc->focus )) continue;
                if (*p == '\r') continue;
                if (*p == '\n' && !is_multiline( view->doc->focus )) *p = ' ';
                copy[len++] = *p;
            }
            edit( view, copy, len, 0, 0 );
            free( copy );
        }
    }
    CloseClipboard();
}

/* the next or the previous field of the page, as Tab goes */
static void tab( struct arctic_html_view *view, BOOL back )
{
    int count = (int)view->list.control_count, at = -1;

    if (!count) return;
    for (int i = 0; i < count; i++) if (view->list.controls[i].element == view->doc->focus) at = i;
    for (int step = 1; step <= count; step++)
    {
        int i = ((at < 0 && back ? 0 : at) + (back ? -step : step) + count * 2) % count;
        const struct control_area *control = &view->list.controls[i];

        if (control->kind == CONTROL_TEXT)
        {
            WCHAR *value = control_value( control->element );

            set_focus( view, control->element, value ? (int)wcslen( value ) : 0 );
            free( value );
            if (control->rect.top < view->scroll_y || control->rect.bottom > view->scroll_y + client_height( view ))
                scroll_to( view, control->rect.top - client_height( view ) / 3 );
            return;
        }
    }
}

static BOOL field_char( struct arctic_html_view *view, WCHAR c )
{
    lxb_dom_element_t *focus = view->doc ? view->doc->focus : NULL;

    if (!focus) return FALSE;
    switch (c)
    {
    case '\b':
        edit( view, NULL, 0, 1, 0 );
        return TRUE;
    case '\r':
        if (is_multiline( focus )) edit( view, L"\n", 1, 0, 0 );
        else
        {
            lxb_dom_element_t *form = control_form( focus );

            if (form) submit( view, form, form_default_button( form ) );
        }
        return TRUE;
    case '\t':
    case '\n':
        return TRUE;
    case 0x16:  /* Ctrl+V */
        paste( view );
        return TRUE;
    case 0x1b:  /* Esc */
        set_focus( view, NULL, 0 );
        return TRUE;
    default:
        if (c < 0x20 || c == 0x7f) return TRUE;
        edit( view, &c, 1, 0, 0 );
        return TRUE;
    }
}

static BOOL field_key( struct arctic_html_view *view, WPARAM key )
{
    lxb_dom_element_t *focus = view->doc ? view->doc->focus : NULL;
    WCHAR *value;
    int len;

    if (key == VK_TAB)
    {
        tab( view, GetKeyState( VK_SHIFT ) < 0 );
        return TRUE;
    }
    if (!focus) return FALSE;
    value = control_value( focus );
    len = value ? (int)wcslen( value ) : 0;
    free( value );
    switch (key)
    {
    case VK_LEFT:   view->doc->caret = max( 0, min( view->doc->caret, len ) - 1 ); break;
    case VK_RIGHT:  view->doc->caret = min( len, view->doc->caret + 1 ); break;
    case VK_HOME:   view->doc->caret = 0; break;
    case VK_END:    view->doc->caret = len; break;
    case VK_DELETE: edit( view, NULL, 0, 0, 1 ); return TRUE;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        return is_multiline( focus );
    case VK_SPACE:
        return TRUE;    /* a space, which WM_CHAR types; not a page down */
    default:
        return FALSE;
    }
    changed( view );
    return TRUE;
}

/* what a script did that the view shows: a navigation, a changed tree, a title */
static void after_script( struct arctic_html_view *view )
{
    struct document *doc = view->doc;
    const lxb_char_t *title;
    size_t len;

    if (!doc) return;
    if (doc->redirect)
    {
        WCHAR *target = utf8_to_wide( doc->redirect, strlen( doc->redirect ), NULL );

        free( doc->redirect );
        doc->redirect = NULL;
        if (target)
        {
            if (!view->host || !SendMessageW( view->host, ARCTIC_HTML_WM_NAVIGATE, 0, (LPARAM)target ))
                view_navigate( view, target );
            free( target );
        }
        return;
    }
    if (tree_mutations( doc ) != view->mutations)
    {
        if (doc->html) check_frames( view, doc, lxb_dom_interface_node( doc->html ) );
        view->mutations = tree_mutations( doc );
        view->relayout = TRUE;
        InvalidateRect( view->window, NULL, FALSE );
        if (doc->html && (title = lxb_html_document_title( doc->html, &len )) && len)
        {
            WCHAR *wide = utf8_to_wide( (const char *)title, len, NULL );

            if (wide && wcscmp( wide, view->title )) set_title( view, wide );
            free( wide );
        }
    }
}

static lxb_dom_element_t *target_at( struct arctic_html_view *view, int x, int y )
{
    lxb_dom_element_t *target;

    if (!view->doc || view->relayout) return NULL;
    if ((target = display_list_hit( &view->list, x, y + view->scroll_y ))) return target;
    return view->doc->html && view->doc->html->body ? lxb_dom_interface_element( view->doc->html->body ) : NULL;
}

static const struct control_area *control_of( struct arctic_html_view *view, lxb_dom_element_t *element )
{
    if (view->relayout) layout( view );
    for (unsigned int i = 0; i < view->list.control_count; i++)
        if (view->list.controls[i].element == element) return &view->list.controls[i];
    return NULL;
}

/* a mouse event to the page's scripts; TRUE when they cancelled it */
static BOOL page_event( struct arctic_html_view *view, lxb_dom_element_t *target, const char *type, int x, int y )
{
    BOOL prevented;

    struct document *doc = document_owning( view->doc, target );

    TRACE( "%s at %d,%d on <%s> of %s\n", type, x, y,
           target ? (const char *)lxb_dom_element_local_name( target, NULL ) : "-", doc ? doc->address : "no document" );
    if (!doc || !doc->script || !target) return FALSE;
    prevented = script_event( doc, target, type, x, y );
    after_script( view );
    return prevented;
}

static LRESULT WINAPI view_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct arctic_html_view *view = (struct arctic_html_view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (!view)
    {
        if (msg == WM_LOADED) document_free( (struct document *)lp );
        return DefWindowProcW( hwnd, msg, wp, lp );
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;   /* the page covers every pixel: no flicker */

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        BeginPaint( hwnd, &ps );
        paint( view );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_LOADED:
        if ((LONG)wp != view->navigation) document_free( (struct document *)lp );
        else install( view, (struct document *)lp );
        return 0;

    case WM_SIZE:
        /* a new width lays the page out again; a new height only shows more of it */
        if (LOWORD(lp) != view->list.width) view->relayout = TRUE;
        else update_scrollbar( view );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        view->relayout = TRUE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_VSCROLL:
        on_scroll( view, LOWORD(wp) );
        return 0;

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM( wp ) + view->wheel_rest;
        int notches = delta / WHEEL_DELTA;

        view->wheel_rest = delta - notches * WHEEL_DELTA;
        scroll_to( view, view->scroll_y - notches * WHEEL_STEP );
        hover( view, -1, -1 );
        return 0;
    }
    case WM_CHAR:
        if (field_char( view, (WCHAR)wp )) return 0;
        break;

    case WM_KEYDOWN:
        if (field_key( view, wp )) return 0;
        switch (wp)
        {
        case VK_UP:    on_scroll( view, SB_LINEUP ); return 0;
        case VK_DOWN:  on_scroll( view, SB_LINEDOWN ); return 0;
        case VK_PRIOR: on_scroll( view, SB_PAGEUP ); return 0;
        case VK_NEXT:
        case VK_SPACE: on_scroll( view, GetKeyState( VK_SHIFT ) < 0 && wp == VK_SPACE ? SB_PAGEUP : SB_PAGEDOWN );
                       return 0;
        case VK_HOME:  on_scroll( view, SB_TOP ); return 0;
        case VK_END:   on_scroll( view, SB_BOTTOM ); return 0;
        }
        break;

    case WM_MOUSEMOVE:
        hover( view, (short)LOWORD(lp), (short)HIWORD(lp) );
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT point;
            const struct control_area *control;

            GetCursorPos( &point );
            ScreenToClient( hwnd, &point );
            if ((control = control_at( view, point.x, point.y )) && control->kind == CONTROL_TEXT)
            {
                SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_IBEAM ) );
                return TRUE;
            }
            if (view->hover && !control)
            {
                SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_HAND ) );
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        const struct control_area *control = control_at( view, x, y );
        lxb_dom_element_t *target = target_at( view, x, y );

        SetFocus( hwnd );   /* the keys go to the page from now on */
        view->pressed = NULL;
        view->pressed_target = target;
        view->pressed_control = control ? control->element : NULL;
        if (page_event( view, target, "mousedown", x, y )) return 0;
        if (view->doc != NULL && view->pressed_control && (control = control_of( view, view->pressed_control )) &&
            control->kind == CONTROL_TEXT)
        {
            /* a field takes the caret as the button goes down, as in Windows */
            click_control( view, control, x );
            view->pressed_control = NULL;
            return 0;
        }
        if (!view->pressed_control) set_focus( view, NULL, 0 );
        view->pressed = link_at( view, x, y );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        lxb_dom_element_t *link = link_at( view, x, y ), *target = target_at( view, x, y );
        lxb_dom_element_t *pressed_control = view->pressed_control, *pressed_link = view->pressed;
        const struct control_area *control;
        struct document *doc = view->doc;

        view->pressed = view->pressed_control = NULL;
        if (page_event( view, target, "mouseup", x, y ) || view->doc != doc) return 0;
        if (!target || target != view->pressed_target)
        {
            view->pressed_target = NULL;
            return 0;
        }
        view->pressed_target = NULL;
        /* click: the page first, then what the browser does - unless the page said no */
        if (page_event( view, target, "click", x, y ) || view->doc != doc) return 0;
        if (pressed_control && (control = control_of( view, pressed_control )))
            click_control( view, control, x );
        else if (link && link == pressed_link) follow( view, link );
        return 0;
    }
    case WM_TIMER:
    {
        struct document *doc = doc_with_timer( view->doc, wp );

        if (doc)
        {
            script_on_timer( doc );
            after_script( view );
            return 0;
        }
        break;
    }
    case WM_FRAME_LOADED:
    {
        struct frame_job *job = (struct frame_job *)lp;
        struct frame *frame;

        /* the frame is still in the page it was loaded for: its document goes in */
        if ((LONG)wp == view->navigation && in_tree( view->doc, job->parent ) &&
            (frame = document_frame( job->parent, job->element, FALSE )) && frame->loading)
        {
            frame->loading = FALSE;
            frame->doc = job->doc;
            attach_tree( job->doc, hwnd );
            view->mutations = 0;
            after_script( view );
            view->relayout = TRUE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else document_free( job->doc );
        free( job->url );
        free( job );
        return 0;
    }

    case WM_RBUTTONDOWN:
        SetFocus( hwnd );
        return 0;

    case WM_GETDLGCODE:
        /* a field takes Enter and Tab itself; the page alone leaves them to the dialog keys of the host */
        if (view->doc && view->doc->focus) return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

BOOL view_register_class(void)
{
    WNDCLASSW class = { .lpfnWndProc = view_proc, .hInstance = arctichtml_instance,
                        .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                        .lpszClassName = view_class };

    if (!RegisterClassW( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR( "no window class for the engine: %lu\n", GetLastError() );
        return FALSE;
    }
    return TRUE;
}

struct arctic_html_view *view_create( HWND parent, const RECT *rect )
{
    struct arctic_html_view *view;
    RECT where = { 0, 0, 0, 0 };

    if (!view_register_class()) return NULL;
    if (!(view = calloc( 1, sizeof(*view) ))) return NULL;

    if (rect) where = *rect;
    else if (parent) GetClientRect( parent, &where );

    view->host = parent;
    view->window = CreateWindowExW( 0, view_class, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                                    where.left, where.top, where.right - where.left,
                                    where.bottom - where.top, parent, NULL, arctichtml_instance, NULL );
    if (!view->window)
    {
        ERR( "no window for the engine: %lu\n", GetLastError() );
        free( view );
        return NULL;
    }
    SetWindowLongPtrW( view->window, GWLP_USERDATA, (LONG_PTR)view );

    view->present = present_create( view->window );
    TRACE( "view %p, window %p, %s\n", view, view->window, present_on_gpu( view->present ) ? "Vulkan" : "GDI" );

    /* a blank page at once: the window has something to draw before the first navigation */
    install( view, document_load( L"about:blank", NULL, 0, 0 ) );
    return view;
}

void view_destroy( struct arctic_html_view *view )
{
    if (!view) return;
    InterlockedIncrement( &view->navigation );  /* what is still loading is dropped */
    if (view->window)
    {
        SetWindowLongPtrW( view->window, GWLP_USERDATA, 0 );
        DestroyWindow( view->window );
    }
    present_destroy( view->present );
    surface_destroy( &view->surface );
    display_list_free( &view->list );
    document_free( view->doc );
    free( view );
}

void view_set_rect( struct arctic_html_view *view, const RECT *rect )
{
    if (!view || !rect) return;
    SetWindowPos( view->window, 0, rect->left, rect->top, rect->right - rect->left,
                  rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE );
    InvalidateRect( view->window, NULL, FALSE );
}

HWND view_window( struct arctic_html_view *view )
{
    return view ? view->window : NULL;
}

static int hex_digit( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* An address that differs from the page's only after '#' is a place in the
 * same document: no load, the view scrolls to the block with that id - the
 * contents of an article are made of such links. */
static BOOL same_document( struct arctic_html_view *view, const WCHAR *url )
{
    const WCHAR *hash = wcschr( url, '#' ), *own = wcschr( view->url, '#' );
    size_t base = own ? (size_t)(own - view->url) : wcslen( view->url );
    char fragment[1024];
    int len, out = 0;

    if (!view->doc || view->loading || (!hash && !own) || !wcscmp( url, view->url )) return FALSE;
    if ((hash ? (size_t)(hash - url) : wcslen( url )) != base || wcsncmp( url, view->url, base )) return FALSE;

    lstrcpynW( view->url, url, ARRAY_SIZE(view->url) );
    tell_host( view, ARCTIC_HTML_WM_LOCATION, view->url );
    if (!hash || !hash[1] || (len = WideCharToMultiByte( CP_UTF8, 0, hash + 1, -1, fragment, sizeof(fragment),
                                                        NULL, NULL )) <= 1)
    {
        scroll_to( view, 0 );   /* "#" alone, or none: the top */
        return TRUE;
    }
    for (int i = 0; i < len - 1; i++)  /* the id as the page wrote it, not as the address carries it */
    {
        if (fragment[i] == '%' && i + 2 < len - 1 && hex_digit( fragment[i + 1] ) >= 0 &&
            hex_digit( fragment[i + 2] ) >= 0)
        {
            fragment[out++] = (char)(hex_digit( fragment[i + 1] ) * 16 + hex_digit( fragment[i + 2] ));
            i += 2;
        }
        else fragment[out++] = fragment[i];
    }
    if (view->relayout) layout( view );
    TRACE( "fragment %.*s among %u anchors\n", out, fragment, view->list.anchor_count );
    for (unsigned int i = 0; i < view->list.anchor_count; i++)
    {
        size_t id_len;
        const char *id = (const char *)lxb_dom_element_get_attribute( view->list.anchors[i].element,
                                                                      (const lxb_char_t *)"id", 2, &id_len );

        if (id && id_len == (size_t)out && !memcmp( id, fragment, out ))
        {
            scroll_to( view, view->list.anchors[i].y );
            break;
        }
    }
    return TRUE;    /* an id the page does not have: it stays where it is, as browsers do */
}

HRESULT view_navigate( struct arctic_html_view *view, const WCHAR *url )
{
    if (!view || !url) return E_INVALIDARG;
    TRACE( "%p %s\n", view, debugstr_w(url) );
    if (same_document( view, url )) return S_OK;
    lstrcpynW( view->url, url, ARRAY_SIZE(view->url) );
    start_loading( view, NULL );
    return S_OK;
}

HRESULT view_title( struct arctic_html_view *view, WCHAR *buffer, UINT count )
{
    if (!view || !buffer || !count) return E_INVALIDARG;
    lstrcpynW( buffer, view->title, count );
    return S_OK;
}

BOOL view_on_gpu( struct arctic_html_view *view )
{
    return view && present_on_gpu( view->present );
}
