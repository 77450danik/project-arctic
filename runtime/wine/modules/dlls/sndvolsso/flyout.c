/*
 * Volume icon of the notification area: the flyout of Windows 10
 *
 * The flyout is MtcUvc's (ShellExperienceHost), a XAML page
 * (MtcUvc/View/MtcUvcView.xaml with its Templates.xaml and Styles.xaml),
 * drawn here with GDI over the shell's acrylic. 360 wide: on top the output
 * in use, 44 high (SpeakerListItemHeight); when there is more than one, it
 * is a toggle with a chevron that opens the list of outputs under it, 44 an
 * output and at most 157 high (SpeakerListScrollViewerMaxHeight), in a
 * quarter of a second with an exponential ease while the volume row fades to
 * 0.15 (Storyboard_ExpandOutputList). Under it the volume row, 56 high
 * (SliderControlHeight): the mute toggle in a 64 wide column (Margin
 * 12,0,8,8, its glyph 24), the slider 232 wide (a 2 high track, the part up
 * to the thumb in the accent colour, an 8 x 24 thumb rounded by 4 that turns
 * white under the pointer), and the level in a 64 wide column at 24 SemiLight.
 * The dark theme's colours: hover #33FFFFFF over the toggle, #19FFFFFF over
 * an output and #33FFFFFF pressed, the chosen output on the accent colour,
 * the track #66FFFFFF, the mute glyph #CCFFFFFF and its unlit waves at 24%.
 *
 * Choosing an output makes it the default; the slider, the wheel and the
 * arrow keys set the volume, 1 a key, 2 a notch of the wheel, 10 a page.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>

#include "sndvolsso.h"
#include "shellapi.h"
#include "dwmapi.h"
#include "winreg.h"
#include "wine/arctic_dwm.h"
#include "wine/arctic_fluent.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

#define FLYOUT_WIDTH   px(360)
#define ROW_HEIGHT     px(44)    /* SpeakerListItemHeight */
#define LIST_MAX       px(157)    /* SpeakerListScrollViewerMaxHeight */
#define VOLUME_ROW     px(56)    /* SliderControlHeight */
#define SIDE_COLUMN    px(64)    /* the mute toggle's and the level's */
#define SLIDER_WIDTH   px(232)
#define THUMB_WIDTH    px(8)
#define THUMB_HEIGHT   px(24)
#define GLYPH_SIZE     px(24)
#define DIMMED         0.15f  /* VolumeControl_Opacity_Disabled */
#define EXPAND_MS      250

#define TIMER_POLL     1
#define TIMER_ANIMATE  2

/* Windows 11's (Quick Settings' volume row and its list of outputs): 12
 * around, the row 48 high with 40 square subtle buttons either end, the
 * outputs 40 high, rounded, the one in use ticked in the accent */
#define PAD11          px(12)
#define ROW11          px(48)
#define BUTTON11       px(40)
#define ITEM11         px(40)
#define LEVEL11        px(36)
#define LIST_MAX11     px(240)

static BOOL style11;

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum hit { HIT_NONE, HIT_MEDIA, HIT_HEADER, HIT_ITEM, HIT_MUTE, HIT_SLIDER };

static HWND flyout, tray_window;
static UINT tray_icon;
static struct endpoint endpoints[MAX_ENDPOINTS], current;
static UINT endpoint_count;
static float level;
static BOOL muted, have_device;
static BOOL expanded, dragging;
static float list_shown;                 /* 0 closed - 1 open */
static DWORD animation_start;
static int scroll;
static enum hit hot = HIT_NONE, pressed = HIT_NONE;
static int hot_item = -1, pressed_item = -1;
static HFONT font_body, font_body_bold, font_level;
static DWORD hidden_at;
static int anchor_x, anchor_y;
static BOOL anchor_above;

static BOOL can_choose(void)
{
    return have_device && endpoint_count > 1;
}

static int list_height(void)
{
    return min( (int)endpoint_count * ROW_HEIGHT, LIST_MAX );
}

static int shown_list_height(void)
{
    return (int)(list_height() * list_shown + 0.5f);
}

/* what plays, on top, as MtcUvc's TransportControls (media.c) */
static struct media_info media;

static int media_height(void)
{
    return media.present ? MEDIA_CARD_HEIGHT : 0;
}

static int volume_top(void)
{
    return media_height() + ROW_HEIGHT + shown_list_height();
}

/* Windows 11's: the row, then the outputs under it */
static int row11_top(void)
{
    return media_height() + PAD11;
}

static int list11_top(void)
{
    return row11_top() + ROW11 + px( 4 );
}

static int list11_height(void)
{
    return min( (int)endpoint_count * ITEM11, LIST_MAX11 );
}

static int shown11(void)
{
    return (int)((list11_height() + px( 4 )) * list_shown + 0.5f);
}

static void row11_parts( RECT *mute, RECT *slider, RECT *value, RECT *chevron )
{
    int top = row11_top(), right = FLYOUT_WIDTH - PAD11 - (can_choose() ? BUTTON11 : 0);

    SetRect( mute, PAD11, top + px( 4 ), PAD11 + BUTTON11, top + px( 4 ) + BUTTON11 );
    SetRect( value, right - LEVEL11, top, right, top + ROW11 );
    SetRect( slider, mute->right + px( 4 ), top, value->left - px( 4 ), top + ROW11 );
    SetRect( chevron, FLYOUT_WIDTH - PAD11 - BUTTON11, top + px( 4 ), FLYOUT_WIDTH - PAD11, top + px( 4 ) + BUTTON11 );
}

static int flyout_height(void)
{
    if (style11) return row11_top() + ROW11 + shown11() + PAD11;
    return volume_top() + VOLUME_ROW;
}

static int thumb_center(void)
{
    return SIDE_COLUMN + THUMB_WIDTH / 2 + (int)(level * (SLIDER_WIDTH - THUMB_WIDTH) + 0.5f);
}

static void place(void)
{
    int height = flyout_height();

    SetWindowPos( flyout, HWND_TOPMOST, anchor_x, anchor_above ? anchor_y - height : anchor_y, FLYOUT_WIDTH, height,
                  SWP_NOACTIVATE );
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

static void reload( BOOL with_list )
{
    have_device = audio_state( &current, &level, &muted );
    if (with_list) endpoint_count = have_device ? audio_endpoints( endpoints, MAX_ENDPOINTS ) : 0;
    media_current( &media );
}

/**********************************************************************
 *          Painting
 */

/* white text at an opacity: over the acrylic, GDI's grey is white seen through */
static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, BYTE alpha, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, arctic_keyed_grey( shell_light, alpha ) );
    DrawTextW( hdc, text, -1, rect, flags | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

static void paint_header( HDC hdc )
{
    RECT rect = { 0, media_height(), FLYOUT_WIDTH, media_height() + ROW_HEIGHT };
    const WCHAR *name = have_device ? current.name : load_string( IDS_NO_DEVICE );

    if (can_choose() && (hot == HIT_HEADER || pressed == HIT_HEADER))
        fill_alpha( hdc, &rect, INK, 0x33 );
    rect.left = px( 12 );
    rect.right = can_choose() ? FLYOUT_WIDTH - px( 36 ) : FLYOUT_WIDTH - px( 12 );
    draw_text( hdc, name, &rect, expanded ? font_body_bold : font_body, 255, DT_LEFT | DT_VCENTER );
    if (can_choose()) draw_chevron( hdc, FLYOUT_WIDTH - px( 18 ), media_height() + ROW_HEIGHT / 2, !expanded, 255 );
}

static void paint_list( HDC hdc )
{
    int top = media_height() + ROW_HEIGHT, shown = shown_list_height(), total = endpoint_count * ROW_HEIGHT;
    HRGN clip;

    if (shown <= 0) return;
    clip = CreateRectRgn( 0, top, FLYOUT_WIDTH, top + shown );
    SelectClipRgn( hdc, clip );
    for (UINT i = 0; i < endpoint_count; i++)
    {
        RECT rect = { 0, top + i * ROW_HEIGHT - scroll, FLYOUT_WIDTH - px( 12 ), top + (i + 1) * ROW_HEIGHT - scroll };
        BOOL chosen = !wcscmp( endpoints[i].id, current.id ), over = hot == HIT_ITEM && hot_item == (int)i;

        if (rect.bottom <= top || rect.top >= top + shown) continue;
        if (chosen) fill_alpha( hdc, &rect, ACCENT_COLOR, over ? 0xcc : 0x99 );
        else if (over) fill_alpha( hdc, &rect, INK, pressed == HIT_ITEM ? 0x33 : 0x19 );
        rect.left += px( 12 );
        rect.right -= px( 12 );
        draw_text( hdc, endpoints[i].name, &rect, font_body, 255, DT_LEFT | DT_VCENTER );
    }
    /* the scroll bar of the ScrollViewer, a thin one, while the list is longer */
    if (total > LIST_MAX)
    {
        int length = max( px( 16 ), LIST_MAX * LIST_MAX / total ), pos = scroll * (LIST_MAX - length) / (total - LIST_MAX);
        RECT bar = { FLYOUT_WIDTH - px( 6 ), top + pos + px( 2 ), FLYOUT_WIDTH - px( 4 ), top + pos + length - px( 2 ) };
        fill_alpha( hdc, &bar, INK, 0x66 );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
}

static void paint_volume( HDC hdc )
{
    int top = volume_top(), center = thumb_center(), track_y = top + px( 26 );
    float opacity = 1.f - (1.f - DIMMED) * list_shown;
    BOOL over_mute = hot == HIT_MUTE, down_mute = pressed == HIT_MUTE && over_mute;
    BOOL over_slider = (hot == HIT_SLIDER || dragging) && have_device;
    BYTE glyph = down_mute ? 0xff : over_mute ? 0xe6 : 0xcc, bars = down_mute ? 0x4d : over_mute ? 0x45 : 0x3d;
    RECT rect;
    WCHAR text[8];

    /* the mute toggle: the glyph centred in 12..56, over the 8 left under it */
    draw_speaker( hdc, px( 34 ) - GLYPH_SIZE / 2, top + px( 24 ) - GLYPH_SIZE / 2, GLYPH_SIZE, level, muted || !have_device,
                  (BYTE)(glyph * opacity), (BYTE)(bars * opacity) );

    /* the slider: the track, the part up to the thumb, the thumb */
    SetRect( &rect, SIDE_COLUMN, track_y, SIDE_COLUMN + SLIDER_WIDTH, track_y + px( 2 ) );
    fill_alpha( hdc, &rect, INK, (BYTE)(0x66 * opacity) );
    if (have_device)
    {
        SetRect( &rect, SIDE_COLUMN, track_y, center, track_y + px( 2 ) );
        fill_alpha( hdc, &rect, ACCENT_COLOR, (BYTE)(255 * opacity) );
    }
    SetRect( &rect, center - THUMB_WIDTH / 2, track_y + px( 1 ) - THUMB_HEIGHT / 2, center + THUMB_WIDTH / 2,
             track_y + px( 1 ) + THUMB_HEIGHT / 2 );
    fill_round_rect( hdc, &rect, px( 4 ), over_slider ? INK : have_device ? ACCENT_COLOR : RGB( 128, 128, 128 ),
                     (BYTE)(255 * opacity) );

    /* the level */
    swprintf( text, ARRAY_SIZE(text), L"%d", have_device ? (int)(level * 100 + 0.5f) : 0 );
    SetRect( &rect, SIDE_COLUMN + SLIDER_WIDTH + px( 8 ), top, FLYOUT_WIDTH - px( 12 ), top + VOLUME_ROW - px( 8 ) );
    draw_text( hdc, text, &rect, font_level, (BYTE)(255 * opacity), DT_CENTER | DT_VCENTER );
}

static enum fluent_state state11( enum hit what, int item )
{
    if (pressed == what && hot == what && (item < 0 || pressed_item == item)) return FLUENT_PRESSED;
    if (hot == what && (item < 0 || hot_item == item)) return FLUENT_HOT;
    return FLUENT_REST;
}

/* the mute button, the slider, the level, the button that opens the outputs */
static void paint_row11( HDC hdc )
{
    RECT mute, slider, value, chevron;
    int glyph = px( 20 );
    enum fluent_state slider_state = !have_device ? FLUENT_DISABLED :
                                     dragging ? FLUENT_PRESSED : hot == HIT_SLIDER ? FLUENT_HOT : FLUENT_REST;
    WCHAR text[8];

    row11_parts( &mute, &slider, &value, &chevron );
    if (have_device) fluent_rect( hdc, &mute, px( 4 ), INK, fluent_subtle_alpha( state11( HIT_MUTE, -1 ), shell_light ) );
    draw_speaker( hdc, (mute.left + mute.right - glyph) / 2, (mute.top + mute.bottom - glyph) / 2, glyph, level,
                  muted || !have_device, have_device ? 255 : 0x5c, 0 );
    fluent_slider( hdc, slider.left, slider.right, (slider.top + slider.bottom) / 2, fluent_unit(),
                   have_device ? level : 0, slider_state, shell_light, 255 );
    swprintf( text, ARRAY_SIZE(text), L"%d", have_device ? (int)(level * 100 + 0.5f) : 0 );
    draw_text( hdc, text, &value, font_body, have_device ? 255 : 0x5c, DT_RIGHT | DT_VCENTER );
    if (can_choose())
    {
        fluent_rect( hdc, &chevron, px( 4 ), INK, fluent_subtle_alpha( state11( HIT_HEADER, -1 ), shell_light ) );
        fluent_chevron( hdc, (chevron.left + chevron.right) / 2, (chevron.top + chevron.bottom) / 2, px( 12 ),
                        expanded ? FLUENT_UP : FLUENT_DOWN, INK, 255 );
    }
}

/* the outputs: rounded rows, the one in use ticked in the accent */
static void paint_list11( HDC hdc )
{
    int top = list11_top(), shown = shown11() - px( 4 ), total = endpoint_count * ITEM11;
    HRGN clip;

    if (shown <= 0) return;
    clip = CreateRectRgn( 0, top, FLYOUT_WIDTH, top + shown );
    SelectClipRgn( hdc, clip );
    for (UINT i = 0; i < endpoint_count; i++)
    {
        RECT rect = { px( 8 ), top + i * ITEM11 - scroll, FLYOUT_WIDTH - px( 8 ), top + (i + 1) * ITEM11 - scroll };
        BOOL chosen = !wcscmp( endpoints[i].id, current.id );
        enum fluent_state state = state11( HIT_ITEM, i );
        BYTE fill = fluent_subtle_alpha( state, shell_light );

        if (rect.bottom <= top || rect.top >= top + shown) continue;
        if (chosen && !fill) fill = fluent_subtle_alpha( FLUENT_HOT, shell_light );
        InflateRect( &rect, 0, -px( 2 ) );
        fluent_rect( hdc, &rect, px( 4 ), INK, fill );
        if (chosen)
        {
            /* CheckMark, 12 across */
            float cx = rect.right - px( 20 ), cy = (rect.top + rect.bottom) / 2.f, u = fluent_unit();
            fluent_line( hdc, cx - 5 * u, cy, cx - 1.5f * u, cy + 3.5f * u, 1.3f * u, FLUENT_ACCENT( shell_light ), 255 );
            fluent_line( hdc, cx - 1.5f * u, cy + 3.5f * u, cx + 5.5f * u, cy - 4 * u, 1.3f * u, FLUENT_ACCENT( shell_light ), 255 );
        }
        rect.left += px( 12 );
        rect.right -= px( 40 );
        draw_text( hdc, endpoints[i].name, &rect, font_body, 255, DT_LEFT | DT_VCENTER );
    }
    if (total > LIST_MAX11)
    {
        int length = max( px( 16 ), LIST_MAX11 * LIST_MAX11 / total ), pos = scroll * (LIST_MAX11 - length) / (total - LIST_MAX11);
        RECT bar = { FLYOUT_WIDTH - px( 5 ), top + pos + px( 2 ), FLYOUT_WIDTH - px( 3 ), top + pos + length - px( 2 ) };
        fluent_rect( hdc, &bar, px( 1 ), INK, 0x66 );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( hwnd, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    void *bits;

    GetClientRect( hwnd, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    mem = CreateCompatibleDC( hdc );
    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        /* black is the acrylic seen through */
        arctic_fill_back( mem, &client, shell_light );
        SetBkMode( mem, TRANSPARENT );
        if (media.present)
        {
            RECT card = { 0, 0, MEDIA_CARD_WIDTH, MEDIA_CARD_HEIGHT };
            media_paint( mem, &card, &media, hot == HIT_MEDIA ? hot_item : -1, pressed == HIT_MEDIA, 255 );
        }
        if (style11)
        {
            paint_row11( mem );
            paint_list11( mem );
        }
        else
        {
            paint_header( mem );
            paint_list( mem );
            paint_volume( mem );
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

static enum hit hit_test11( POINT pt, int *item )
{
    RECT mute, slider, value, chevron;
    int top = list11_top();

    row11_parts( &mute, &slider, &value, &chevron );
    if (have_device && PtInRect( &mute, pt )) return HIT_MUTE;
    if (have_device && pt.x >= slider.left && pt.x < slider.right && pt.y >= slider.top && pt.y < slider.bottom)
        return HIT_SLIDER;
    if (can_choose() && PtInRect( &chevron, pt )) return HIT_HEADER;
    if (expanded && pt.y >= top && pt.y < top + shown11() - px( 4 ) && pt.x >= px( 8 ) && pt.x < FLYOUT_WIDTH - px( 8 ))
    {
        int i = (pt.y - top + scroll) / ITEM11;

        if (i < 0 || i >= (int)endpoint_count) return HIT_NONE;
        *item = i;
        return HIT_ITEM;
    }
    return HIT_NONE;
}

static enum hit hit_test( POINT pt, int *item )
{
    int top = volume_top();

    *item = -1;
    if (pt.y < 0 || pt.x < 0 || pt.x >= FLYOUT_WIDTH) return HIT_NONE;
    if (style11 && pt.y >= media_height()) return hit_test11( pt, item );
    if (pt.y < media_height())
    {
        RECT card = { 0, 0, MEDIA_CARD_WIDTH, MEDIA_CARD_HEIGHT };
        return (*item = media_hit( &card, pt, &media )) >= 0 ? HIT_MEDIA : HIT_NONE;
    }
    if (pt.y < media_height() + ROW_HEIGHT) return can_choose() ? HIT_HEADER : HIT_NONE;
    if (pt.y < top)
    {
        int i = (pt.y - media_height() - ROW_HEIGHT + scroll) / ROW_HEIGHT;

        if (!expanded || pt.x >= FLYOUT_WIDTH - px( 12 ) || i < 0 || i >= (int)endpoint_count) return HIT_NONE;
        *item = i;
        return HIT_ITEM;
    }
    if (list_shown > 0 || !have_device) return HIT_NONE;
    if (pt.x >= px( 12 ) && pt.x < SIDE_COLUMN - px( 8 ) && pt.y < top + VOLUME_ROW - px( 8 )) return HIT_MUTE;
    if (pt.x >= SIDE_COLUMN && pt.x < SIDE_COLUMN + SLIDER_WIDTH) return HIT_SLIDER;
    return HIT_NONE;
}

static void set_level( float value )
{
    int before = (int)(level * 100 + 0.5f), after;

    value = max( 0.f, min( 1.f, value ) );
    after = (int)(value * 100 + 0.5f);
    level = after / 100.f;
    if (after == before) return;
    audio_set_level( level );
    /* a change of the level unmutes, as in Windows */
    if (muted)
    {
        muted = FALSE;
        audio_set_mute( FALSE );
    }
    InvalidateRect( flyout, NULL, FALSE );
    tray_update();
}

static void set_level_at( int x )
{
    if (style11)
    {
        RECT mute, slider, value, chevron;

        row11_parts( &mute, &slider, &value, &chevron );
        set_level( fluent_slider_value( slider.left, slider.right, fluent_unit(), x ) );
        return;
    }
    set_level( (float)(x - SIDE_COLUMN - THUMB_WIDTH / 2) / (SLIDER_WIDTH - THUMB_WIDTH) );
}

static void step_level( int percent )
{
    set_level( ((int)(level * 100 + 0.5f) + percent) / 100.f );
}

static void animate_list( BOOL open )
{
    expanded = open;
    animation_start = GetTickCount();
    if (open)
    {
        reload( TRUE );
        scroll = 0;
    }
    SetTimer( flyout, TIMER_ANIMATE, 15, NULL );
    InvalidateRect( flyout, NULL, FALSE );
}

static void on_animate(void)
{
    float t = (GetTickCount() - animation_start) / (float)EXPAND_MS, eased;

    if (t >= 1.f)
    {
        t = 1.f;
        KillTimer( flyout, TIMER_ANIMATE );
    }
    eased = t >= 1.f ? 1.f : 1.f - powf( 2.f, -10.f * t );   /* ExponentialEase, EaseOut */
    list_shown = expanded ? eased : 1.f - eased;
    place();
    InvalidateRect( flyout, NULL, FALSE );
}

static void on_click( enum hit hit, int item )
{
    switch (hit)
    {
    case HIT_HEADER:
        animate_list( !expanded );
        break;
    case HIT_ITEM:
        if (wcscmp( endpoints[item].id, current.id ))
        {
            audio_set_default( endpoints[item].id );
            reload( FALSE );
            tray_update();
        }
        animate_list( FALSE );
        break;
    case HIT_MUTE:
        muted = !muted;
        audio_set_mute( muted );
        InvalidateRect( flyout, NULL, FALSE );
        tray_update();
        break;
    case HIT_MEDIA:
        media_press( &media, item );
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
        if (wparam == TIMER_ANIMATE) on_animate();
        else if (wparam == TIMER_POLL && !dragging)
        {
            /* the volume keys and other programs change it too */
            struct endpoint before = current;
            struct media_info old_media = media;
            float old_level = level;
            BOOL old_muted = muted, old_device = have_device;

            reload( FALSE );
            if (old_media.present != media.present) place();
            if (old_level != level || old_muted != muted || old_device != have_device || wcscmp( before.id, current.id ) ||
                memcmp( &old_media, &media, sizeof(media) ))
                InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        enum hit hit;
        int item;

        if (dragging)
        {
            set_level_at( pt.x );
            return 0;
        }
        hit = hit_test( pt, &item );
        if (hit != hot || item != hot_item)
        {
            hot = hit;
            hot_item = item;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        if (!dragging)
        {
            hot = pressed = HIT_NONE;
            hot_item = pressed_item = -1;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

        pressed = hit_test( pt, &pressed_item );
        if (pressed == HIT_SLIDER)
        {
            /* a press on the track moves the thumb there, then it follows */
            dragging = TRUE;
            SetCapture( hwnd );
            set_level_at( pt.x );
        }
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        enum hit hit;
        int item;

        if (dragging)
        {
            dragging = FALSE;
            ReleaseCapture();
        }
        hit = hit_test( pt, &item );
        if (hit == pressed && item == pressed_item) on_click( hit, item );
        pressed = HIT_NONE;
        pressed_item = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_CAPTURECHANGED:
        dragging = FALSE;
        return 0;

    case WM_MOUSEWHEEL:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int notches = GET_WHEEL_DELTA_WPARAM( wparam ) / WHEEL_DELTA, item;

        ScreenToClient( hwnd, &pt );
        if (style11 && expanded && pt.y >= list11_top())
        {
            int most = max( 0, (int)endpoint_count * ITEM11 - list11_height() );
            scroll = max( 0, min( most, scroll - notches * ITEM11 ) );
            hot = hit_test( pt, &item );
            hot_item = item;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (style11 && have_device) step_level( notches * 2 );
        else if (expanded && pt.y >= media_height() + ROW_HEIGHT && pt.y < volume_top())
        {
            int most = max( 0, (int)endpoint_count * ROW_HEIGHT - list_height() );
            scroll = max( 0, min( most, scroll - notches * ROW_HEIGHT ) );
            hot = hit_test( pt, &item );
            hot_item = item;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (have_device && !expanded) step_level( notches * 2 );
        return 0;
    }

    case WM_KEYDOWN:
        switch (wparam)
        {
        case VK_ESCAPE: flyout_hide(); break;
        case VK_LEFT:
        case VK_DOWN:   if (have_device) step_level( -1 ); break;
        case VK_RIGHT:
        case VK_UP:     if (have_device) step_level( 1 ); break;
        case VK_NEXT:   if (have_device) step_level( -10 ); break;
        case VK_PRIOR:  if (have_device) step_level( 10 ); break;
        case VK_HOME:   if (have_device) set_level( 0 ); break;
        case VK_END:    if (have_device) set_level( 1 ); break;
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

static BOOL create_flyout(void)
{
    struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, ACRYLIC_TINT, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
    WNDCLASSW cls = { 0 };
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP, corner = ARCTIC_FLYOUT_CORNER;

    if (flyout) return TRUE;
    cls.lpfnWndProc = flyout_proc;
    cls.hInstance = sndvolsso_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticVolumeFlyout";
    RegisterClassW( &cls );
    flyout = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                              0, 0, FLYOUT_WIDTH, ROW_HEIGHT + VOLUME_ROW, NULL, NULL, sndvolsso_instance, NULL );
    if (!flyout) return FALSE;
    SetWindowCompositionAttribute( flyout, &attr );
    DwmSetWindowAttribute( flyout, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( flyout, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_body = shell_font( px( 15 ), FW_NORMAL );
    font_body_bold = shell_font( px( 15 ), FW_SEMIBOLD );
    font_level = shell_font( px( 24 ), 350 /* SemiLight */ );
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
    KillTimer( flyout, TIMER_ANIMATE );
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
    expanded = dragging = FALSE;
    list_shown = 0;
    scroll = 0;
    hot = pressed = HIT_NONE;
    hot_item = pressed_item = -1;
    reload( TRUE );
    anchor();
    place();
    InvalidateRect( flyout, NULL, FALSE );
    shell_look( flyout );
    ShowWindow( flyout, SW_SHOW );
    SetForegroundWindow( flyout );
    SetFocus( flyout );
    SetTimer( flyout, TIMER_POLL, 250, NULL );
}

/* the volume changed elsewhere (the volume keys) */
void flyout_refresh(void)
{
    if (!flyout_visible() || dragging) return;
    reload( FALSE );
    InvalidateRect( flyout, NULL, FALSE );
}
