/*
 * Battery icon of the notification area: the flyout of Windows 10
 *
 * The flyout is BatteryFlyoutExperience's (ShellExperienceHost), a XAML page
 * (BatteryFlyoutExperience/MainPage.xaml in Windows.UI.PCShell.pri, with its
 * CommonStyles), drawn here with GDI over the shell's acrylic, 360 wide:
 * - BatteryIconPanel: the battery glyph at 64 (margin 8,2,6,2), the charge in
 *   HeaderTextBlockStyle (46, Light; margin 6,2) and the state in
 *   SecondaryBodyTextBlockStyle (15, BaseMedium, at most 145 wide, wrapped;
 *   margin 8,2,12,2 and 0,4,0,0);
 * - MultiBatteryPanel: with more than one battery, a line each (margin 12,0,
 *   items 0,2): "Батарея 1: 50%, використовується";
 * - PowerOverlaySliderPanel, over the Balanced scheme: the power slider, 336
 *   wide at margin 12,18,12,0, its header "Режим живлення (з живленням від
 *   акумулятора): Рекомендовано", ticks under it every place; under it a
 *   battery and a lightning bolt at 16 (margins 12,0,0,2 and 0,0,12,2) and
 *   "Найкращий час роботи акумулятора" / "Найвища продуктивність" in Caption
 *   (margins 12,0,0,30 and 0,0,12,30);
 * - the link "Параметри живлення від акумулятора" (margin 12,8,12,6), to
 *   Control Panel's Power Options;
 * - without the slider, on battery: the quick action Battery saver (85 x 64).
 * Slider: Windows 10's template, a 2 high track (BaseMediumLow) in a 32 high
 * control, the part up to the thumb and the 8 x 24 thumb in the accent
 * colour, the thumb ChromeAltLow under the pointer and ChromeHigh pressed,
 * 1 x 4 ticks 4 under the track.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "batmeter.h"
#include "shellapi.h"
#include "dwmapi.h"
#include "winreg.h"
#include "wine/arctic_dwm.h"
#include "wine/arctic_fluent.h"

#define FLYOUT_WIDTH   px(360)
#define ICON_SIZE      px(64)
#define SLIDER_LEFT    px(12)
#define SLIDER_WIDTH   px(336)
#define SLIDER_HEIGHT  px(32)
#define THUMB_WIDTH    px(8)
#define THUMB_HEIGHT   px(24)
#define TILE_WIDTH     px(85)
#define TILE_HEIGHT    px(64)

#define TIMER_POLL     1

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum hit { HIT_NONE, HIT_SLIDER, HIT_LINK, HIT_TILE };

static HWND flyout, tray_window;
static UINT tray_icon;
static struct battery_view view;
static enum hit hot = HIT_NONE, pressed = HIT_NONE;
static BOOL dragging;
static HFONT font_body, font_header, font_caption;
static int line15, line12, line46;
static DWORD hidden_at;
static int anchor_x, anchor_y;
static BOOL anchor_above;

/* the layout, worked out from what there is to show */
static struct
{
    int row_height, percent_x, label_x, label_width;
    int list_top, list_height;
    int header_top, header_height, slider_top, ends_top, labels_top, panel_bottom;
    int link_top, link_bottom, tile_top, height;
} layout;

static int text_height( const WCHAR *text, HFONT font, int width )
{
    HDC hdc = GetDC( NULL );
    HGDIOBJ old = SelectObject( hdc, font );
    RECT rect = { 0, 0, width, 0 };

    DrawTextW( hdc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( hdc, old );
    ReleaseDC( NULL, hdc );
    return rect.bottom;
}

static int text_width( const WCHAR *text, HFONT font )
{
    HDC hdc = GetDC( NULL );
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    ReleaseDC( NULL, hdc );
    return size.cx;
}

static const WCHAR *percent_text(void)
{
    static WCHAR text[16];

    if (view.status.percent == ARCTIC_UNKNOWN) return L"";
    lstrcpynW( text, format_string( IDS_PERCENT, view.status.percent ), ARRAY_SIZE(text) );
    return text;
}

static const WCHAR *slider_header(void)
{
    static WCHAR text[256];

    swprintf( text, ARRAY_SIZE(text), L"%s%s", load_string( view.status.ac_online ? IDS_MODE_AC : IDS_MODE_DC ),
              battery_mode_name( &view, view.position ) );
    return text;
}

static BOOL show_tile(void)
{
    return !view.slider && !view.status.ac_online;
}

static BOOL style11;
static void lay_out11(void);

static void lay_out(void)
{
    WCHAR label[256];
    int y;

    if (style11)
    {
        lay_out11();
        return;
    }
    battery_state_label( &view, label, ARRAY_SIZE(label) );

    /* the icon, the charge and the state side by side */
    layout.percent_x = px( 8 ) + ICON_SIZE + px( 6 ) + px( 6 );
    layout.label_x = layout.percent_x + text_width( percent_text(), font_header ) + px( 6 ) + px( 8 );
    layout.label_width = min( px( 145 ), FLYOUT_WIDTH - px( 12 ) - layout.label_x );
    layout.row_height = max( ICON_SIZE + px( 4 ), line46 + px( 4 ) );
    layout.row_height = max( layout.row_height, text_height( label, font_body, layout.label_width ) + px( 8 ) );
    y = layout.row_height;

    /* a line a battery, with more than one */
    layout.list_top = y;
    layout.list_height = view.status.battery_count > 1 ? view.status.battery_count * (line15 + px( 4 )) : 0;
    y += layout.list_height;

    if (view.slider)
    {
        layout.header_top = y + px( 18 );
        layout.header_height = text_height( slider_header(), font_body, SLIDER_WIDTH );
        layout.slider_top = layout.header_top + layout.header_height + px( 4 );
        layout.ends_top = layout.slider_top + SLIDER_HEIGHT;
        layout.labels_top = layout.ends_top + px( 16 ) + px( 2 );
        layout.panel_bottom = layout.labels_top + max( text_height( load_string( IDS_BEST_BATTERY ), font_caption, px( 160 ) ),
                                                       line12 ) + px( 30 );
        y = layout.panel_bottom;
    }
    /* HyperlinkButton: padding 0,5,0,6 inside the margin 12,8,12,6 */
    layout.link_top = y + px( 8 );
    layout.link_bottom = layout.link_top + px( 5 ) + line15 + px( 6 );
    y = layout.link_bottom + px( 6 );
    if (show_tile())
    {
        layout.tile_top = y + px( 4 );
        y = layout.tile_top + TILE_HEIGHT + px( 4 );
    }
    layout.height = y;
}

/* Windows 11's: 16 around, the battery at 48 beside its charge at 28
 * (SemiBold) and its state, the power slider of WinUI, battery saver as a
 * quick action (96 x 48, its name under it), and at the bottom a darker
 * strip that leads to the battery's settings, as Quick Settings' footer */
#define PAD11          px(16)
#define GLYPH11        px(48)
#define SLIDER11       px(32)
#define TILE11_WIDTH   px(96)
#define TILE11_HEIGHT  px(48)
#define FOOTER11       px(48)

static HFONT font_title, font_body14;
static int line28, line14;

static struct
{
    int row_top, row_height, text_x;
    int list_top, list_height;
    int header_top, header_height, slider_top, labels_top, labels_height;
    int tile_top, footer_top, height;
} l11;

static void lay_out11(void)
{
    WCHAR label[256];
    int y = PAD11, width = FLYOUT_WIDTH - 2 * PAD11;

    battery_state_label( &view, label, ARRAY_SIZE(label) );
    l11.row_top = y;
    l11.text_x = PAD11 + GLYPH11 + px( 16 );
    l11.row_height = max( GLYPH11, line28 + (label[0] ? text_height( label, font_body14, FLYOUT_WIDTH - PAD11 - l11.text_x ) : 0) );
    y += l11.row_height;

    l11.list_top = y + px( 4 );
    l11.list_height = view.status.battery_count > 1 ? view.status.battery_count * (line14 + px( 4 )) : 0;
    y = l11.list_top + l11.list_height;

    if (view.slider)
    {
        l11.header_top = y + px( 16 );
        l11.header_height = text_height( slider_header(), font_body14, width );
        l11.slider_top = l11.header_top + l11.header_height + px( 4 );
        l11.labels_top = l11.slider_top + SLIDER11 + px( 4 );
        l11.labels_height = max( text_height( load_string( IDS_BEST_BATTERY ), font_caption, width / 2 - px( 8 ) ),
                                 text_height( load_string( IDS_BEST_PERFORMANCE ), font_caption, width / 2 - px( 8 ) ) );
        y = l11.labels_top + l11.labels_height;
    }
    if (show_tile())
    {
        l11.tile_top = y + px( 16 );
        y = l11.tile_top + TILE11_HEIGHT + px( 6 ) + 2 * line12;
    }
    l11.footer_top = y + PAD11;
    l11.height = l11.footer_top + FOOTER11;
    layout.height = l11.height;
}

static void place(void)
{
    SetWindowPos( flyout, HWND_TOPMOST, anchor_x, anchor_above ? anchor_y - layout.height : anchor_y, FLYOUT_WIDTH,
                  layout.height, SWP_NOACTIVATE );
}

/* over the icon, on the taskbar's edge, inside the work area; Windows 11's
 * at the right end of the taskbar, 12 off it */
static void anchor(void)
{
    NOTIFYICONIDENTIFIER id = { sizeof(id), tray_window, tray_icon };
    MONITORINFO info = { sizeof(info) };
    RECT icon;
    POINT pt;

    if (FAILED(Shell_NotifyIconGetRect( &id, &icon )))
    {
        GetCursorPos( &pt );
        SetRect( &icon, pt.x, pt.y, pt.x + 1, pt.y + 1 );
    }
    if (style11)
    {
        fluent_anchor( &icon, FLYOUT_WIDTH, &anchor_x, &anchor_y, &anchor_above );
        return;
    }
    GetMonitorInfoW( MonitorFromRect( &icon, MONITOR_DEFAULTTONEAREST ), &info );
    anchor_x = (icon.left + icon.right - FLYOUT_WIDTH) / 2;
    anchor_x = max( info.rcWork.left, min( anchor_x, info.rcWork.right - FLYOUT_WIDTH ) );
    anchor_above = (icon.top + icon.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2;
    anchor_y = anchor_above ? info.rcWork.bottom : info.rcWork.top;
}

/**********************************************************************
 *          Painting
 */

/* white text at an opacity: over the acrylic, GDI's grey is white seen through */
static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, flags | DT_NOPREFIX );
    SelectObject( hdc, old );
}

#define WHITE   RGB( 0xff, 0xff, 0xff )
#define INK     ARCTIC_INK( shell_light )   /* text and glyphs; on the accent always white */
#define MEDIUM  (shell_light ? RGB( 0x66, 0x66, 0x66 ) : RGB( 0x99, 0x99, 0x99 ))   /* BaseMedium: 60% */

static void paint_battery( HDC hdc )
{
    WCHAR label[256];
    RECT rect;

    draw_glyph( hdc, battery_view_glyph( &view ), px( 8 ), (layout.row_height - ICON_SIZE) / 2, ICON_SIZE, INK, 255 );

    SetRect( &rect, layout.percent_x, 0, layout.label_x, layout.row_height );
    draw_text( hdc, percent_text(), &rect, font_header, INK, DT_SINGLELINE | DT_VCENTER | DT_LEFT );

    battery_state_label( &view, label, ARRAY_SIZE(label) );
    if (label[0])
    {
        int height = text_height( label, font_body, layout.label_width );
        int top = (layout.row_height - height) / 2 + px( 2 );

        SetRect( &rect, layout.label_x, top, layout.label_x + layout.label_width, top + height );
        draw_text( hdc, label, &rect, font_body, MEDIUM, DT_WORDBREAK | DT_LEFT );
    }
}

static void paint_list( HDC hdc )
{
    const struct arctic_power_status *s = &view.status;

    if (s->battery_count < 2) return;
    for (UINT i = 0; i < s->battery_count; i++)
    {
        const struct arctic_battery *bat = &s->batteries[i];
        int top = layout.list_top + i * (line15 + px( 4 )) + px( 2 );
        RECT rect = { px( 12 ), top, FLYOUT_WIDTH - px( 12 ), top + line15 };
        const WCHAR *text;

        if (bat->state == ARCTIC_BATTERY_FULL || bat->percent >= 100) text = format_string( IDS_BATTERY_FULL, i + 1, bat->percent );
        else if (bat->state == ARCTIC_BATTERY_DISCHARGING) text = format_string( IDS_BATTERY_IN_USE, i + 1, bat->percent );
        else text = format_string( IDS_BATTERY_PERCENT, i + 1, bat->percent );
        draw_text( hdc, text, &rect, font_body, MEDIUM, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS );
    }
}

static int thumb_left( int position )
{
    return SLIDER_LEFT + (SLIDER_WIDTH - THUMB_WIDTH) * position / max( view.positions - 1, 1 );
}

static void paint_slider( HDC hdc )
{
    int track_y = layout.slider_top + px( 15 ), left = thumb_left( view.position );
    BOOL over = hot == HIT_SLIDER || dragging, down = pressed == HIT_SLIDER || dragging;
    RECT rect;

    if (!view.slider) return;
    SetRect( &rect, SLIDER_LEFT, layout.header_top, SLIDER_LEFT + SLIDER_WIDTH, layout.header_top + layout.header_height );
    draw_text( hdc, slider_header(), &rect, font_body, INK, DT_WORDBREAK | DT_LEFT );

    /* the track, the part up to the thumb */
    SetRect( &rect, SLIDER_LEFT, track_y, SLIDER_LEFT + SLIDER_WIDTH, track_y + px( 2 ) );
    fill_alpha( hdc, &rect, INK, 0x66 );
    SetRect( &rect, SLIDER_LEFT, track_y, left + THUMB_WIDTH / 2, track_y + px( 2 ) );
    fill_alpha( hdc, &rect, ACCENT_COLOR, 255 );

    /* the ticks, a place each, 4 under the track */
    for (int i = 0; i < view.positions; i++)
    {
        int x = thumb_left( i ) + THUMB_WIDTH / 2;
        SetRect( &rect, x, track_y + px( 2 ) + px( 4 ), x + max( px( 1 ), 1 ), track_y + px( 2 ) + px( 8 ) );
        fill_alpha( hdc, &rect, INK, 0x66 );
    }

    /* the thumb */
    SetRect( &rect, left, track_y + px( 1 ) - THUMB_HEIGHT / 2, left + THUMB_WIDTH, track_y + px( 1 ) + THUMB_HEIGHT / 2 );
    fill_round_rect( hdc, &rect, px( 4 ), down ? RGB( 0x76, 0x76, 0x76 ) : over ? RGB( 0xf2, 0xf2, 0xf2 ) : ACCENT_COLOR, 255 );

    /* the ends: a battery and a lightning bolt, then their names */
    draw_glyph( hdc, 0xe83f, SLIDER_LEFT, layout.ends_top, px( 16 ), MEDIUM, 255 );
    draw_glyph( hdc, 0xe945, SLIDER_LEFT + SLIDER_WIDTH - px( 16 ), layout.ends_top, px( 16 ), MEDIUM, 255 );
    SetRect( &rect, SLIDER_LEFT, layout.labels_top, SLIDER_LEFT + SLIDER_WIDTH / 2, layout.panel_bottom - px( 30 ) );
    draw_text( hdc, load_string( IDS_BEST_BATTERY ), &rect, font_caption, MEDIUM, DT_WORDBREAK | DT_LEFT );
    SetRect( &rect, SLIDER_LEFT + SLIDER_WIDTH / 2, layout.labels_top, SLIDER_LEFT + SLIDER_WIDTH, layout.panel_bottom - px( 30 ) );
    draw_text( hdc, load_string( IDS_BEST_PERFORMANCE ), &rect, font_caption, MEDIUM, DT_WORDBREAK | DT_RIGHT );
}

static void paint_link( HDC hdc )
{
    const WCHAR *text = load_string( IDS_SETTINGS_LINK );
    COLORREF color = pressed == HIT_LINK ? MEDIUM : hot == HIT_LINK ? LINK_HOT : LINK_COLOR;
    RECT rect = { px( 12 ), layout.link_top + px( 5 ), FLYOUT_WIDTH - px( 12 ), layout.link_top + px( 5 ) + line15 };

    draw_text( hdc, text, &rect, font_body, color, DT_SINGLELINE | DT_LEFT );
    if (hot == HIT_LINK)
    {
        RECT underline = { rect.left, rect.top + line15 - px( 2 ), rect.left + text_width( text, font_body ),
                           rect.top + line15 - px( 1 ) };
        fill_alpha( hdc, &underline, color, 255 );
    }
}

static void paint_tile( HDC hdc, UINT32 *bits, int width, int height )
{
    RECT rect = { px( 4 ), layout.tile_top, px( 4 ) + TILE_WIDTH, layout.tile_top + TILE_HEIGHT }, text;
    const WCHAR *name = load_string( IDS_BATTERY_SAVER );
    int lines;
    BOOL on = view.saver && pressed != HIT_TILE;
    COLORREF fore = on ? WHITE : INK;

    if (!show_tile()) return;
    /* ToggleButtonRevealStyle: BaseLow off, the accent on, BaseMediumLow pressed */
    if (pressed == HIT_TILE) fill_alpha( hdc, &rect, INK, 0x66 );
    else if (view.saver) fill_alpha( hdc, &rect, ACCENT_COLOR, 255 );
    else fill_alpha( hdc, &rect, INK, 0x33 );
    if (hot == HIT_TILE)
    {
        RECT side;
        SetRect( &side, rect.left, rect.top, rect.right, rect.top + px( 1 ) ); fill_alpha( hdc, &side, fore, 0x99 );
        SetRect( &side, rect.left, rect.bottom - px( 1 ), rect.right, rect.bottom ); fill_alpha( hdc, &side, fore, 0x99 );
        SetRect( &side, rect.left, rect.top, rect.left + px( 1 ), rect.bottom ); fill_alpha( hdc, &side, fore, 0x99 );
        SetRect( &side, rect.right - px( 1 ), rect.top, rect.right, rect.bottom ); fill_alpha( hdc, &side, fore, 0x99 );
    }
    /* padding 3,7,0,3 inside the 1 border: the 16 glyph top left, the title bottom left */
    draw_glyph( hdc, battery_glyph( GLYPH_SAVER, view.status.percent ), rect.left + px( 4 ), rect.top + px( 8 ), px( 16 ),
                fore, 255 );
    lines = min( text_height( name, font_caption, TILE_WIDTH - px( 5 ) ), 2 * line12 );
    SetRect( &text, rect.left + px( 4 ), rect.bottom - px( 4 ) - lines, rect.right - px( 1 ), rect.bottom - px( 4 ) );
    draw_text( hdc, name, &text, font_caption, fore, DT_WORDBREAK | DT_LEFT );
    /* the accent is opaque: white text on it must not be keyed by brightness */
    if (view.saver && pressed != HIT_TILE)
        for (int y = max( (int)rect.top, 0 ); y < min( (int)rect.bottom, height ); y++)
            for (int x = max( (int)rect.left, 0 ); x < min( (int)rect.right, width ); x++)
                bits[y * width + x] |= 0xff000000;
}

static enum fluent_state state11( enum hit what )
{
    if (pressed == what && hot == what) return FLUENT_PRESSED;
    if (hot == what) return FLUENT_HOT;
    return FLUENT_REST;
}

static void slider11_extent( int *left, int *right )
{
    *left = PAD11 - px( 10 );   /* the thumb's 10 inside the track's ends */
    *right = FLYOUT_WIDTH - PAD11 + px( 10 );
}

static void paint11( HDC hdc, UINT32 *bits, int width, int height )
{
    WCHAR label[256];
    RECT rect;
    int x;

    /* the battery, its charge, its state */
    draw_glyph( hdc, battery_view_glyph( &view ), PAD11, l11.row_top + (l11.row_height - GLYPH11) / 2, GLYPH11, INK, 255 );
    battery_state_label( &view, label, ARRAY_SIZE(label) );
    {
        int text_height_ = line28 + (label[0] ? text_height( label, font_body14, FLYOUT_WIDTH - PAD11 - l11.text_x ) : 0);
        int top = l11.row_top + (l11.row_height - text_height_) / 2;

        SetRect( &rect, l11.text_x, top, FLYOUT_WIDTH - PAD11, top + line28 );
        draw_text( hdc, percent_text(), &rect, font_title, INK, DT_SINGLELINE | DT_LEFT );
        if (label[0])
        {
            SetRect( &rect, l11.text_x, top + line28, FLYOUT_WIDTH - PAD11, l11.row_top + l11.row_height + line14 );
            draw_text( hdc, label, &rect, font_body14, MEDIUM, DT_WORDBREAK | DT_LEFT );
        }
    }

    /* more than one battery: a line each */
    for (UINT i = 0; view.status.battery_count > 1 && i < view.status.battery_count; i++)
    {
        const struct arctic_battery *bat = &view.status.batteries[i];
        int top = l11.list_top + i * (line14 + px( 4 ));
        const WCHAR *text;

        if (bat->state == ARCTIC_BATTERY_FULL || bat->percent >= 100) text = format_string( IDS_BATTERY_FULL, i + 1, bat->percent );
        else if (bat->state == ARCTIC_BATTERY_DISCHARGING) text = format_string( IDS_BATTERY_IN_USE, i + 1, bat->percent );
        else text = format_string( IDS_BATTERY_PERCENT, i + 1, bat->percent );
        SetRect( &rect, PAD11, top, FLYOUT_WIDTH - PAD11, top + line14 );
        draw_text( hdc, text, &rect, font_body14, MEDIUM, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS );
    }

    /* the power mode: its name, the slider with a tick a place, the ends' names */
    if (view.slider)
    {
        int left, right, cy = l11.slider_top + SLIDER11 / 2;
        float unit = fluent_unit();

        SetRect( &rect, PAD11, l11.header_top, FLYOUT_WIDTH - PAD11, l11.header_top + l11.header_height );
        draw_text( hdc, slider_header(), &rect, font_body14, INK, DT_WORDBREAK | DT_LEFT );
        slider11_extent( &left, &right );
        for (int i = 0; i < view.positions; i++)
        {
            /* TickBar: 1 wide, 4 high, 4 under the track */
            x = fluent_slider_x( left, right, unit, (float)i / max( view.positions - 1, 1 ) );
            SetRect( &rect, x, cy + px( 2 ) + px( 4 ), x + max( px( 1 ), 1 ), cy + px( 2 ) + px( 8 ) );
            fill_alpha( hdc, &rect, INK, 0x8b );
        }
        fluent_slider( hdc, left, right, cy, unit, (float)view.position / max( view.positions - 1, 1 ),
                       dragging ? FLUENT_PRESSED : hot == HIT_SLIDER ? FLUENT_HOT : FLUENT_REST, shell_light, 255 );
        SetRect( &rect, PAD11, l11.labels_top, FLYOUT_WIDTH / 2 - px( 8 ), l11.labels_top + l11.labels_height );
        draw_text( hdc, load_string( IDS_BEST_BATTERY ), &rect, font_caption, MEDIUM, DT_WORDBREAK | DT_LEFT );
        SetRect( &rect, FLYOUT_WIDTH / 2 + px( 8 ), l11.labels_top, FLYOUT_WIDTH - PAD11, l11.labels_top + l11.labels_height );
        draw_text( hdc, load_string( IDS_BEST_PERFORMANCE ), &rect, font_caption, MEDIUM, DT_WORDBREAK | DT_RIGHT );
    }

    /* battery saver, a quick action */
    if (show_tile())
    {
        RECT tile = { PAD11, l11.tile_top, PAD11 + TILE11_WIDTH, l11.tile_top + TILE11_HEIGHT };
        COLORREF fore = fluent_toggle( hdc, &tile, view.saver, state11( HIT_TILE ), shell_light, bits, width, height );
        int glyph = px( 20 );

        draw_glyph( hdc, battery_glyph( GLYPH_SAVER, view.status.percent ), (tile.left + tile.right - glyph) / 2,
                    (tile.top + tile.bottom - glyph) / 2, glyph, fore, 255 );
        SetRect( &rect, tile.left - px( 8 ), tile.bottom + px( 6 ), tile.right + px( 8 ), tile.bottom + px( 6 ) + 2 * line12 );
        draw_text( hdc, load_string( IDS_BATTERY_SAVER ), &rect, font_caption, INK, DT_WORDBREAK | DT_CENTER );
    }

    /* the footer: darker, the way to the battery's settings */
    SetRect( &rect, 0, l11.footer_top, FLYOUT_WIDTH, l11.height );
    fill_alpha( hdc, &rect, shell_light ? RGB( 0, 0, 0 ) : RGB( 0, 0, 0 ), shell_light ? 0x08 : 0x40 );
    if (hot == HIT_LINK) fill_alpha( hdc, &rect, INK, fluent_subtle_alpha( state11( HIT_LINK ), shell_light ) );
    SetRect( &rect, PAD11, l11.footer_top, FLYOUT_WIDTH - PAD11 - px( 24 ), l11.height );
    draw_text( hdc, load_string( IDS_SETTINGS_LINK ), &rect, font_body14, INK, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS );
    fluent_chevron( hdc, FLYOUT_WIDTH - PAD11 - px( 6 ), (l11.footer_top + l11.height) / 2, px( 12 ), FLUENT_RIGHT, INK, 255 );
}

static enum hit hit_test11( POINT pt )
{
    if (view.slider && pt.x >= PAD11 - px( 10 ) && pt.x < FLYOUT_WIDTH - PAD11 + px( 10 ) &&
        pt.y >= l11.slider_top && pt.y < l11.slider_top + SLIDER11)
        return HIT_SLIDER;
    if (show_tile() && pt.x >= PAD11 && pt.x < PAD11 + TILE11_WIDTH && pt.y >= l11.tile_top &&
        pt.y < l11.tile_top + TILE11_HEIGHT)
        return HIT_TILE;
    if (pt.y >= l11.footer_top && pt.y < l11.height) return HIT_LINK;
    return HIT_NONE;
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( hwnd, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    UINT32 *bits;

    GetClientRect( hwnd, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    mem = CreateCompatibleDC( hdc );
    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        /* black is the acrylic seen through */
        arctic_fill_back( mem, &client, shell_light );
        SetBkMode( mem, TRANSPARENT );
        if (style11) paint11( mem, bits, client.right, client.bottom );
        else
        {
            paint_battery( mem );
            paint_list( mem );
            paint_slider( mem );
            paint_link( mem );
            paint_tile( mem, bits, client.right, client.bottom );
        }
        BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    EndPaint( hwnd, &ps );
}

/**********************************************************************
 *          Input
 */

static enum hit hit_test( POINT pt )
{
    if (style11) return hit_test11( pt );
    if (view.slider && pt.x >= SLIDER_LEFT - px( 4 ) && pt.x < SLIDER_LEFT + SLIDER_WIDTH + px( 4 ) &&
        pt.y >= layout.slider_top && pt.y < layout.slider_top + SLIDER_HEIGHT)
        return HIT_SLIDER;
    if (pt.y >= layout.link_top && pt.y < layout.link_bottom && pt.x >= px( 12 ) &&
        pt.x < px( 12 ) + text_width( load_string( IDS_SETTINGS_LINK ), font_body ))
        return HIT_LINK;
    if (show_tile() && pt.x >= px( 4 ) && pt.x < px( 4 ) + TILE_WIDTH && pt.y >= layout.tile_top &&
        pt.y < layout.tile_top + TILE_HEIGHT)
        return HIT_TILE;
    return HIT_NONE;
}

static void reload(void)
{
    int height = layout.height;

    battery_read( &view );
    lay_out();
    if (layout.height != height) place();
    InvalidateRect( flyout, NULL, FALSE );
}

static void set_position( int position )
{
    position = max( 0, min( position, view.positions - 1 ) );
    if (position == view.position) return;
    battery_set_position( &view, position );
    lay_out();
    place();
    InvalidateRect( flyout, NULL, FALSE );
    tray_update();
}

/* the slider snaps to its places */
static void set_position_at( int x )
{
    int steps = max( view.positions - 1, 1 ), span = SLIDER_WIDTH - THUMB_WIDTH;
    int at = x - SLIDER_LEFT - THUMB_WIDTH / 2;

    if (style11)
    {
        int left, right;

        slider11_extent( &left, &right );
        set_position( (int)(fluent_slider_value( left, right, fluent_unit(), x ) * steps + 0.5f) );
        return;
    }
    set_position( (at * steps + span / 2) / span );
}

static void on_click( enum hit hit )
{
    switch (hit)
    {
    case HIT_LINK:
        /* the battery's page: the icon's own, as the user asked */
        flyout_hide();
        open_battery_panel();
        break;
    case HIT_TILE:
        battery_set_saver( !view.saver );
        reload();
        tray_update();
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK flyout_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_TIMER:
        if (wparam == TIMER_POLL && !dragging) reload();
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        enum hit hit;

        if (dragging)
        {
            set_position_at( pt.x );
            return 0;
        }
        if ((hit = hit_test( pt )) != hot)
        {
            hot = hit;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        if (!dragging)
        {
            hot = pressed = HIT_NONE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

        pressed = hit_test( pt );
        if (pressed == HIT_SLIDER)
        {
            /* a press on the track moves the thumb there, then it follows */
            dragging = TRUE;
            SetCapture( hwnd );
            set_position_at( pt.x );
        }
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

        if (dragging)
        {
            dragging = FALSE;
            ReleaseCapture();
        }
        if (hit_test( pt ) == pressed) on_click( pressed );
        pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_CAPTURECHANGED:
        dragging = FALSE;
        return 0;

    case WM_MOUSEWHEEL:
        if (view.slider) set_position( view.position + (GET_WHEEL_DELTA_WPARAM( wparam ) > 0 ? 1 : -1) );
        return 0;

    case WM_KEYDOWN:
        switch (wparam)
        {
        case VK_ESCAPE: flyout_hide(); break;
        case VK_LEFT:
        case VK_DOWN:   if (view.slider) set_position( view.position - 1 ); break;
        case VK_RIGHT:
        case VK_UP:     if (view.slider) set_position( view.position + 1 ); break;
        case VK_HOME:   if (view.slider) set_position( 0 ); break;
        case VK_END:    if (view.slider) set_position( view.positions - 1 ); break;
        }
        return 0;

    case WM_ACTIVATE:
        /* a click anywhere else closes it, as in Windows */
        if (LOWORD( wparam ) == WA_INACTIVE) flyout_hide();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static int font_line( HFONT font )
{
    HDC hdc = GetDC( NULL );
    HGDIOBJ old = SelectObject( hdc, font );
    TEXTMETRICW metrics;

    GetTextMetricsW( hdc, &metrics );
    SelectObject( hdc, old );
    ReleaseDC( NULL, hdc );
    return metrics.tmHeight;
}

static BOOL create_flyout(void)
{
    struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, ACRYLIC_TINT, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
    WNDCLASSW cls = { 0 };
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP, corner = ARCTIC_FLYOUT_CORNER;

    if (flyout) return TRUE;
    cls.lpfnWndProc = flyout_proc;
    cls.hInstance = batmeter_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticBatteryFlyout";
    RegisterClassW( &cls );
    flyout = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                              0, 0, FLYOUT_WIDTH, px( 200 ), NULL, NULL, batmeter_instance, NULL );
    if (!flyout) return FALSE;
    SetWindowCompositionAttribute( flyout, &attr );
    DwmSetWindowAttribute( flyout, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( flyout, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_body = shell_font( px( 15 ), FW_NORMAL );
    font_header = shell_font( px( 46 ), FW_LIGHT );
    font_caption = shell_font( px( 12 ), FW_NORMAL );
    line15 = font_line( font_body );
    line12 = font_line( font_caption );
    line46 = font_line( font_header );
    /* Windows 11's TitleTextBlockStyle and BodyTextBlockStyle */
    font_title = shell_font( px( 28 ), FW_SEMIBOLD );
    font_body14 = shell_font( px( 14 ), FW_NORMAL );
    line28 = font_line( font_title );
    line14 = font_line( font_body14 );
    return TRUE;
}

/**********************************************************************
 *          Showing it
 */

BOOL flyout_visible(void)
{
    return flyout && IsWindowVisible( flyout );
}

void flyout_hide(void)
{
    if (!flyout_visible()) return;
    KillTimer( flyout, TIMER_POLL );
    if (dragging) ReleaseCapture();
    dragging = FALSE;
    ShowWindow( flyout, SW_HIDE );
    hidden_at = GetTickCount();
}

void flyout_toggle( HWND tray, UINT icon_id )
{
    if (!create_flyout()) return;
    if (flyout_visible())
    {
        flyout_hide();
        return;
    }
    /* the click on the icon that took the focus away and closed it */
    if (GetTickCount() - hidden_at < 300) return;

    tray_window = tray;
    tray_icon = icon_id;
    style11 = arctic_style11();
    dragging = FALSE;
    hot = pressed = HIT_NONE;
    battery_read( &view );
    lay_out();
    anchor();
    place();
    InvalidateRect( flyout, NULL, FALSE );
    shell_look( flyout );
    ShowWindow( flyout, SW_SHOW );
    SetForegroundWindow( flyout );
    SetFocus( flyout );
    SetTimer( flyout, TIMER_POLL, 1000, NULL );
}

/* the batteries changed (the power policy's broadcast, the tray's timer) */
void flyout_refresh(void)
{
    if (!flyout_visible() || dragging) return;
    reload();
}
