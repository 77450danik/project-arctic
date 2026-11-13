/*
 * Paint: the tools under the title bar, the status bar, the text's bar
 *
 * Windows 11's Paint has one row of groups with a name under each:
 * Selection, Image, Tools, Brushes, Shapes, Size, Colours. They are drawn
 * here, as the rest of the window, from the pictures of tools/paint-icons.py.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "paint.h"

struct tools tools =
{
    .tool = TOOL_PENCIL,
    .before_picker = TOOL_PENCIL,
    .brush = BR_BRUSH,
    .shape = SH_RECT,
    .outline = ST_SOLID,
    .fill = ST_NONE,
    .size = { [TOOL_SELECT] = 1, [TOOL_PENCIL] = 1, [TOOL_FILL] = 1, [TOOL_TEXT] = 1, [TOOL_ERASER] = 8,
              [TOOL_PICKER] = 1, [TOOL_MAGNIFIER] = 1, [TOOL_BRUSH] = 5, [TOOL_SHAPE] = 3 },
    .color1 = RGB( 0, 0, 0 ),
    .color2 = RGB( 255, 255, 255 ),
};

const char *const brush_icons[BR_COUNT] =
{
    "brush", "callig1", "callig2", "airbrush", "oilbrush", "crayon", "marker", "natpencil", "watercolor",
};

const char *const shape_icons[SH_COUNT] =
{
    "sh_line", "sh_curve", "sh_oval", "sh_rect", "sh_rrect", "sh_polygon", "sh_triangle", "sh_rtriangle",
    "sh_diamond", "sh_pentagon", "sh_hexagon", "sh_aright", "sh_aleft", "sh_aup", "sh_adown", "sh_star4",
    "sh_star5", "sh_star6", "sh_crect", "sh_coval", "sh_ccloud", "sh_heart", "sh_lightning",
};

const WCHAR *brush_name( int kind )
{
    switch (kind)
    {
    case BR_BRUSH: return tr( L"Brush", L"Пензель" );
    case BR_CALLIGRAPHY: return tr( L"Calligraphy brush", L"Каліграфічний пензель" );
    case BR_PEN: return tr( L"Calligraphy pen", L"Каліграфічне перо" );
    case BR_AIRBRUSH: return tr( L"Airbrush", L"Аерограф" );
    case BR_OIL: return tr( L"Oil brush", L"Пензель для олійних фарб" );
    case BR_CRAYON: return tr( L"Crayon", L"Крейда" );
    case BR_MARKER: return tr( L"Marker", L"Маркер" );
    case BR_NATURAL: return tr( L"Natural pencil", L"Простий олівець" );
    default: return tr( L"Watercolour brush", L"Пензель для акварелі" );
    }
}

const WCHAR *shape_name( int kind )
{
    static const WCHAR *const en[SH_COUNT] =
    {
        L"Line", L"Curve", L"Oval", L"Rectangle", L"Rounded rectangle", L"Polygon", L"Triangle", L"Right triangle",
        L"Diamond", L"Pentagon", L"Hexagon", L"Right arrow", L"Left arrow", L"Up arrow", L"Down arrow",
        L"Four-point star", L"Five-point star", L"Six-point star", L"Rounded rectangular callout", L"Oval callout",
        L"Cloud callout", L"Heart", L"Lightning",
    };
    static const WCHAR *const uk[SH_COUNT] =
    {
        L"Лінія", L"Крива", L"Овал", L"Прямокутник", L"Округлений прямокутник", L"Багатокутник", L"Трикутник",
        L"Прямокутний трикутник", L"Ромб", L"П'ятикутник", L"Шестикутник", L"Стрілка вправо", L"Стрілка вліво",
        L"Стрілка вгору", L"Стрілка вниз", L"Чотирикутна зірка", L"П'ятикутна зірка", L"Шестикутна зірка",
        L"Округлена прямокутна виноска", L"Овальна виноска", L"Виноска-хмарка", L"Серце", L"Блискавка",
    };
    return tr( en[kind], uk[kind] );
}

static const COLORREF palette[20] =
{
    RGB( 0x00, 0x00, 0x00 ), RGB( 0x7f, 0x7f, 0x7f ), RGB( 0x88, 0x00, 0x15 ), RGB( 0xed, 0x1c, 0x24 ),
    RGB( 0xff, 0x7f, 0x27 ), RGB( 0xff, 0xf2, 0x00 ), RGB( 0x22, 0xb1, 0x4c ), RGB( 0x00, 0xa2, 0xe8 ),
    RGB( 0x3f, 0x48, 0xcc ), RGB( 0xa3, 0x49, 0xa4 ),
    RGB( 0xff, 0xff, 0xff ), RGB( 0xc3, 0xc3, 0xc3 ), RGB( 0xb9, 0x7a, 0x57 ), RGB( 0xff, 0xae, 0xc9 ),
    RGB( 0xff, 0xc9, 0x0e ), RGB( 0xef, 0xe4, 0xb0 ), RGB( 0xb5, 0xe6, 0x1d ), RGB( 0x99, 0xd9, 0xea ),
    RGB( 0x70, 0x92, 0xbe ), RGB( 0xc8, 0xbf, 0xe7 ),
};

/* the thicknesses a tool offers */
static int size_choices( enum tool tool, int *out )
{
    static const int pencil[] = { 1, 2, 3, 4 }, eraser[] = { 4, 6, 8, 10 }, brush[] = { 3, 5, 8, 16 },
                     air[] = { 8, 16, 24, 32 }, shape[] = { 1, 3, 5, 8 };
    const int *c = tool == TOOL_PENCIL ? pencil : tool == TOOL_ERASER ? eraser :
                   tool == TOOL_BRUSH ? (tools.brush == BR_AIRBRUSH ? air : brush) : shape;

    memcpy( out, c, 4 * sizeof(int) );
    return 4;
}

static enum tool size_tool(void)
{
    switch (tools.tool)
    {
    case TOOL_PENCIL: case TOOL_ERASER: case TOOL_BRUSH: return tools.tool;
    default: return TOOL_SHAPE;
    }
}

/**********************************************************************
 *          The zones of the ribbon
 */

enum zone_kind { Z_BUTTON, Z_SPLIT, Z_DROP, Z_SHAPE, Z_COLOR, Z_CUSTOM, Z_CURRENT, Z_LABEL, Z_SEP };

struct zone
{
    enum zone_kind kind;
    RECT r;
    int cmd;            /* the command, or what the drop shows */
    int arg;
    const char *icon;
    const WCHAR *text;  /* the group's name, a tooltip */
};

static HWND ribbon, tooltip;
static struct surface rsurf;
static struct zone zones[128];
static int zone_count, hot = -1, down = -1;

#define RIBBON_H px( 100 )

int ribbon_height(void) { return RIBBON_H; }

static struct zone *add( enum zone_kind kind, int x, int y, int w, int h, int cmd, const char *icon, const WCHAR *text )
{
    struct zone *z;

    if (zone_count >= ARRAY_SIZE(zones)) return &zones[ARRAY_SIZE(zones) - 1];
    z = &zones[zone_count++];
    z->kind = kind;
    SetRect( &z->r, x, y, x + w, y + h );
    z->cmd = cmd;
    z->icon = icon;
    z->text = text;
    z->arg = 0;
    return z;
}

/* a group's name under it, the line after it; returns where the next begins */
static int end_group( int x0, int x1, const WCHAR *name )
{
    int lw = text_width( get_font( F_SMALL ), name, -1 ), w = max( x1 - x0, lw + px( 8 ) );

    add( Z_LABEL, x0, RIBBON_H - px( 25 ), w, px( 20 ), 0, NULL, name );
    add( Z_SEP, x0 + w + px( 8 ), px( 12 ), 1, RIBBON_H - px( 24 ), 0, NULL, NULL );
    return x0 + w + px( 17 );
}

static void ribbon_layout(void)
{
    int x = px( 12 ), top = px( 8 ), bw = px( 32 ), bh = px( 30 ), x0;
    const char *sel_icon = tools.select == SEL_FREE ? "select_free" : "select_rect";

    zone_count = 0;

    /* Selection */
    x0 = x;
    add( Z_SPLIT, x, top, px( 44 ), px( 40 ), IDM_TOOL_FIRST + TOOL_SELECT, sel_icon, tr( L"Select", L"Виділити" ) );
    add( Z_DROP, x, top + px( 40 ), px( 44 ), px( 20 ), IDM_SEL_RECT, NULL, tr( L"Selection options", L"Параметри виділення" ) );
    x = end_group( x0, x + px( 44 ), tr( L"Selection", L"Виділення" ) );

    /* Image */
    x0 = x;
    add( Z_BUTTON, x, top, bw, bh, IDM_CROP, "crop", tr( L"Crop (Ctrl+Shift+X)", L"Обітнути (Ctrl+Shift+X)" ) );
    add( Z_BUTTON, x + bw + px( 2 ), top, bw, bh, IDM_RESIZE, "resize", tr( L"Resize and skew (Ctrl+W)", L"Змінити розмір і нахил (Ctrl+W)" ) );
    add( Z_DROP, x, top + bh + px( 2 ), bw + px( 18 ), bh, IDM_ROTATE_RIGHT, "rotate", tr( L"Rotate", L"Обернути" ) );
    x = end_group( x0, x + 2 * bw + px( 2 ), tr( L"Image", L"Зображення" ) );

    /* Tools */
    x0 = x;
    {
        static const struct { enum tool tool; const char *icon; } t[6] =
        {
            { TOOL_PENCIL, "pencil" }, { TOOL_FILL, "bucket" }, { TOOL_TEXT, "text" },
            { TOOL_ERASER, "eraser" }, { TOOL_PICKER, "picker" }, { TOOL_MAGNIFIER, "magnifier" },
        };
        static const WCHAR *en[6] = { L"Pencil", L"Fill", L"Text", L"Eraser", L"Colour picker", L"Magnifier" };
        static const WCHAR *uk[6] = { L"Олівець", L"Заливка", L"Текст", L"Гумка", L"Піпетка", L"Лупа" };
        for (int i = 0; i < 6; i++)
            add( Z_BUTTON, x + (i % 3) * (bw + px( 2 )), top + (i / 3) * (bh + px( 2 )), bw, bh,
                 IDM_TOOL_FIRST + t[i].tool, t[i].icon, tr( en[i], uk[i] ) );
    }
    x = end_group( x0, x + 3 * bw + 2 * px( 2 ), tr( L"Tools", L"Інструменти" ) );

    /* Brushes */
    x0 = x;
    add( Z_SPLIT, x, top, px( 44 ), px( 40 ), IDM_TOOL_FIRST + TOOL_BRUSH, brush_icons[tools.brush], brush_name( tools.brush ) );
    add( Z_DROP, x, top + px( 40 ), px( 44 ), px( 20 ), IDM_BRUSH_FIRST, NULL, tr( L"Brushes", L"Пензлі" ) );
    x = end_group( x0, x + px( 44 ), tr( L"Brushes", L"Пензлі" ) );

    /* Shapes: the gallery, the outline and the fill */
    x0 = x;
    {
        int cw = px( 26 ), ch = px( 21 ), cols = 8;
        for (int i = 0; i < SH_COUNT; i++)
        {
            struct zone *z = add( Z_SHAPE, x + (i % cols) * cw, top + (i / cols) * ch, cw, ch, IDM_SHAPE_FIRST + i,
                                  shape_icons[i], shape_name( i ) );
            z->arg = i;
        }
        x += cols * cw + px( 6 );
        add( Z_DROP, x, top, px( 50 ), bh, IDM_OUTLINE_SOLID, "outline", tr( L"Shape outline", L"Контур фігури" ) );
        add( Z_DROP, x, top + bh + px( 2 ), px( 50 ), bh, IDM_FILL_SOLID, "fillshape", tr( L"Shape fill", L"Заливка фігури" ) );
        x += px( 50 );
    }
    x = end_group( x0, x, tr( L"Shapes", L"Фігури" ) );

    /* Size */
    x0 = x;
    add( Z_SPLIT, x, top, px( 44 ), px( 40 ), IDM_SIZE_FIRST, "size", tr( L"Size", L"Розмір" ) )->kind = Z_DROP;
    add( Z_DROP, x, top + px( 40 ), px( 44 ), px( 20 ), IDM_SIZE_FIRST, NULL, tr( L"Size", L"Розмір" ) );
    x = end_group( x0, x + px( 44 ), tr( L"Size", L"Розмір" ) );

    /* Colours: the two in use, the palette, the custom ones, Edit colours */
    x0 = x;
    {
        int d = px( 20 ), gap = px( 4 );
        struct zone *z;
        z = add( Z_CURRENT, x, top, px( 32 ), px( 32 ), 0, NULL, tr( L"Colour 1", L"Колір 1" ) );
        z->arg = 1;
        z = add( Z_CURRENT, x + px( 4 ), top + px( 34 ), px( 24 ), px( 24 ), 0, NULL, tr( L"Colour 2", L"Колір 2" ) );
        z->arg = 2;
        x += px( 40 );
        for (int i = 0; i < 30; i++)
        {
            z = add( i < 20 ? Z_COLOR : Z_CUSTOM, x + (i % 10) * (d + gap), top + (i / 10) * (d + gap), d, d, 0, NULL, NULL );
            z->arg = i < 20 ? i : i - 20;
        }
        x += 10 * (d + gap) + px( 4 );
        add( Z_BUTTON, x, top, px( 44 ), px( 58 ), IDM_EDIT_COLORS, NULL, tr( L"Edit colours", L"Змінити кольори" ) );
        x += px( 44 );
    }
    end_group( x0, x, tr( L"Colours", L"Кольори" ) );
    zones[zone_count - 1].kind = Z_LABEL;   /* no line after the last */
    zones[zone_count - 1].text = NULL;

    if (tooltip)
    {
        int count = (int)SendMessageW( tooltip, TTM_GETTOOLCOUNT, 0, 0 );
        for (int i = count - 1; i >= 0; i--)
        {
            TTTOOLINFOW ti = { sizeof(ti) };
            if (SendMessageW( tooltip, TTM_ENUMTOOLSW, i, (LPARAM)&ti )) SendMessageW( tooltip, TTM_DELTOOLW, 0, (LPARAM)&ti );
        }
        for (int i = 0; i < zone_count; i++)
        {
            TTTOOLINFOW ti = { sizeof(ti), TTF_SUBCLASS, ribbon, i + 1 };
            WCHAR buf[64];
            if (zones[i].kind == Z_LABEL || zones[i].kind == Z_SEP) continue;
            if (zones[i].kind == Z_COLOR || zones[i].kind == Z_CUSTOM)
            {
                COLORREF c = zones[i].kind == Z_COLOR ? palette[zones[i].arg] : tools.custom[zones[i].arg];
                if (zones[i].kind == Z_CUSTOM && zones[i].arg >= tools.custom_count) continue;
                swprintf( buf, ARRAY_SIZE(buf), L"%s %d, %d, %d", tr( L"Red, green, blue:", L"RGB:" ),
                          GetRValue( c ), GetGValue( c ), GetBValue( c ) );
                ti.lpszText = buf;
            }
            else if (!zones[i].text) continue;
            else ti.lpszText = (WCHAR *)zones[i].text;
            ti.rect = zones[i].r;
            SendMessageW( tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti );
        }
    }
}

static BOOL zone_checked( const struct zone *z )
{
    int cmd = z->cmd;

    if (z->kind == Z_SHAPE) return tools.tool == TOOL_SHAPE && tools.shape == z->arg;
    if (z->kind == Z_SPLIT && cmd >= IDM_TOOL_FIRST && cmd < IDM_TOOL_FIRST + TOOL_COUNT)
        return tools.tool == cmd - IDM_TOOL_FIRST;
    if (z->kind == Z_BUTTON && cmd >= IDM_TOOL_FIRST && cmd < IDM_TOOL_FIRST + TOOL_COUNT)
        return tools.tool == cmd - IDM_TOOL_FIRST;
    return FALSE;
}

static void draw_color_dot( struct surface *s, const RECT *r, COLORREF c, BOOL selected, BOOL empty )
{
    float cx = (r->left + r->right) / 2.0f, cy = (r->top + r->bottom) / 2.0f, rad = (r->right - r->left) / 2.0f - px( 1 );

    if (empty)
    {
        ring( s, cx, cy, rad, 1.0f, th.faint, 255 );
        return;
    }
    fill_circle( s, cx, cy, rad, c, 255 );
    ring( s, cx, cy, rad, 1.0f, th.dark ? RGB( 0x6a, 0x6a, 0x6a ) : RGB( 0xb8, 0xb8, 0xb8 ), 255 );
    if (selected)
    {
        ring( s, cx, cy, rad + px( 3 ), (float)px( 2 ), th.accent, 255 );
    }
}

static void ribbon_paint( HDC hdc )
{
    RECT rc;
    int icon = px( 20 ), big = px( 24 ), sm = px( 16 );

    GetClientRect( ribbon, &rc );
    if (!surface_size( &rsurf, rc.right, rc.bottom )) return;
    fill( &rsurf, &rc, th.bar );
    {
        RECT line = { 0, rc.bottom - 1, rc.right, rc.bottom };
        fill( &rsurf, &line, th.bar_line );
    }
    for (int i = 0; i < zone_count; i++)
    {
        struct zone *z = &zones[i];
        RECT r = z->r;
        BOOL is_hot = hot == i, is_down = down == i, checked = zone_checked( z );
        BOOL enabled = z->cmd ? command_enabled( z->cmd ) : TRUE;

        switch (z->kind)
        {
        case Z_LABEL:
            if (z->text) draw_text( &rsurf, z->text, -1, &r, get_font( F_SMALL ), th.dim, DT_CENTER );
            break;
        case Z_SEP:
            fill( &rsurf, &r, th.bar_line );
            break;
        case Z_SHAPE:
        case Z_BUTTON:
        case Z_SPLIT:
        case Z_DROP:
        {
            BOOL pair_hot = FALSE;
            /* a split button lights as one, its parts apart when pressed */
            if (z->kind == Z_SPLIT && i + 1 < zone_count && hot == i + 1) pair_hot = TRUE;
            if (z->kind == Z_DROP && !z->icon && i > 0 && hot == i - 1 && zones[i - 1].kind != Z_LABEL) pair_hot = TRUE;
            if (checked) fill_round( &rsurf, &r, (float)px( 4 ), th.checked, 255 );
            else if (is_down) fill_round( &rsurf, &r, (float)px( 4 ), th.pressed, 255 );
            else if (is_hot && enabled) fill_round( &rsurf, &r, (float)px( 4 ), th.hover, 255 );
            else if (pair_hot) fill_round( &rsurf, &r, (float)px( 4 ), mix( th.bar, th.hover, 140 ), 255 );
            if (z->cmd == IDM_EDIT_COLORS)
            {
                color_wheel( &rsurf, (r.left + r.right) / 2.0f, r.top + px( 22 ), (float)px( 12 ) );
                ring( &rsurf, (r.left + r.right) / 2.0f, r.top + px( 22 ), (float)px( 12 ), 1.0f,
                      th.dark ? RGB( 0x6a, 0x6a, 0x6a ) : RGB( 0xb8, 0xb8, 0xb8 ), 255 );
                break;
            }
            if (z->kind == Z_SHAPE)
                draw_icon( &rsurf, z->icon, (r.left + r.right - sm) / 2, (r.top + r.bottom - sm) / 2, sm,
                           checked ? IS_ACCENT : IS_NORMAL );
            else if (z->kind == Z_SPLIT || (z->kind == Z_DROP && z->r.bottom - z->r.top >= px( 36 )))
                draw_icon( &rsurf, z->icon, (r.left + r.right - big) / 2, (r.top + r.bottom - big) / 2, big,
                           enabled ? IS_NORMAL : IS_DISABLED );
            else if (z->kind == Z_DROP && !z->icon)
                draw_glyph( &rsurf, G_DOWN_SMALL, (r.left + r.right - sm) / 2, (r.top + r.bottom - sm) / 2, sm, th.dim );
            else if (z->kind == Z_DROP)
            {
                int ix = r.left + px( 6 );
                draw_icon( &rsurf, z->icon, ix, (r.top + r.bottom - icon) / 2, icon, enabled ? IS_NORMAL : IS_DISABLED );
                draw_glyph( &rsurf, G_DOWN_SMALL, r.right - px( 18 ), (r.top + r.bottom - sm) / 2, sm, th.dim );
            }
            else
                draw_icon( &rsurf, z->icon, (r.left + r.right - icon) / 2, (r.top + r.bottom - icon) / 2, icon,
                           enabled ? IS_NORMAL : IS_DISABLED );
            break;
        }
        case Z_CURRENT:
            if (is_hot) fill_round( &rsurf, &(RECT){ r.left - px( 2 ), r.top - px( 2 ), r.right + px( 2 ), r.bottom + px( 2 ) },
                                    (float)px( 4 ), th.hover, 255 );
            draw_color_dot( &rsurf, &r, z->arg == 1 ? tools.color1 : tools.color2, FALSE, FALSE );
            break;
        case Z_COLOR:
            draw_color_dot( &rsurf, &r, palette[z->arg], is_hot, FALSE );
            break;
        case Z_CUSTOM:
            draw_color_dot( &rsurf, &r, tools.custom[z->arg], is_hot && z->arg < tools.custom_count,
                            z->arg >= tools.custom_count );
            break;
        }
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, rc.right, rc.bottom, rsurf.dc, 0, 0, SRCCOPY );
}

static int ribbon_hit( POINT pt )
{
    for (int i = 0; i < zone_count; i++)
        if (zones[i].kind != Z_LABEL && zones[i].kind != Z_SEP && PtInRect( &zones[i].r, pt )) return i;
    return -1;
}

/**********************************************************************
 *          What opens under the buttons
 */

static void anchor_of( int i, RECT *r )
{
    *r = zones[i].r;
    if (zones[i].kind == Z_DROP && !zones[i].icon && i > 0) r->top = zones[i - 1].r.top;
    MapWindowPoints( ribbon, NULL, (POINT *)r, 2 );
}

static struct popup size_popup;

static void size_paint( struct popup *p )
{
    int choices[4], n = size_choices( size_tool(), choices ), row = px( 36 );

    for (int i = 0; i < n; i++)
    {
        RECT r = { px( 4 ), px( 4 ) + i * row, p->width - px( 4 ), px( 4 ) + (i + 1) * row };
        int thick = max( 1, min( choices[i], px( 16 ) ) );
        RECT line = { r.left + px( 12 ), (r.top + r.bottom - thick) / 2, r.right - px( 70 ), (r.top + r.bottom - thick) / 2 + thick };
        RECT label = { r.right - px( 60 ), r.top, r.right - px( 8 ), r.bottom };
        WCHAR text[32];

        if (choices[i] == tools.size[size_tool()]) fill_round( &p->surf, &r, (float)px( 4 ), th.checked, 255 );
        else if (p->hot == i) fill_round( &p->surf, &r, (float)px( 4 ), th.hover, 255 );
        fill_round( &p->surf, &line, thick / 2.0f, th.text, 255 );
        swprintf( text, ARRAY_SIZE(text), L"%d %s", choices[i], tr( L"px", L"пкс" ) );
        draw_text( &p->surf, text, -1, &label, get_font( F_UI ), th.text, DT_RIGHT );
    }
}

static int size_hit( struct popup *p, POINT pt )
{
    int i = (pt.y - px( 4 )) / px( 36 );
    return pt.y >= px( 4 ) && i >= 0 && i < 4 ? i : -1;
}

static void open_drop( int i )
{
    struct zone *z = &zones[i];
    RECT anchor;
    int id = 0;

    anchor_of( i, &anchor );
    if (z->cmd == IDM_SEL_RECT)
    {
        struct menu_item items[] =
        {
            { IDM_SEL_RECT, tr( L"Rectangle", L"Прямокутник" ), "select_rect", NULL,
              (tools.select == SEL_RECT ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
            { IDM_SEL_FREE, tr( L"Free-form", L"Довільна форма" ), "select_free", NULL,
              (tools.select == SEL_FREE ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
            { 0 },
            { IDM_SELECT_ALL, tr( L"Select all", L"Виділити все" ), "selectall", L"Ctrl+A" },
            { IDM_INVERT_SEL, tr( L"Invert selection", L"Інвертувати виділення" ), "invertsel", NULL },
            { IDM_DELETE, tr( L"Delete", L"Видалити" ), "delete", L"Delete", command_enabled( IDM_DELETE ) ? 0 : MFS_DISABLED },
            { 0 },
            { IDM_TRANSPARENT, tr( L"Transparent selection", L"Прозоре виділення" ), NULL, NULL,
              tools.transparent_select ? MFS_CHECKED : 0 },
        };
        id = menu_run( frame, &anchor, items, ARRAY_SIZE(items) );
    }
    else if (z->cmd == IDM_ROTATE_RIGHT)
    {
        struct menu_item items[] =
        {
            { IDM_ROTATE_RIGHT, tr( L"Rotate right 90°", L"Обернути праворуч на 90°" ), "rotright" },
            { IDM_ROTATE_LEFT, tr( L"Rotate left 90°", L"Обернути ліворуч на 90°" ), "rotleft" },
            { IDM_ROTATE_180, tr( L"Rotate 180°", L"Обернути на 180°" ), "rotate" },
            { IDM_FLIP_V, tr( L"Flip vertical", L"Віддзеркалити вертикально" ), "flipv" },
            { IDM_FLIP_H, tr( L"Flip horizontal", L"Віддзеркалити горизонтально" ), "fliph" },
        };
        id = menu_run( frame, &anchor, items, ARRAY_SIZE(items) );
    }
    else if (z->cmd == IDM_BRUSH_FIRST)
    {
        struct menu_item items[BR_COUNT];
        memset( items, 0, sizeof(items) );
        for (int b = 0; b < BR_COUNT; b++)
        {
            items[b].id = IDM_BRUSH_FIRST + b;
            items[b].text = brush_name( b );
            items[b].icon = brush_icons[b];
            items[b].flags = tools.tool == TOOL_BRUSH && tools.brush == b ? MFS_CHECKED | MFT_RADIOCHECK : 0;
        }
        id = menu_run( frame, &anchor, items, BR_COUNT );
    }
    else if (z->cmd == IDM_OUTLINE_SOLID || z->cmd == IDM_FILL_SOLID)
    {
        BOOL out = z->cmd == IDM_OUTLINE_SOLID;
        enum style_kind cur = out ? tools.outline : tools.fill;
        struct menu_item items[] =
        {
            { out ? IDM_OUTLINE_NONE : IDM_FILL_NONE, out ? tr( L"No outline", L"Без контуру" ) : tr( L"No fill", L"Без заливки" ),
              NULL, NULL, (cur == ST_NONE ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
            { out ? IDM_OUTLINE_SOLID : IDM_FILL_SOLID, out ? tr( L"Solid colour", L"Суцільний колір" ) : tr( L"Solid colour", L"Суцільний колір" ),
              NULL, NULL, (cur == ST_SOLID ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        };
        id = menu_run( frame, &anchor, items, ARRAY_SIZE(items) );
    }
    else if (z->cmd == IDM_SIZE_FIRST)
    {
        int choices[4], r;
        size_choices( size_tool(), choices );
        size_popup.width = px( 200 );
        size_popup.height = 4 * px( 36 ) + px( 8 );
        size_popup.paint = size_paint;
        size_popup.hit = size_hit;
        if ((r = run_popup( frame, &anchor, &size_popup )) >= 0)
        {
            tools.size[size_tool()] = choices[r];
            canvas_style_changed();
        }
        ribbon_refresh();
        return;
    }
    if (id) command( id );
}

static void click_zone( int i, BOOL right )
{
    struct zone *z = &zones[i];

    switch (z->kind)
    {
    case Z_COLOR:
    case Z_CUSTOM:
    {
        COLORREF c;
        if (z->kind == Z_CUSTOM && z->arg >= tools.custom_count) { command( IDM_EDIT_COLORS ); return; }
        c = z->kind == Z_COLOR ? palette[z->arg] : tools.custom[z->arg];
        if (right) tools.color2 = c;
        else tools.color1 = c;
        canvas_style_changed();
        ribbon_refresh();
        return;
    }
    case Z_CURRENT:
        if (right) return;
        command( IDM_SWAP_COLORS );
        return;
    case Z_SHAPE:
        command( z->cmd );
        return;
    case Z_DROP:
        open_drop( i );
        return;
    default:
        if (z->cmd) command( z->cmd );
        return;
    }
}

static LRESULT CALLBACK ribbon_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        ribbon_paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int h = ribbon_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        if (h != hot)
        {
            hot = h;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        int h = ribbon_hit( pt );
        if (h < 0) return 0;
        if (zones[h].kind == Z_DROP)
        {
            down = h;
            InvalidateRect( hwnd, NULL, FALSE );
            UpdateWindow( hwnd );
            down = -1;
            click_zone( h, msg == WM_RBUTTONDOWN );
            hot = -1;
            InvalidateRect( hwnd, NULL, FALSE );
            return 0;
        }
        down = h;
        SetCapture( hwnd );
        InvalidateRect( hwnd, NULL, FALSE );
        if (msg == WM_RBUTTONDOWN) { ReleaseCapture(); down = -1; click_zone( h, TRUE ); }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int was = down;
        down = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect( hwnd, NULL, FALSE );
        if (was >= 0 && ribbon_hit( pt ) == was) click_zone( was, FALSE );
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        int h = ribbon_hit( pt );
        /* a custom colour to change it */
        if (h >= 0 && zones[h].kind == Z_CURRENT) command( IDM_EDIT_COLORS );
        return 0;
    }
    case WM_SIZE:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND ribbon_create( HWND parent )
{
    WNDCLASSW cls = { CS_DBLCLKS, ribbon_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintRibbon" };

    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    ribbon = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, NULL, instance, NULL );
    tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, ribbon,
                               NULL, instance, NULL );
    ribbon_layout();
    return ribbon;
}

void ribbon_refresh(void)
{
    if (!ribbon) return;
    ribbon_layout();
    InvalidateRect( ribbon, NULL, FALSE );
}

void ribbon_theme_changed(void)
{
    ribbon_refresh();
}

void set_tool( enum tool tool )
{
    if (tools.tool == tool) return;
    if (tool != TOOL_PICKER && tool != TOOL_MAGNIFIER) canvas_commit();
    if (tool == TOOL_PICKER && tools.tool != TOOL_PICKER) tools.before_picker = tools.tool;
    tools.tool = tool;
    ribbon_refresh();
    canvas_refresh();
}

/**********************************************************************
 *          The status bar
 */

static HWND status;
static struct surface ssurf;
static int status_hot = -1;
enum { S_ZOOMOUT, S_SLIDER, S_ZOOMIN, S_ZOOMTEXT, S_FIT, S_COUNT };
static RECT szones[S_COUNT];
static BOOL slider_drag;

extern ULONGLONG file_size;   /* main.c */

static double zoom_to_t( double z ) { return log( z / 0.125 ) / log( 2.0 ) / 6.0; }
static double t_to_zoom( double t ) { return 0.125 * pow( 2, max( 0.0, min( 1.0, t ) ) * 6.0 ); }

static void status_layout( int width, int height )
{
    int y = (height - px( 28 )) / 2, x = width - px( 12 );

    x -= px( 28 );
    SetRect( &szones[S_FIT], x, y, x + px( 28 ), y + px( 28 ) );
    x -= px( 8 ) + px( 56 );
    SetRect( &szones[S_ZOOMTEXT], x, y, x + px( 56 ), y + px( 28 ) );
    x -= px( 28 );
    SetRect( &szones[S_ZOOMIN], x, y, x + px( 28 ), y + px( 28 ) );
    x -= px( 130 );
    SetRect( &szones[S_SLIDER], x, y, x + px( 130 ), y + px( 28 ) );
    x -= px( 28 );
    SetRect( &szones[S_ZOOMOUT], x, y, x + px( 28 ), y + px( 28 ) );
}

static void status_paint( HDC hdc )
{
    RECT rc, r;
    POINT cur;
    BOOL in;
    SIZE sel;
    WCHAR text[96];
    int x = px( 14 ), icon = px( 16 );
    const WCHAR *unit = tr( L"px", L"пкс" );

    GetClientRect( status, &rc );
    if (!surface_size( &ssurf, rc.right, rc.bottom )) return;
    status_layout( rc.right, rc.bottom );
    fill( &ssurf, &rc, th.bar );
    r = rc;
    r.bottom = 1;
    fill( &ssurf, &r, th.bar_line );
    canvas_get_status( &cur, &in, &sel );

    /* where the mouse is, what is selected, the canvas, the file */
    {
        const char *icons[4] = { "st_cursor", "st_select", "st_canvas", "st_disk" };
        WCHAR parts[4][64];
        int widths[4] = { px( 130 ), px( 130 ), px( 140 ), px( 150 ) };

        parts[0][0] = parts[1][0] = parts[3][0] = 0;
        if (in) swprintf( parts[0], 64, L"%d, %d %s", cur.x, cur.y, unit );
        if (sel.cx) swprintf( parts[1], 64, L"%d × %d %s", sel.cx, sel.cy, unit );
        swprintf( parts[2], 64, L"%d × %d %s", pic ? pic->width : 0, pic ? pic->height : 0, unit );
        if (file_size)
        {
            if (file_size < 1024 * 1024)
                swprintf( parts[3], 64, L"%s %.1f %s", tr( L"Size:", L"Розмір:" ), file_size / 1024.0, tr( L"KB", L"КБ" ) );
            else
                swprintf( parts[3], 64, L"%s %.1f %s", tr( L"Size:", L"Розмір:" ), file_size / 1048576.0, tr( L"MB", L"МБ" ) );
            /* the decimal mark of the language */
            if (*tr( L".", L"," ) == ',') for (WCHAR *c = parts[3]; *c; c++) if (*c == '.') *c = ',';
        }
        for (int i = 0; i < 4; i++)
        {
            if (x + widths[i] > szones[S_ZOOMOUT].left - px( 8 )) break;
            draw_icon( &ssurf, icons[i], x, (rc.bottom - icon) / 2, icon, IS_NORMAL );
            SetRect( &r, x + px( 24 ), 0, x + widths[i], rc.bottom );
            draw_text( &ssurf, parts[i], -1, &r, get_font( F_UI ), th.text, DT_LEFT );
            x += widths[i];
        }
    }

    /* the zoom */
    for (int i = 0; i < S_COUNT; i++)
    {
        r = szones[i];
        if (status_hot == i && i != S_SLIDER) fill_round( &ssurf, &r, (float)px( 4 ), th.hover, 255 );
        switch (i)
        {
        case S_ZOOMOUT:
            draw_glyph( &ssurf, G_MINUS, (r.left + r.right - px( 14 )) / 2, (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.text );
            break;
        case S_ZOOMIN:
            draw_glyph( &ssurf, G_PLUS, (r.left + r.right - px( 14 )) / 2, (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.text );
            break;
        case S_FIT:
            draw_icon( &ssurf, "fit", (r.left + r.right - icon) / 2, (r.top + r.bottom - icon) / 2, icon, IS_NORMAL );
            break;
        case S_ZOOMTEXT:
            swprintf( text, ARRAY_SIZE(text), L"%d%%", (int)floor( canvas_zoom() * 100 + 0.5 ) );
            r.right -= px( 16 );
            draw_text( &ssurf, text, -1, &r, get_font( F_UI ), th.text, DT_RIGHT );
            draw_glyph( &ssurf, G_DOWN_SMALL, szones[i].right - px( 16 ), (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.dim );
            break;
        case S_SLIDER:
        {
            int cy = (r.top + r.bottom) / 2, l = r.left + px( 8 ), w = r.right - r.left - px( 16 );
            double t = zoom_to_t( canvas_zoom() );
            int tx = l + (int)(max( 0.0, min( 1.0, t ) ) * w);
            RECT track = { l, cy - px( 2 ), l + w, cy + px( 2 ) }, done = { l, cy - px( 2 ), tx, cy + px( 2 ) };
            fill_round( &ssurf, &track, (float)px( 2 ), th.dark ? RGB( 0x9a, 0x9a, 0x9a ) : RGB( 0x86, 0x86, 0x86 ), 255 );
            fill_round( &ssurf, &done, (float)px( 2 ), th.accent, 255 );
            fill_circle( &ssurf, tx + 0.5f, cy + 0.5f, (float)px( 10 ), th.dark ? RGB( 0x45, 0x45, 0x45 ) : RGB( 0xff, 0xff, 0xff ), 255 );
            ring( &ssurf, tx + 0.5f, cy + 0.5f, (float)px( 10 ), 1.0f, th.dark ? RGB( 0x55, 0x55, 0x55 ) : RGB( 0xd0, 0xd0, 0xd0 ), 255 );
            fill_circle( &ssurf, tx + 0.5f, cy + 0.5f, (float)px( status_hot == S_SLIDER || slider_drag ? 7 : 5 ), th.accent, 255 );
            break;
        }
        }
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, rc.right, rc.bottom, ssurf.dc, 0, 0, SRCCOPY );
}

static int status_hit( POINT pt )
{
    for (int i = 0; i < S_COUNT; i++)
        if (PtInRect( &szones[i], pt )) return i;
    return -1;
}

static void slider_set( POINT pt )
{
    RECT r = szones[S_SLIDER];
    int l = r.left + px( 8 ), w = r.right - r.left - px( 16 );
    double z = t_to_zoom( (double)(pt.x - l) / w );

    /* whole percents, and 100 % where it is near */
    z = floor( z * 100 + 0.5 ) / 100;
    if (fabs( z - 1.0 ) < 0.06) z = 1.0;
    canvas_set_zoom( z, NULL );
}

static void zoom_menu(void)
{
    static const int levels[] = { 800, 700, 600, 500, 400, 300, 200, 100, 75, 50, 25, 12 };
    struct menu_item items[ARRAY_SIZE(levels) + 2];
    WCHAR names[ARRAY_SIZE(levels)][16];
    RECT anchor = szones[S_ZOOMTEXT];
    int id, n = 0;

    memset( items, 0, sizeof(items) );
    for (int i = 0; i < ARRAY_SIZE(levels); i++)
    {
        swprintf( names[i], 16, L"%d%%", levels[i] == 12 ? 12 : levels[i] );
        if (levels[i] == 12) lstrcpyW( names[i], L"12,5%" );
        items[n].id = 3000 + i;
        items[n].text = names[i];
        n++;
    }
    items[n++].id = 0;
    items[n].id = IDM_ZOOM_FIT;
    items[n].text = tr( L"Fit to window", L"За розміром вікна" );
    items[n].keys = L"Ctrl+Shift+0";
    n++;
    MapWindowPoints( status, NULL, (POINT *)&anchor, 2 );
    anchor.top = anchor.bottom;   /* it opens up, over the bar */
    id = menu_run( frame, &anchor, items, n );
    if (id >= 3000 && id < 3000 + (int)ARRAY_SIZE(levels))
        canvas_set_zoom( levels[id - 3000] == 12 ? 0.125 : levels[id - 3000] / 100.0, NULL );
    else if (id) command( id );
}

static LRESULT CALLBACK status_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        status_paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int h = status_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        if (slider_drag) { slider_set( pt ); return 0; }
        if (h != status_hot)
        {
            status_hot = h;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        status_hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        switch (status_hit( pt ))
        {
        case S_ZOOMOUT: command( IDM_ZOOM_OUT ); break;
        case S_ZOOMIN: command( IDM_ZOOM_IN ); break;
        case S_FIT: command( IDM_ZOOM_FIT ); break;
        case S_ZOOMTEXT: zoom_menu(); break;
        case S_SLIDER:
            slider_drag = TRUE;
            SetCapture( hwnd );
            slider_set( pt );
            break;
        }
        return 0;
    case WM_LBUTTONUP:
        if (slider_drag)
        {
            slider_drag = FALSE;
            ReleaseCapture();
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND status_create( HWND parent )
{
    WNDCLASSW cls = { 0, status_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintStatus" };

    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    status = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, NULL, instance, NULL );
    return status;
}

void status_refresh(void)
{
    if (status) InvalidateRect( status, NULL, FALSE );
}

void status_theme_changed(void)
{
    status_refresh();
}

/**********************************************************************
 *          The text's bar, under the tools while text is typed
 */

struct text_style text_style = { L"Segoe UI", 12 };

static HWND textbar, face_box, size_box;
static struct surface tsurf;
static HBRUSH field_brush;
enum { TB_BOLD, TB_ITALIC, TB_UNDER, TB_STRIKE, TB_CLEAR, TB_OPAQUE, TB_COUNT };
static RECT tzones[TB_COUNT];
static int text_hot = -1;

static void textbar_layout( int width, int height )
{
    int x = px( 12 ) + px( 200 ) + px( 8 ) + px( 70 ) + px( 16 ), y = (height - px( 32 )) / 2;

    for (int i = 0; i < TB_COUNT; i++)
    {
        if (i == TB_CLEAR) x += px( 16 );
        SetRect( &tzones[i], x, y, x + px( 34 ), y + px( 32 ) );
        x += px( 36 );
    }
}

static BOOL text_checked( int i )
{
    switch (i)
    {
    case TB_BOLD: return text_style.bold;
    case TB_ITALIC: return text_style.italic;
    case TB_UNDER: return text_style.underline;
    case TB_STRIKE: return text_style.strike;
    case TB_CLEAR: return !text_style.opaque;
    default: return text_style.opaque;
    }
}

static void textbar_paint( HDC hdc )
{
    RECT rc, r;
    static const WCHAR *letters[4] = { L"B", L"I", L"U", L"S" };

    GetClientRect( textbar, &rc );
    if (!surface_size( &tsurf, rc.right, rc.bottom )) return;
    textbar_layout( rc.right, rc.bottom );
    fill( &tsurf, &rc, th.bar );
    r = rc;
    r.top = r.bottom - 1;
    fill( &tsurf, &r, th.bar_line );
    for (int i = 0; i < TB_COUNT; i++)
    {
        r = tzones[i];
        if (text_checked( i )) fill_round( &tsurf, &r, (float)px( 4 ), th.checked, 255 );
        else if (text_hot == i) fill_round( &tsurf, &r, (float)px( 4 ), th.hover, 255 );
        if (i < TB_CLEAR)
        {
            LOGFONTW lf = { 0 };
            HFONT font;
            lf.lfHeight = -px( 15 );
            lf.lfWeight = i == TB_BOLD ? FW_BOLD : FW_NORMAL;
            lf.lfItalic = i == TB_ITALIC;
            lf.lfUnderline = i == TB_UNDER;
            lf.lfStrikeOut = i == TB_STRIKE;
            lf.lfQuality = CLEARTYPE_QUALITY;
            lstrcpyW( lf.lfFaceName, L"Segoe UI" );
            font = CreateFontIndirectW( &lf );
            draw_text( &tsurf, letters[i], 1, &r, font, th.text, DT_CENTER );
            DeleteObject( font );
        }
        else draw_icon( &tsurf, i == TB_CLEAR ? "bg_clear" : "bg_opaque", (r.left + r.right - px( 20 )) / 2,
                        (r.top + r.bottom - px( 20 )) / 2, px( 20 ), IS_NORMAL );
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, rc.right, rc.bottom, tsurf.dc, 0, 0, SRCCOPY );
}

static int CALLBACK add_face( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lparam )
{
    if (lf->lfFaceName[0] == '@') return 1;
    if (SendMessageW( (HWND)lparam, CB_FINDSTRINGEXACT, -1, (LPARAM)lf->lfFaceName ) == CB_ERR)
        SendMessageW( (HWND)lparam, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName );
    return 1;
}

static void text_changed(void)
{
    canvas_style_changed();
    InvalidateRect( textbar, NULL, FALSE );
}

static LRESULT CALLBACK textbar_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        textbar_paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    {
        int y = (HIWORD( lparam ) - px( 24 )) / 2;
        SetWindowPos( face_box, NULL, px( 12 ), y, px( 200 ), px( 300 ), SWP_NOZORDER | SWP_NOACTIVATE );
        SetWindowPos( size_box, NULL, px( 12 ) + px( 208 ), y, px( 70 ), px( 300 ), SWP_NOZORDER | SWP_NOACTIVATE );
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int h = -1;
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        for (int i = 0; i < TB_COUNT; i++) if (PtInRect( &tzones[i], pt )) h = i;
        if (h != text_hot) { text_hot = h; InvalidateRect( hwnd, NULL, FALSE ); }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        text_hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        for (int i = 0; i < TB_COUNT; i++)
        {
            if (!PtInRect( &tzones[i], pt )) continue;
            switch (i)
            {
            case TB_BOLD: text_style.bold = !text_style.bold; break;
            case TB_ITALIC: text_style.italic = !text_style.italic; break;
            case TB_UNDER: text_style.underline = !text_style.underline; break;
            case TB_STRIKE: text_style.strike = !text_style.strike; break;
            case TB_CLEAR: text_style.opaque = FALSE; break;
            case TB_OPAQUE: text_style.opaque = TRUE; break;
            }
            text_changed();
        }
        return 0;
    case WM_COMMAND:
    {
        WCHAR buf[LF_FACESIZE];
        HWND from = (HWND)lparam;
        int code = HIWORD( wparam );
        if (code == CBN_SELCHANGE)
        {
            int i = (int)SendMessageW( from, CB_GETCURSEL, 0, 0 );
            if (i >= 0) SendMessageW( from, CB_GETLBTEXT, i, (LPARAM)buf );
            else buf[0] = 0;
        }
        else if (code == CBN_EDITCHANGE) GetWindowTextW( from, buf, ARRAY_SIZE(buf) );
        else return 0;
        if (!buf[0]) return 0;
        if (from == face_box) lstrcpynW( text_style.face, buf, LF_FACESIZE );
        else
        {
            int size = _wtoi( buf );
            if (size >= 1 && size <= 1000) text_style.size = size;
        }
        text_changed();
        if (code == CBN_SELCHANGE) PostMessageW( GetParent( hwnd ), WM_APP, 0, 0 );   /* the text gets the keys again */
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (!field_brush) field_brush = CreateSolidBrush( th.field );
        SetTextColor( (HDC)wparam, th.text );
        SetBkColor( (HDC)wparam, th.field );
        return (LRESULT)field_brush;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND textbar_create( HWND parent )
{
    WNDCLASSW cls = { 0, textbar_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintTextBar" };
    static const int sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    LOGFONTW lf = { 0 };
    HDC hdc;
    WCHAR buf[16];

    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    textbar = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, NULL, instance, NULL );
    face_box = CreateWindowExW( 0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN | CBS_SORT | CBS_AUTOHSCROLL,
                                0, 0, 10, 10, textbar, NULL, instance, NULL );
    size_box = CreateWindowExW( 0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                0, 0, 10, 10, textbar, NULL, instance, NULL );
    SendMessageW( face_box, WM_SETFONT, (WPARAM)get_font( F_UI ), 0 );
    SendMessageW( size_box, WM_SETFONT, (WPARAM)get_font( F_UI ), 0 );
    lf.lfCharSet = DEFAULT_CHARSET;
    hdc = GetDC( 0 );
    EnumFontFamiliesExW( hdc, &lf, add_face, (LPARAM)face_box, 0 );
    ReleaseDC( 0, hdc );
    if (SendMessageW( face_box, CB_FINDSTRINGEXACT, -1, (LPARAM)text_style.face ) == CB_ERR)
        lstrcpynW( text_style.face, ui_face(), LF_FACESIZE );
    SetWindowTextW( face_box, text_style.face );
    for (int i = 0; i < ARRAY_SIZE(sizes); i++)
    {
        swprintf( buf, ARRAY_SIZE(buf), L"%d", sizes[i] );
        SendMessageW( size_box, CB_ADDSTRING, 0, (LPARAM)buf );
    }
    swprintf( buf, ARRAY_SIZE(buf), L"%d", text_style.size );
    SetWindowTextW( size_box, buf );
    return textbar;
}

void textbar_show( BOOL show )
{
    if (!textbar || !!IsWindowVisible( textbar ) == !!show) return;
    ShowWindow( textbar, show ? SW_SHOWNA : SW_HIDE );
    layout_frame();
}

BOOL textbar_shown(void)
{
    return textbar && IsWindowVisible( textbar );
}

void textbar_theme_changed(void)
{
    if (field_brush) DeleteObject( field_brush );
    field_brush = NULL;
    if (textbar) InvalidateRect( textbar, NULL, TRUE );
}
