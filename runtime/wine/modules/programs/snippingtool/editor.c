/*
 * Snipping Tool: the window that marks a snip up
 *
 * Laid out as Windows 11's: New on the left, the pens, the eraser and the
 * crop in the middle, undo and redo after them, copy, save and the folder
 * on the right; the snip fits the window, never larger than it is. Every
 * change goes to the clipboard on its own, as with "Automatically copy
 * changes", which is on in Windows by default.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdio.h>

#include "snippingtool.h"
#include "commctrl.h"
#include "commdlg.h"
#include "shellapi.h"
#include "shellscalingapi.h"
enum tool { TOOL_NONE, TOOL_PEN, TOOL_HIGHLIGHTER, TOOL_ERASER, TOOL_CROP };

enum button { B_NEW, B_PEN, B_HIGHLIGHTER, B_ERASER, B_CROP, B_UNDO, B_REDO, B_COPY, B_SAVE, B_FOLDER, B_COUNT };

static const struct { enum glyph glyph; UINT tip; } buttons[B_COUNT] =
{
    { GLYPH_NEW, IDS_NEW }, { GLYPH_PEN, IDS_PEN }, { GLYPH_HIGHLIGHTER, IDS_HIGHLIGHTER },
    { GLYPH_ERASER, IDS_ERASER }, { GLYPH_CROP, IDS_CROP }, { GLYPH_UNDO, IDS_UNDO }, { GLYPH_REDO, IDS_REDO },
    { GLYPH_COPY, IDS_COPY }, { GLYPH_SAVE, IDS_SAVE }, { GLYPH_FOLDER, IDS_FOLDER },
};

/* the ballpoint pen's colours and widths, as Windows 11 offers them */
static const UINT32 pen_colors[] = { 0xff000000, 0xffe81123, 0xffffb900, 0xff107c10, 0xff0078d4, 0xff881798, 0xffffffff };
static const float pen_widths[] = { 2, 4, 8 };
#define HIGHLIGHTER_COLOR 0xffffe600
#define HIGHLIGHTER_WIDTH 18

#define TIMER_COPY 1
#define ID_COLOR   100
#define ID_WIDTH   200

struct state
{
    struct image   *base;
    struct stroke **strokes;
    int             count;
};

struct history
{
    struct state *items;
    int           count, capacity;
};

struct editor
{
    HWND            hwnd, tooltip;
    UINT            dpi;
    float           scale;
    HFONT           font;
    struct theme    theme;
    WCHAR           path[MAX_PATH];  /* where the snip was kept, or nothing */

    struct image   *base;            /* the snip, cropped as the user cropped it */
    struct stroke **strokes;
    int             count, capacity;
    struct history  undo, redo;
    struct image   *comp;            /* base and strokes */
    struct image   *live;            /* comp and the stroke being drawn */
    struct image   *view;            /* live at the window's zoom */
    float           zoom;
    RECT            image_rect;      /* where view is */

    enum tool       tool;
    int             color, width;
    struct stroke  *drawing;
    BOOL            erasing, erased;
    BOOL            cropping;
    POINT           crop_from;
    RECT            crop;

    RECT            bar, canvas, rects[B_COUNT];
    int             hot, pressed;
    BOOL            tracking;

    struct image    back;
    HDC             back_dc;
    HBITMAP         back_bitmap;
};

static int px( const struct editor *e, float value )
{
    return (int)floorf( value * e->scale + 0.5f );
}

/* undo */

static struct state snapshot( const struct editor *e )
{
    struct state state = { image_addref( e->base ), NULL, 0 };

    if (e->count && (state.strokes = malloc( e->count * sizeof(*state.strokes) )))
    {
        for (int i = 0; i < e->count; i++) state.strokes[i] = stroke_addref( e->strokes[i] );
        state.count = e->count;
    }
    return state;
}

static void state_free( struct state *state )
{
    for (int i = 0; i < state->count; i++) stroke_release( state->strokes[i] );
    free( state->strokes );
    image_release( state->base );
    memset( state, 0, sizeof(*state) );
}

static void history_push( struct history *history, struct state state )
{
    if (history->count == history->capacity)
    {
        int capacity = max( 16, history->capacity * 2 );
        struct state *items = realloc( history->items, capacity * sizeof(*items) );
        if (!items)
        {
            state_free( &state );
            return;
        }
        history->items = items;
        history->capacity = capacity;
    }
    history->items[history->count++] = state;
}

static void history_clear( struct history *history )
{
    while (history->count) state_free( &history->items[--history->count] );
}

static void push_undo( struct editor *e )
{
    history_push( &e->undo, snapshot( e ) );
    history_clear( &e->redo );
}

static void release_strokes( struct editor *e )
{
    for (int i = 0; i < e->count; i++) stroke_release( e->strokes[i] );
    free( e->strokes );
    e->strokes = NULL;
    e->count = e->capacity = 0;
}

static void add_stroke( struct editor *e, struct stroke *stroke )
{
    if (e->count == e->capacity)
    {
        int capacity = max( 16, e->capacity * 2 );
        struct stroke **strokes = realloc( e->strokes, capacity * sizeof(*strokes) );
        if (!strokes)
        {
            stroke_release( stroke );
            return;
        }
        e->strokes = strokes;
        e->capacity = capacity;
    }
    e->strokes[e->count++] = stroke;
}

/* drawing */

static BOOL enabled( const struct editor *e, int button )
{
    switch (button)
    {
    case B_NEW:  return TRUE;
    case B_UNDO: return e->undo.count > 0;
    case B_REDO: return e->redo.count > 0;
    default:     return e->base != NULL;
    }
}

static BOOL checked( const struct editor *e, int button )
{
    switch (button)
    {
    case B_PEN:         return e->tool == TOOL_PEN;
    case B_HIGHLIGHTER: return e->tool == TOOL_HIGHLIGHTER;
    case B_ERASER:      return e->tool == TOOL_ERASER;
    case B_CROP:        return e->tool == TOOL_CROP;
    default:            return FALSE;
    }
}

static void draw_text( struct editor *e, const WCHAR *text, RECT *rect, UINT32 color, UINT flags )
{
    SetTextColor( e->back_dc, to_colorref( color ) );
    SetBkMode( e->back_dc, TRANSPARENT );
    DrawTextW( e->back_dc, text, -1, rect, flags | DT_NOPREFIX );
}

static void draw_bar( struct editor *e, const RECT *clip )
{
    const struct theme *t = &e->theme;
    RECT r, line = { e->bar.left, e->bar.bottom - 1, e->bar.right, e->bar.bottom };

    if (!IntersectRect( &r, &e->bar, clip )) return;
    image_fill( &e->back, &r, t->surface );
    image_fill( &e->back, &line, t->border );

    for (int i = 0; i < B_COUNT; i++)
    {
        RECT button = e->rects[i], icon;
        UINT32 fore = enabled( e, i ) ? t->text : (t->subtext & 0x00ffffff) | 0x80000000;

        if (!IntersectRect( &r, &button, clip )) continue;
        InflateRect( &button, 0, -px( e, 2 ) );
        if (i == B_NEW)  /* the accent button, as Windows 11's */
        {
            ink_round_rect( &e->back, &button, 4 * e->scale, e->hot == i ? (t->accent & 0x00ffffff) | 0xe0000000 :
                            t->accent, clip );
            fore = t->on_accent;
            SetRect( &icon, button.left + px( e, 8 ), button.top, button.left + px( e, 30 ), button.bottom );
        }
        else
        {
            if (checked( e, i ) || (enabled( e, i ) && (e->hot == i || e->pressed == i)))
                ink_round_rect( &e->back, &button, 4 * e->scale,
                                checked( e, i ) || e->pressed == i ? t->pressed : t->hover, clip );
            icon = button;
        }
        ink_glyph( &e->back, buttons[i].glyph, &icon, px( e, 20 ), fore, clip );
        if (i == B_PEN || i == B_HIGHLIGHTER)  /* the colour the pen writes in */
        {
            RECT mark = { (icon.left + icon.right) / 2 - px( e, 7 ), icon.bottom - px( e, 5 ),
                          (icon.left + icon.right) / 2 + px( e, 7 ), icon.bottom - px( e, 3 ) };
            ink_round_rect( &e->back, &mark, e->scale,
                            i == B_PEN ? pen_colors[e->color] : HIGHLIGHTER_COLOR, clip );
        }
    }
    /* the groups apart */
    for (int i = 0; i < 3; i++)
    {
        static const int after[3] = { B_NEW, B_CROP, B_REDO };
        int x = (e->rects[after[i]].right + e->rects[after[i] + 1].left) / 2;
        RECT sep = { x, e->bar.top + px( e, 14 ), x + max( 1, px( e, 1 ) ), e->bar.bottom - px( e, 14 ) };
        if (i == 2 && e->rects[B_COPY].left - e->rects[B_REDO].right < px( e, 16 )) continue;
        image_fill( &e->back, &sep, t->border );
    }
    GdiFlush();
    r = e->rects[B_NEW];
    r.left += px( e, 32 );
    draw_text( e, string( IDS_NEW ), &r, t->on_accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE );
}

static void draw_canvas( struct editor *e, const RECT *clip )
{
    const struct theme *t = &e->theme;
    RECT r, frame;

    if (!IntersectRect( &r, &e->canvas, clip )) return;
    image_fill( &e->back, &r, t->back );
    if (!e->view)
    {
        RECT text = e->canvas;
        GdiFlush();
        draw_text( e, string( IDS_EMPTY ), &text, t->subtext, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
        return;
    }

    frame = e->image_rect;
    InflateRect( &frame, 1, 1 );
    if (IntersectRect( &r, &frame, clip )) image_fill( &e->back, &r, t->border );
    if (IntersectRect( &r, &e->image_rect, clip ))
    {
        for (int y = r.top; y < r.bottom; y++)
        {
            const UINT32 *src = e->view->bits + (SIZE_T)(y - e->image_rect.top) * e->view->width - e->image_rect.left;
            UINT32 *dst = e->back.bits + (SIZE_T)y * e->back.width;
            for (int x = r.left; x < r.right; x++)
            {
                UINT32 p = src[x];
                unsigned int a = p >> 24, out = 0xff000000;
                if (a == 255) out = p;  /* what is transparent shows white, as in Windows */
                else for (int c = 0; c < 24; c += 8) out |= ((((p >> c) & 0xff) * a + 255 * (255 - a)) / 255) << c;
                dst[x] = out;
            }
        }
    }

    if (e->tool == TOOL_CROP && !IsRectEmpty( &e->crop ))
    {
        RECT c = e->crop, parts[4];
        struct pointf corner[3];
        float k = 1.5f * e->scale, len = 14 * e->scale;

        SetRect( &parts[0], e->image_rect.left, e->image_rect.top, e->image_rect.right, c.top );
        SetRect( &parts[1], e->image_rect.left, c.bottom, e->image_rect.right, e->image_rect.bottom );
        SetRect( &parts[2], e->image_rect.left, c.top, c.left, c.bottom );
        SetRect( &parts[3], c.right, c.top, e->image_rect.right, c.bottom );
        for (int i = 0; i < 4; i++) if (IntersectRect( &r, &parts[i], clip )) image_blend_rect( &e->back, &r, 0x99000000 );
        for (int i = 0; i < 4; i++)  /* the corners, as handles */
        {
            float x = i & 1 ? c.right : c.left, y = i & 2 ? c.bottom : c.top;
            float dx = i & 1 ? -len : len, dy = i & 2 ? -len : len;
            corner[0].x = x + dx; corner[0].y = y;
            corner[1].x = x;      corner[1].y = y;
            corner[2].x = x;      corner[2].y = y + dy;
            ink_polyline( &e->back, corner, 3, FALSE, k, 0xffffffff, clip );
        }
    }
}

static void render( struct editor *e, const RECT *area )
{
    RECT client, r;

    if (!e->back.bits) return;
    GetClientRect( e->hwnd, &client );
    if (!IntersectRect( &r, area, &client )) return;
    GdiFlush();
    draw_canvas( e, &r );
    draw_bar( e, &r );
    InvalidateRect( e->hwnd, &r, FALSE );
}

static void render_all( struct editor *e )
{
    RECT client;

    GetClientRect( e->hwnd, &client );
    render( e, &client );
}

/* where things are, for the window's size */

static void layout( struct editor *e )
{
    RECT client, old = e->image_rect;
    int size = px( e, 40 ), height = px( e, 48 ), y, x, width, total;
    SIZE text;

    GetClientRect( e->hwnd, &client );
    SetRect( &e->bar, 0, 0, client.right, height );
    SetRect( &e->canvas, 0, height, client.right, max( height, client.bottom ) );
    y = (height - size) / 2;

    GetTextExtentPoint32W( e->back_dc, string( IDS_NEW ), wcslen( string( IDS_NEW ) ), &text );
    SetRect( &e->rects[B_NEW], px( e, 8 ), y, px( e, 8 ) + px( e, 44 ) + text.cx, y + size );

    x = client.right - px( e, 8 ) - 3 * size;
    for (int i = B_COPY; i <= B_FOLDER; i++, x += size) SetRect( &e->rects[i], x, y, x + size, y + size );

    total = 6 * size + px( e, 16 );
    x = (client.right - total) / 2;
    x = max( x, e->rects[B_NEW].right + px( e, 16 ) );
    for (int i = B_PEN; i <= B_REDO; i++)
    {
        if (i == B_UNDO) x += px( e, 16 );
        SetRect( &e->rects[i], x, y, x + size, y + size );
        x += size;
    }

    if (e->tooltip)
        for (int i = 0; i < B_COUNT; i++)
        {
            TTTOOLINFOW info = { .cbSize = sizeof(info), .hwnd = e->hwnd, .uId = i, .rect = e->rects[i] };
            SendMessageW( e->tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&info );
        }

    if (!e->live)
    {
        image_release( e->view );
        e->view = NULL;
        SetRectEmpty( &e->image_rect );
        return;
    }
    width = e->canvas.right - e->canvas.left - 2 * px( e, 24 );
    total = e->canvas.bottom - e->canvas.top - 2 * px( e, 24 );
    e->zoom = min( 1.0f, min( (float)max( 1, width ) / e->live->width, (float)max( 1, total ) / e->live->height ) );
    width = max( 1, (int)floorf( e->live->width * e->zoom + 0.5f ) );
    total = max( 1, (int)floorf( e->live->height * e->zoom + 0.5f ) );
    x = (e->canvas.left + e->canvas.right - width) / 2;
    y = (e->canvas.top + e->canvas.bottom - total) / 2;
    SetRect( &e->image_rect, x, y, x + width, y + total );
    if (!e->view || e->view->width != width || e->view->height != total || !EqualRect( &old, &e->image_rect ))
    {
        RECT all = { 0, 0, width, total };
        image_release( e->view );
        if ((e->view = image_create( width, total ))) ink_scale( e->live, e->view, e->zoom, &all );
    }
}

/* the composition again, from the snip and the strokes */
static void rebuild( struct editor *e )
{
    RECT all;

    image_release( e->comp );
    image_release( e->live );
    image_release( e->view );
    e->comp = e->live = e->view = NULL;
    if (e->base)
    {
        image_rect( e->base, &all );
        e->comp = image_crop( e->base, &all );
        for (int i = 0; e->comp && i < e->count; i++) stroke_render( e->comp, e->strokes[i], NULL );
        if (e->comp) e->live = image_crop( e->comp, &all );
    }
    layout( e );
    render_all( e );
}

/* rect of the snip changed in live: its part of the view and the window */
static void refresh( struct editor *e, const RECT *rect )
{
    RECT r, all, v;

    if (!e->live) return;
    image_rect( e->live, &all );
    if (!IntersectRect( &r, rect, &all )) return;
    image_copy_rect( e->live, e->comp, &r );
    if (e->drawing) stroke_render( e->live, e->drawing, &r );
    if (!e->view) return;
    SetRect( &v, (int)floorf( r.left * e->zoom ) - 1, (int)floorf( r.top * e->zoom ) - 1,
             (int)ceilf( r.right * e->zoom ) + 1, (int)ceilf( r.bottom * e->zoom ) + 1 );
    ink_scale( e->live, e->view, e->zoom, &v );
    OffsetRect( &v, e->image_rect.left, e->image_rect.top );
    render( e, &v );
}

/* a change: to the clipboard in a moment, as Windows' "copy changes" */
static void changed( struct editor *e )
{
    SetTimer( e->hwnd, TIMER_COPY, 300, NULL );
    render( e, &e->bar );
}

static void set_document( struct editor *e, struct image *image, const WCHAR *path )
{
    history_clear( &e->undo );
    history_clear( &e->redo );
    release_strokes( e );
    image_release( e->base );
    e->base = image;
    lstrcpynW( e->path, path ? path : L"", ARRAY_SIZE(e->path) );
    e->tool = TOOL_NONE;
    SetRectEmpty( &e->crop );
    rebuild( e );
}

static void restore( struct editor *e, struct state *state )
{
    release_strokes( e );
    image_release( e->base );
    e->base = state->base;
    e->strokes = state->strokes;
    e->count = e->capacity = state->count;
    memset( state, 0, sizeof(*state) );
    rebuild( e );
}

static void undo( struct editor *e )
{
    if (!e->undo.count || e->drawing) return;
    history_push( &e->redo, snapshot( e ) );
    restore( e, &e->undo.items[--e->undo.count] );
    changed( e );
}

static void redo( struct editor *e )
{
    if (!e->redo.count || e->drawing) return;
    history_push( &e->undo, snapshot( e ) );
    restore( e, &e->redo.items[--e->redo.count] );
    changed( e );
}

static struct pointf to_image( const struct editor *e, POINT pt )
{
    struct pointf p = { (pt.x + 0.5f - e->image_rect.left) / e->zoom, (pt.y + 0.5f - e->image_rect.top) / e->zoom };
    return p;
}

static void erase_at( struct editor *e, POINT pt )
{
    struct pointf p = to_image( e, pt );
    BOOL removed = FALSE;

    for (int i = e->count - 1; i >= 0; i--)
    {
        if (!stroke_hit( e->strokes[i], p.x, p.y, 6 * e->scale / e->zoom )) continue;
        if (!e->erased) push_undo( e );
        e->erased = removed = TRUE;
        stroke_release( e->strokes[i] );
        memmove( e->strokes + i, e->strokes + i + 1, (e->count - i - 1) * sizeof(*e->strokes) );
        e->count--;
    }
    if (removed) rebuild( e );
}

static void apply_crop( struct editor *e )
{
    struct image *base;
    RECT r, all;

    SetRect( &r, (int)floorf( (e->crop.left - e->image_rect.left) / e->zoom ),
             (int)floorf( (e->crop.top - e->image_rect.top) / e->zoom ),
             (int)ceilf( (e->crop.right - e->image_rect.left) / e->zoom ),
             (int)ceilf( (e->crop.bottom - e->image_rect.top) / e->zoom ) );
    image_rect( e->base, &all );
    SetRectEmpty( &e->crop );
    if (!IntersectRect( &r, &r, &all ) || r.right - r.left < 2 || r.bottom - r.top < 2 || EqualRect( &r, &all ))
    {
        render_all( e );
        return;
    }
    if (!(base = image_crop( e->base, &r ))) return;
    push_undo( e );
    for (int i = 0; i < e->count; i++)  /* the strokes stay where they were on the snip */
    {
        struct stroke *moved = stroke_moved( e->strokes[i], (float)-r.left, (float)-r.top );
        stroke_release( e->strokes[i] );
        e->strokes[i] = moved;
    }
    for (int i = e->count - 1; i >= 0; i--)
        if (!e->strokes[i]) memmove( e->strokes + i, e->strokes + i + 1, (--e->count - i) * sizeof(*e->strokes) );
    image_release( e->base );
    e->base = base;
    e->tool = TOOL_NONE;
    rebuild( e );
    changed( e );
}

/* actions */

static void set_tool( struct editor *e, enum tool tool )
{
    e->tool = e->tool == tool ? TOOL_NONE : tool;
    SetRectEmpty( &e->crop );
    render_all( e );
}

static void pump( DWORD ms )
{
    MSG msg;

    for (DWORD start = GetTickCount(); GetTickCount() - start < ms;)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT)
            {
                PostQuitMessage( (int)msg.wParam );
                return;
            }
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
    }
}

/* New: the window steps aside while the screen is snipped, as in Windows */
static void new_snip( struct editor *e )
{
    struct image *image;
    WCHAR path[MAX_PATH];

    ShowWindow( e->hwnd, SW_HIDE );
    pump( 350 );  /* until dwm.exe no longer shows it */
    if ((image = snip_screen()))
    {
        if (!autosave_path( path, ARRAY_SIZE(path) ) || FAILED(save_image( path, image ))) path[0] = 0;
        clipboard_set_image( e->hwnd, image );
        set_document( e, image, path );
    }
    ShowWindow( e->hwnd, SW_SHOW );
    SetForegroundWindow( e->hwnd );
}

static void copy( struct editor *e )
{
    KillTimer( e->hwnd, TIMER_COPY );
    if (e->comp) clipboard_set_image( e->hwnd, e->comp );
}

static void save_as( struct editor *e )
{
    WCHAR file[MAX_PATH] = L"", folder[MAX_PATH], filter[128], *name;
    OPENFILENAMEW ofn = { .lStructSize = sizeof(ofn) };

    if (!e->comp) return;
    if (e->path[0] && (name = wcsrchr( e->path, '\\' ))) lstrcpynW( file, name + 1, ARRAY_SIZE(file) );
    lstrcpynW( filter, string( IDS_FILTER ), ARRAY_SIZE(filter) - 1 );
    filter[wcslen( filter ) + 1] = 0;
    for (WCHAR *p = filter; *p; p++) if (*p == '|') *p = 0;
    if (!screenshots_folder( folder, ARRAY_SIZE(folder) )) folder[0] = 0;

    ofn.hwndOwner = e->hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_SIZE(file);
    ofn.lpstrInitialDir = folder[0] ? folder : NULL;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW( &ofn )) return;
    if (ofn.nFilterIndex == 2 && !wcsrchr( file, '.' )) wcscat( file, L".jpg" );
    if (FAILED(save_image( file, e->comp )))
        MessageBoxW( e->hwnd, string( IDS_SAVE_FAILED ), string( IDS_APP_NAME ), MB_OK | MB_ICONERROR );
}

static void open_folder( struct editor *e )
{
    WCHAR args[MAX_PATH + 16], folder[MAX_PATH];

    if (e->path[0] && GetFileAttributesW( e->path ) != INVALID_FILE_ATTRIBUTES)
    {
        swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", e->path );
        ShellExecuteW( e->hwnd, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
    }
    else if (screenshots_folder( folder, ARRAY_SIZE(folder) ))
        ShellExecuteW( e->hwnd, NULL, folder, NULL, NULL, SW_SHOWNORMAL );
}

/* the ballpoint pen's colours and widths, under its button */
static void pen_menu( struct editor *e )
{
    HMENU menu = CreatePopupMenu();
    POINT pt = { e->rects[B_PEN].left, e->rects[B_PEN].bottom };
    int id;

    for (int i = 0; i < ARRAY_SIZE(pen_colors); i++)
        AppendMenuW( menu, MF_STRING | (i == e->color ? MF_CHECKED : 0), ID_COLOR + i, string( IDS_COLOR_BLACK + i ) );
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    for (int i = 0; i < ARRAY_SIZE(pen_widths); i++)
        AppendMenuW( menu, MF_STRING | (i == e->width ? MF_CHECKED : 0), ID_WIDTH + i, string( IDS_SIZE_THIN + i ) );
    ClientToScreen( e->hwnd, &pt );
    id = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, e->hwnd, NULL );
    DestroyMenu( menu );
    if (id >= ID_COLOR && id < ID_COLOR + ARRAY_SIZE(pen_colors)) set_setting( L"PenColor", e->color = id - ID_COLOR );
    if (id >= ID_WIDTH && id < ID_WIDTH + ARRAY_SIZE(pen_widths)) set_setting( L"PenWidth", e->width = id - ID_WIDTH );
    render( e, &e->bar );
}

static void click( struct editor *e, int button )
{
    if (!enabled( e, button )) return;
    switch (button)
    {
    case B_NEW:         new_snip( e ); break;
    case B_PEN:         if (e->tool == TOOL_PEN) pen_menu( e ); else set_tool( e, TOOL_PEN ); break;
    case B_HIGHLIGHTER: set_tool( e, TOOL_HIGHLIGHTER ); break;
    case B_ERASER:      set_tool( e, TOOL_ERASER ); break;
    case B_CROP:        set_tool( e, TOOL_CROP ); break;
    case B_UNDO:        undo( e ); break;
    case B_REDO:        redo( e ); break;
    case B_COPY:        copy( e ); break;
    case B_SAVE:        save_as( e ); break;
    case B_FOLDER:      open_folder( e ); break;
    }
}

static int hit( const struct editor *e, POINT pt )
{
    for (int i = 0; i < B_COUNT; i++) if (PtInRect( &e->rects[i], pt )) return i;
    return -1;
}

static void set_hot( struct editor *e, int hot )
{
    if (hot == e->hot) return;
    e->hot = hot;
    render( e, &e->bar );
}

static void mouse_down( struct editor *e, POINT pt )
{
    int button = hit( e, pt );

    if (button >= 0)
    {
        if (!enabled( e, button )) return;
        e->pressed = button;
        SetCapture( e->hwnd );
        render( e, &e->bar );
        return;
    }
    if (!e->base || !PtInRect( &e->image_rect, pt )) return;
    switch (e->tool)
    {
    case TOOL_PEN:
    case TOOL_HIGHLIGHTER:
    {
        BOOL marker = e->tool == TOOL_HIGHLIGHTER;
        float width = marker ? HIGHLIGHTER_WIDTH : pen_widths[e->width];
        struct pointf p = to_image( e, pt );
        RECT dirty;

        if (!(e->drawing = stroke_create( marker ? HIGHLIGHTER_COLOR : pen_colors[e->color],
                                          width * e->scale / e->zoom / 2, marker )))
            return;
        stroke_add( e->drawing, p.x, p.y );
        SetCapture( e->hwnd );
        stroke_bounds( e->drawing, 0, &dirty );
        refresh( e, &dirty );
        break;
    }
    case TOOL_ERASER:
        e->erasing = TRUE;
        e->erased = FALSE;
        SetCapture( e->hwnd );
        erase_at( e, pt );
        break;
    case TOOL_CROP:
        e->cropping = TRUE;
        e->crop_from = pt;
        SetRectEmpty( &e->crop );
        SetCapture( e->hwnd );
        break;
    default:
        break;
    }
}

static void mouse_move( struct editor *e, POINT pt )
{
    if (!e->tracking)
    {
        TRACKMOUSEEVENT track = { .cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = e->hwnd };
        e->tracking = TrackMouseEvent( &track );
    }
    if (e->pressed < 0 && !e->drawing && !e->erasing && !e->cropping) set_hot( e, hit( e, pt ) );

    if (e->drawing)
    {
        struct pointf p = to_image( e, pt ), *last = &e->drawing->points[e->drawing->count - 1];
        RECT dirty;

        if (fabsf( p.x - last->x ) * e->zoom < 1 && fabsf( p.y - last->y ) * e->zoom < 1) return;
        stroke_add( e->drawing, p.x, p.y );
        stroke_bounds( e->drawing, e->drawing->count - 1, &dirty );
        refresh( e, &dirty );
    }
    else if (e->erasing) erase_at( e, pt );
    else if (e->cropping)
    {
        RECT area = e->image_rect;
        SetRect( &e->crop, min( e->crop_from.x, pt.x ), min( e->crop_from.y, pt.y ), max( e->crop_from.x, pt.x ),
                 max( e->crop_from.y, pt.y ) );
        IntersectRect( &e->crop, &e->crop, &e->image_rect );
        InflateRect( &area, px( e, 4 ), px( e, 4 ) );
        render( e, &area );
    }
}

static void mouse_up( struct editor *e, POINT pt )
{
    if (e->pressed >= 0)
    {
        int button = e->pressed;
        e->pressed = -1;
        ReleaseCapture();
        render( e, &e->bar );
        if (hit( e, pt ) == button) click( e, button );
        return;
    }
    if (e->drawing)
    {
        struct stroke *stroke = e->drawing;
        e->drawing = NULL;
        ReleaseCapture();
        push_undo( e );
        stroke_render( e->comp, stroke, NULL );  /* live already shows it */
        add_stroke( e, stroke );
        changed( e );
    }
    else if (e->erasing)
    {
        e->erasing = FALSE;
        ReleaseCapture();
        if (e->erased) changed( e );
    }
    else if (e->cropping)
    {
        e->cropping = FALSE;
        ReleaseCapture();
        apply_crop( e );
    }
}

static void create_back_buffer( struct editor *e )
{
    RECT client;
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biPlanes = 1, .biBitCount = 32 } };
    void *bits;

    GetClientRect( e->hwnd, &client );
    if (e->back_bitmap)
    {
        if (e->back.width == client.right && e->back.height == client.bottom) return;
        SelectObject( e->back_dc, GetStockObject( DEFAULT_GUI_FONT ) );
        DeleteObject( e->back_bitmap );
        e->back_bitmap = 0;
        e->back.bits = NULL;
    }
    if (client.right <= 0 || client.bottom <= 0) return;
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    if (!(e->back_bitmap = CreateDIBSection( e->back_dc, &info, DIB_RGB_COLORS, &bits, NULL, 0 ))) return;
    SelectObject( e->back_dc, e->back_bitmap );
    SelectObject( e->back_dc, e->font );
    e->back.ref = 1;
    e->back.width = client.right;
    e->back.height = client.bottom;
    e->back.bits = bits;
    e->back.owned = FALSE;
}

static void set_dpi( struct editor *e, UINT dpi )
{
    e->dpi = dpi;
    e->scale = dpi / 96.0f;
    if (e->font) DeleteObject( e->font );
    e->font = ui_font( dpi, px( e, 14 ), FW_NORMAL );
    SelectObject( e->back_dc, e->font );
    if (e->tooltip) SendMessageW( e->tooltip, WM_SETFONT, (WPARAM)e->font, FALSE );
}

static void create_tooltips( struct editor *e )
{
    if (!(e->tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                        0, 0, 0, 0, e->hwnd, 0, instance, NULL )))
        return;
    SendMessageW( e->tooltip, WM_SETFONT, (WPARAM)e->font, FALSE );
    for (int i = 0; i < B_COUNT; i++)
    {
        TTTOOLINFOW info = { .cbSize = sizeof(info), .uFlags = TTF_SUBCLASS, .hwnd = e->hwnd, .uId = i,
                             .rect = e->rects[i], .lpszText = (WCHAR *)string( buttons[i].tip ) };
        SendMessageW( e->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info );
    }
}

static void destroy( struct editor *e )
{
    history_clear( &e->undo );
    history_clear( &e->redo );
    free( e->undo.items );
    free( e->redo.items );
    release_strokes( e );
    stroke_release( e->drawing );
    image_release( e->base );
    image_release( e->comp );
    image_release( e->live );
    image_release( e->view );
    if (e->back_dc) DeleteDC( e->back_dc );
    if (e->back_bitmap) DeleteObject( e->back_bitmap );
    if (e->font) DeleteObject( e->font );
    free( e );
}

static LRESULT CALLBACK editor_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct editor *e = (struct editor *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { GET_X_LPARAM( lparam ), GET_Y_LPARAM( lparam ) };

    if (!e && msg != WM_NCCREATE) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_NCCREATE:
        e = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        e->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)e );
        break;

    case WM_SIZE:
        create_back_buffer( e );
        layout( e );
        render_all( e );
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = px( e, 560 );
        info->ptMinTrackSize.y = px( e, 240 );
        return 0;
    }

    case WM_DPICHANGED:
    {
        const RECT *rect = (const RECT *)lparam;
        set_dpi( e, LOWORD( wparam ) );
        SetWindowPos( hwnd, 0, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                      SWP_NOZORDER | SWP_NOACTIVATE );
        layout( e );
        render_all( e );
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        GdiFlush();
        BitBlt( hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                ps.rcPaint.bottom - ps.rcPaint.top, e->back_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        if (LOWORD( lparam ) == HTCLIENT)
        {
            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            SetCursor( LoadCursorW( NULL, e->tool != TOOL_NONE && PtInRect( &e->image_rect, pt ) ? (const WCHAR *)IDC_CROSS
                                                                                                  : (const WCHAR *)IDC_ARROW ) );
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        mouse_move( e, pt );
        return 0;

    case WM_MOUSELEAVE:
        e->tracking = FALSE;
        set_hot( e, -1 );
        return 0;

    case WM_LBUTTONDOWN:
        mouse_down( e, pt );
        return 0;

    case WM_LBUTTONUP:
        mouse_up( e, pt );
        return 0;

    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd)
        {
            POINT none = { -1, -1 };
            if (e->drawing || e->erasing || e->cropping || e->pressed >= 0) mouse_up( e, none );
        }
        return 0;

    case WM_KEYDOWN:
    {
        BOOL ctrl = GetKeyState( VK_CONTROL ) < 0, shift = GetKeyState( VK_SHIFT ) < 0;

        if (wparam == VK_ESCAPE && e->tool != TOOL_NONE) set_tool( e, e->tool );
        else if (ctrl && wparam == 'N') click( e, B_NEW );
        else if (ctrl && wparam == 'C') click( e, B_COPY );
        else if (ctrl && wparam == 'S') click( e, B_SAVE );
        else if (ctrl && wparam == 'Z') shift ? redo( e ) : undo( e );
        else if (ctrl && wparam == 'Y') redo( e );
        return 0;
    }

    case WM_TIMER:
        if (wparam == TIMER_COPY) copy( e );
        return 0;

    case WM_DESTROY:
        if (KillTimer( hwnd, TIMER_COPY ) && e->comp) clipboard_set_image( 0, e->comp );  /* a change not copied yet */
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        destroy( e );
        editor_closed();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* a window for image (taken) kept at path; with no image, one that waits for New */
HWND editor_open( struct image *image, const WCHAR *path )
{
    static const WCHAR class_name[] = L"ArcticSnippingTool";
    static BOOL registered;
    MONITORINFO info = { .cbSize = sizeof(info) };
    UINT dpi_x = 96, dpi_y = 96;
    struct editor *e;
    HMONITOR monitor;
    POINT pt;
    RECT rect;
    int width, height, bar;
    float s;

    if (!(e = calloc( 1, sizeof(*e) )))
    {
        image_release( image );
        return 0;
    }
    if (!registered)
    {
        WNDCLASSW class = { .style = CS_DBLCLKS, .lpfnWndProc = editor_proc, .hInstance = instance,
                            .hIcon = LoadIconW( instance, MAKEINTRESOURCEW( IDI_SNIPPINGTOOL ) ),
                            .hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW ), .lpszClassName = class_name };
        registered = RegisterClassW( &class ) != 0;
    }
    e->hot = e->pressed = -1;
    e->color = min( setting( L"PenColor", 1 ), ARRAY_SIZE(pen_colors) - 1 );
    e->width = min( setting( L"PenWidth", 1 ), ARRAY_SIZE(pen_widths) - 1 );
    get_theme( &e->theme );
    e->back_dc = CreateCompatibleDC( NULL );

    /* the snip at its size if the monitor has room, else what it has */
    GetCursorPos( &pt );
    monitor = MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY );
    GetMonitorInfoW( monitor, &info );
    GetDpiForMonitor( monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y );
    s = dpi_x / 96.0f;
    bar = (int)(48 * s);
    width = image ? image->width + (int)(48 * s) : (int)(640 * s);
    height = image ? image->height + (int)(48 * s) + bar : (int)(300 * s);
    width = max( width, (int)(720 * s) );
    height = max( height, (int)(420 * s) );
    width = min( width, (info.rcWork.right - info.rcWork.left) * 85 / 100 );
    height = min( height, (info.rcWork.bottom - info.rcWork.top) * 85 / 100 );
    SetRect( &rect, 0, 0, width, height );
    AdjustWindowRectExForDpi( &rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_x );
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    e->dpi = dpi_x;
    e->scale = s;
    e->font = ui_font( dpi_x, (int)(14 * s + 0.5f), FW_NORMAL );
    SelectObject( e->back_dc, e->font );

    if (!CreateWindowExW( 0, class_name, string( IDS_APP_NAME ), WS_OVERLAPPEDWINDOW,
                          (info.rcWork.left + info.rcWork.right - width) / 2,
                          (info.rcWork.top + info.rcWork.bottom - height) / 2, width, height, 0, 0, instance, e ))
    {
        image_release( image );
        destroy( e );
        return 0;
    }
    editor_opened();
    if (GetDpiForWindow( e->hwnd ) != e->dpi) set_dpi( e, GetDpiForWindow( e->hwnd ) );
    create_back_buffer( e );
    layout( e );
    create_tooltips( e );
    set_document( e, image, path );
    ShowWindow( e->hwnd, SW_SHOWNORMAL );
    SetForegroundWindow( e->hwnd );
    return e->hwnd;
}
