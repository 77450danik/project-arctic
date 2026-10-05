/*
 * Network icon of the notification area: the flyout of Windows 10
 *
 * NetworkUX.MainPage of Windows 10 (Windows.UI.ShellCommon.pri), drawn with
 * GDI over the shell's look (startui.dll, the taskbar's page Оформлення: the
 * acrylic #101010 at 75% of the flyout by default). Every size, colour and
 * text is the XAML's; only the glyphs are our own, Segoe MDL2 Assets not
 * being ours to ship. docs/network-flyout.md has the numbers and where they
 * come from.
 *
 * Wi-Fi switches the radio (WlanSetInterface, iwd's Device.Powered);
 * airplane mode switches it off and remembers that it did. A mobile hotspot
 * is not there yet: its tile is greyed.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "winsock2.h"
#include "ws2ipdef.h"
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "shellapi.h"
#include "iphlpapi.h"
#include "wlanapi.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

#include "pnidui.h"

WINE_DEFAULT_DEBUG_CHANNEL(pnidui);

#define FLYOUT_WIDTH   px(360)
#define GLYPH          px(24)
#define TILE_HEIGHT    px(64)

#define ACCENT         RGB( 0x00, 0x78, 0xd7 )
#define LINK_COLOR     RGB( 0x99, 0xeb, 0xff )

#define REG_KEY        L"Software\\Arctic\\Network"

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum entry_kind { ENTRY_WIRED, ENTRY_WIRELESS, ENTRY_MESSAGE };

struct entry
{
    enum entry_kind kind;
    WCHAR name[64];
    WCHAR detail[96];           /* UnselectedStatus */
    WCHAR detail_selected[96];  /* Status */
    DOT11_SSID ssid;
    WCHAR profile[WLAN_MAX_NAME_LENGTH];
    UINT quality;
    BOOL secured, connected, has_profile, connecting;
};

enum hit { HIT_NONE, HIT_ENTRY, HIT_BUTTON, HIT_CHECK, HIT_LINK, HIT_TILE };
enum tile { TILE_WIFI, TILE_AIRPLANE, TILE_HOTSPOT, TILE_COUNT };

static HWND flyout;
static struct entry *entries;
static UINT entry_count;
static int selected = -1, scroll;
static enum hit hot = HIT_NONE, pressed = HIT_NONE;
static int hot_index = -1, pressed_index = -1;
static HFONT font_name, font_tile;
static BOOL radio_on, airplane;
static DWORD hidden_at;
BOOL asking_key;

/**********************************************************************
 *          Glyphs: Segoe MDL2 Assets' Wifi, Ethernet, Airplane and
 *          Hotspot, drawn as lines and arcs on a 16 unit grid
 */

#define SUPERSAMPLE 4
#define PI 3.14159265f

static BOOL near_segment( float x, float y, float ax, float ay, float bx, float by, float width )
{
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0, ex, ey;

    t = max( 0.f, min( 1.f, t ) );
    ex = ax + t * dx - x;
    ey = ay + t * dy - y;
    return ex * ex + ey * ey <= width * width / 4;
}

/* an arc around (cx, cy) between two angles in degrees, clockwise on the screen */
static BOOL near_arc( float x, float y, float cx, float cy, float r, float from, float to, float width )
{
    float dx = x - cx, dy = y - cy, d = sqrtf( dx * dx + dy * dy ), t;

    if (fabsf( d - r ) > width / 2) return FALSE;
    t = atan2f( dy, dx ) * 180 / PI - from;
    while (t < 0) t += 360;
    while (t >= 360) t -= 360;
    return t <= to - from;
}

enum glyph { GLYPH_WIFI, GLYPH_ETHERNET, GLYPH_AIRPLANE, GLYPH_HOTSPOT, GLYPH_LOCK };

/* coverage of one sample: 1 lit, 2 dim (the arcs above the signal), 0 none */
static int sample( enum glyph glyph, float x, float y, UINT level )
{
    switch (glyph)
    {
    case GLYPH_WIFI:
    {
        /* a little left and smaller, so a padlock fits bottom right, as in Windows */
        static const float radii[3] = { 3.6f, 6.8f, 10.0f };
        if ((x - 7) * (x - 7) + (y - 12.6f) * (y - 12.6f) <= 1.35f * 1.35f) return level ? 1 : 2;
        for (int i = 0; i < 3; i++)
            if (near_arc( x, y, 7, 12.6f, radii[i], -135, -45, 1.05f )) return (UINT)i + 1 < level ? 1 : 2;
        return 0;
    }
    case GLYPH_ETHERNET:
        /* a screen on its foot, a cable going down */
        if (near_segment( x, y, 1.5f, 2, 14.5f, 2, 1 ) || near_segment( x, y, 14.5f, 2, 14.5f, 10.5f, 1 ) ||
            near_segment( x, y, 14.5f, 10.5f, 1.5f, 10.5f, 1 ) || near_segment( x, y, 1.5f, 10.5f, 1.5f, 2, 1 ) ||
            near_segment( x, y, 8, 10.5f, 8, 14, 1 ) || near_segment( x, y, 4.5f, 14, 11.5f, 14, 1 ))
            return 1;
        return 0;
    case GLYPH_AIRPLANE:
        if (near_segment( x, y, 8, 1, 8, 13, 1.4f ) || near_segment( x, y, 1, 9, 8, 5.5f, 1.1f ) ||
            near_segment( x, y, 8, 5.5f, 15, 9, 1.1f ) || near_segment( x, y, 5, 15, 8, 12.8f, 1.1f ) ||
            near_segment( x, y, 8, 12.8f, 11, 15, 1.1f ))
            return 1;
        return 0;
    case GLYPH_HOTSPOT:
        if ((x - 8) * (x - 8) + (y - 8) * (y - 8) <= 1.4f * 1.4f) return 1;
        if (near_arc( x, y, 8, 8, 4, -40, 40, 1 ) || near_arc( x, y, 8, 8, 4, 140, 220, 1 ) ||
            near_arc( x, y, 8, 8, 7, -40, 40, 1 ) || near_arc( x, y, 8, 8, 7, 140, 220, 1 ))
            return 1;
        return 0;
    case GLYPH_LOCK:
        if (x >= 11.4f && x <= 15.8f && y >= 12.2f && y <= 15.8f) return 1;
        if (near_arc( x, y, 13.6f, 12.2f, 1.45f, -180, 0, 0.8f )) return 1;
        return 0;
    }
    return 0;
}

static HBITMAP create_dib( HDC hdc, int width, int height, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    return CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0 );
}

/* premultiplied pixels over what is there */
static void blend_pixels( HDC hdc, int x, int y, int width, int height, const UINT32 *pixels )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    UINT32 *bits;
    HBITMAP bitmap;

    if ((bitmap = create_dib( hdc, width, height, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        memcpy( bits, pixels, width * height * 4 );
        GdiAlphaBlend( hdc, x, y, width, height, mem, 0, 0, width, height, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

static void draw_glyph( HDC hdc, enum glyph glyph, int x, int y, int size, UINT level, COLORREF color, BYTE alpha )
{
    float scale = 16.0f / size;
    UINT32 *pixels = malloc( size * size * 4 );

    if (!pixels) return;
    for (int py = 0; py < size; py++)
    {
        for (int px_ = 0; px_ < size; px_++)
        {
            float lit = 0, dim = 0;
            UINT32 a;

            for (int j = 0; j < SUPERSAMPLE; j++)
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    int s = sample( glyph, (px_ + (i + 0.5f) / SUPERSAMPLE) * scale, (py + (j + 0.5f) / SUPERSAMPLE) * scale, level );
                    if (s == 1) lit++;
                    else if (s == 2) dim++;
                }
            /* the arcs above the signal are there, faint */
            a = (UINT32)((lit + dim * 0.3f) / (SUPERSAMPLE * SUPERSAMPLE) * alpha + 0.5f);
            pixels[py * size + px_] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                      ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    blend_pixels( hdc, x, y, size, size, pixels );
    free( pixels );
}

static void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    UINT32 pixel = ((UINT32)alpha << 24) | ((GetRValue( color ) * alpha / 255) << 16) |
                   ((GetGValue( color ) * alpha / 255) << 8) | (GetBValue( color ) * alpha / 255);
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem;
    UINT32 *bits;
    HBITMAP bitmap;

    if (width <= 0 || height <= 0) return;
    mem = CreateCompatibleDC( hdc );
    if ((bitmap = create_dib( hdc, 1, 1, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        *bits = pixel;
        GdiAlphaBlend( hdc, rect->left, rect->top, width, height, mem, 0, 0, 1, 1, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

static HFONT make_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = ANTIALIASED_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, format | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

/**********************************************************************
 *          What it lists
 */

static void read_radio(void)
{
    WLAN_RADIO_STATE *radio = NULL;
    DWORD size, value = 0, value_size = sizeof(value);
    HKEY key;

    radio_on = have_wifi;
    if (have_wifi && !WlanQueryInterface( wlan, &wifi_guid, wlan_intf_opcode_radio_state, NULL, &size,
                                          (void **)&radio, NULL ))
    {
        radio_on = radio->dwNumberOfPhys && radio->PhyRadioState[0].dot11SoftwareRadioState == dot11_radio_state_on;
        WlanFreeMemory( radio );
    }
    airplane = FALSE;
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key ))
    {
        if (!RegQueryValueExW( key, L"AirplaneMode", NULL, NULL, (BYTE *)&value, &value_size )) airplane = !!value;
        RegCloseKey( key );
    }
}

static struct entry *add_entry( enum entry_kind kind, const WCHAR *name )
{
    struct entry *grown = realloc( entries, (entry_count + 1) * sizeof(*entries) );

    if (!grown) return NULL;
    entries = grown;
    memset( &entries[entry_count], 0, sizeof(*entries) );
    entries[entry_count].kind = kind;
    lstrcpynW( entries[entry_count].name, name, ARRAY_SIZE(entries[0].name) );
    return &entries[entry_count++];
}

static void fill_entries(void)
{
    WLAN_AVAILABLE_NETWORK_LIST *networks = NULL;
    struct state state = { 0 };
    struct entry *entry;

    free( entries );
    entries = NULL;
    entry_count = 0;
    read_wired( &state );
    read_wifi( &state );
    read_radio();

    if (state.wired_present && (entry = add_entry( ENTRY_WIRED, load_string( IDS_NETWORK ) )))
    {
        entry->connected = state.wired_connected;
        lstrcpyW( entry->detail, load_string( state.wired_connected ? IDS_CONNECTED :
                                              state.wired_limited ? IDS_NO_INTERNET : IDS_WIRED_NONE ) );
        lstrcpyW( entry->detail_selected, entry->detail );
    }

    if (have_wifi && !radio_on)
    {
        add_entry( ENTRY_MESSAGE, load_string( airplane ? IDS_AIRPLANE_ON : IDS_WIFI_OFF ) );
        return;
    }

    /* the networks in range, the one in use first, each once */
    if (have_wifi && !WlanGetAvailableNetworkList( wlan, &wifi_guid, 0, NULL, &networks ))
    {
        for (int pass = 0; pass < 2; pass++)
        {
            for (DWORD i = 0; i < networks->dwNumberOfItems; i++)
            {
                const WLAN_AVAILABLE_NETWORK *net = &networks->Network[i];
                BOOL connected = !!(net->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED);
                WCHAR ssid[64];

                if (!net->dot11Ssid.uSSIDLength || connected != !pass) continue;
                ssid_to_text( &net->dot11Ssid, ssid, ARRAY_SIZE(ssid) );
                if (!(entry = add_entry( ENTRY_WIRELESS, ssid ))) break;
                entry->ssid = net->dot11Ssid;
                entry->quality = net->wlanSignalQuality;
                entry->secured = net->bSecurityEnabled;
                entry->connected = connected;
                entry->has_profile = !!(net->dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE);
                entry->connecting = !wcscmp( ssid, connecting_ssid );
                lstrcpynW( entry->profile, net->strProfileName, ARRAY_SIZE(entry->profile) );
                /* the texts of Windows 10: "Є підключення, безпечне", "Безпечне", "Відкрита" */
                if (entry->connecting) lstrcpyW( entry->detail, load_string( IDS_CONNECTING ) );
                else if (connected)
                    lstrcpyW( entry->detail, load_string( entry->secured ? IDS_CONNECTED_SECURED : IDS_CONNECTED_OPEN ) );
                else lstrcpyW( entry->detail, load_string( entry->secured ? IDS_SECURED : IDS_OPEN_NETWORK ) );
                lstrcpyW( entry->detail_selected, entry->detail );
            }
        }
        if (!networks->dwNumberOfItems) add_entry( ENTRY_MESSAGE, load_string( IDS_NO_NETWORKS ) );
        WlanFreeMemory( networks );
    }
    if (!entry_count) add_entry( ENTRY_MESSAGE, load_string( IDS_NO_NETWORKS ) );
}

/**********************************************************************
 *          Layout: NetworkUX.MainPage, sizes in DIPs at the shell's DPI
 */

static int line15, line12;      /* the heights of a line at 15 and at 12 */
static int entry_top[256], separator_y, list_content;
static BOOL autoconnect = TRUE;

static int text_block(void)
{
    /* the name, the state at 15 and the 6 under it; a row's text is 45 high at least */
    return max( px( 45 ), 2 * line15 + px( 6 ) );
}

static int entry_height( int i )
{
    const struct entry *entry = &entries[i];
    int height;

    if (entry->kind == ENTRY_MESSAGE)  /* Wi-Fi off: the glyph's margins 12,2,12,22; the text's 0,5 */
        return max( px( 2 + 26 + 22 ), px( 5 ) + 2 * line15 + px( 6 ) + px( 5 ) );
    if (entry->kind == ENTRY_WIRED)    /* ConnectionListTemplate: 0,5,0,4 around the text */
        return px( 5 ) + text_block() + px( 4 );
    /* EntityListViewItemStyle: padding 0,5,10,4 */
    height = px( 5 ) + text_block() + px( 4 );
    if (i == selected)
    {
        /* the pre-connection content: margin 48,4,0,0, a panel 299 wide with 4 under it */
        height += px( 4 );
        if (!entry->connected) height += px( 32 );          /* Connect automatically */
        height += px( 12 ) + px( 32 ) + px( 4 );             /* the button, its margin, the panel's */
    }
    return height;
}

/* where each row starts in the list, and the separator between the
 * connections and the wireless networks (margin 12 all round) */
static void layout_entries(void)
{
    int y = px( 12 );   /* the StackPanel's margin 0,12,0,0 */

    separator_y = -1;
    for (UINT i = 0; i < entry_count && i < ARRAY_SIZE(entry_top); i++)
    {
        if (i && entries[i - 1].kind == ENTRY_WIRED && entries[i].kind != ENTRY_WIRED)
        {
            separator_y = y + px( 12 );
            y += px( 12 ) + 1 + px( 12 );
        }
        entry_top[i] = y;
        y += entry_height( i );
    }
    list_content = y;
}

static int content_height(void)
{
    return list_content;
}

static int list_height(void)
{
    RECT work;

    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    return min( list_content, (work.bottom - work.top) * 3 / 5 );
}

static int link_top(void)
{
    return list_height() + px( 5 );   /* SettingsLink: margin 12,5,12,0 */
}

static int link_height(void)
{
    return px( 5 ) + line15 + px( 6 );  /* HyperlinkButton padding 0,5,0,6 */
}

static int help_height(void)
{
    HDC hdc = GetDC( flyout );
    HGDIOBJ old = SelectObject( hdc, font_tile );
    RECT rect = { 0, 0, FLYOUT_WIDTH - 2 * px( 12 ), 0 };

    DrawTextW( hdc, load_string( IDS_SETTINGS_HELP ), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( hdc, old );
    ReleaseDC( flyout, hdc );
    return rect.bottom;
}

static int tiles_top(void)
{
    /* the help: margin 12,-5,12,12 */
    return link_top() + link_height() - px( 5 ) + help_height() + px( 12 );
}

static int flyout_height(void)
{
    return tiles_top() + TILE_HEIGHT + px( 4 );
}

static BOOL entry_rect( int index, RECT *rect )
{
    int top = entry_top[index] - scroll;

    SetRect( rect, 0, top, FLYOUT_WIDTH, top + entry_height( index ) );
    return rect->bottom > 0 && rect->top < list_height();
}

static int text_left( int index )
{
    /* the glyph's 24 with margins 12,0; a connection's FontIcon is 26 */
    return entries[index].kind == ENTRY_WIRED ? px( 12 + 26 + 12 ) : px( 48 );
}

static RECT check_rect( int index )
{
    RECT rect;

    entry_rect( index, &rect );
    SetRect( &rect, px( 48 ), rect.top + px( 5 ) + text_block() + px( 4 ), px( 48 + 299 ),
             rect.top + px( 5 ) + text_block() + px( 4 ) + px( 32 ) );
    return rect;
}

static RECT button_rect( int index )
{
    RECT rect;
    int top, width;
    HDC hdc = GetDC( flyout );
    HGDIOBJ old = SelectObject( hdc, font_name );
    SIZE size;
    const WCHAR *text = load_string( entries[index].connected ? IDS_DISCONNECT_PLAIN : IDS_CONNECT_PLAIN );

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    ReleaseDC( flyout, hdc );
    /* MinWidth 148 (SingleButtonMinWidth), padding 8,4,8,5, 32 high, right in the 299 panel */
    width = max( px( 148 ), size.cx + px( 16 ) );
    entry_rect( index, &rect );
    top = rect.top + px( 5 ) + text_block() + px( 4 ) + (entries[index].connected ? 0 : px( 32 )) + px( 12 );
    SetRect( &rect, px( 48 + 299 ) - width, top, px( 48 + 299 ), top + px( 32 ) );
    return rect;
}

static RECT link_rect(void)
{
    RECT rect;
    SetRect( &rect, px( 12 ), link_top(), FLYOUT_WIDTH - px( 12 ), link_top() + link_height() );
    return rect;
}

/* QuickActionControlStyle: 85 x 64, margin 4,0,0,4, side by side */
static RECT tile_rect( int tile )
{
    RECT rect;
    int left = px( 4 ) + tile * px( 85 + 4 );

    SetRect( &rect, left, tiles_top(), left + px( 85 ), tiles_top() + px( 64 ) );
    return rect;
}

static BOOL tile_enabled( int tile )
{
    if (tile == TILE_HOTSPOT) return FALSE;
    return have_wifi;
}

static BOOL tile_on( int tile )
{
    if (tile == TILE_WIFI) return radio_on;
    if (tile == TILE_AIRPLANE) return airplane;
    return FALSE;
}

static enum hit hit_test( POINT pt, int *index )
{
    RECT rect;

    *index = -1;
    if (pt.y < list_height())
    {
        for (UINT i = 0; i < entry_count; i++)
        {
            if (!entry_rect( i, &rect ) || !PtInRect( &rect, pt )) continue;
            if (entries[i].kind == ENTRY_MESSAGE) return HIT_NONE;
            *index = i;
            if ((int)i == selected && entries[i].kind == ENTRY_WIRELESS)
            {
                if (rect = button_rect( i ), PtInRect( &rect, pt )) return HIT_BUTTON;
                if (!entries[i].connected && (rect = check_rect( i ), PtInRect( &rect, pt ))) return HIT_CHECK;
            }
            return HIT_ENTRY;
        }
        return HIT_NONE;
    }
    if (rect = link_rect(), PtInRect( &rect, pt )) return HIT_LINK;
    for (int t = 0; t < TILE_COUNT; t++)
    {
        rect = tile_rect( t );
        if (PtInRect( &rect, pt ) && tile_enabled( t ))
        {
            *index = t;
            return HIT_TILE;
        }
    }
    return HIT_NONE;
}

/**********************************************************************
 *          Painting
 */

static BOOL is_hot( enum hit what, int index )
{
    return hot == what && hot_index == index;
}

static BOOL is_pressed( enum hit what, int index )
{
    return is_hot( what, index ) && pressed == what && pressed_index == index;
}

/* a colour at some opacity over an opaque one, for text on the accent */
static COLORREF mix( COLORREF top, COLORREF under, int alpha )
{
    return RGB( (GetRValue( top ) * alpha + GetRValue( under ) * (255 - alpha)) / 255,
                (GetGValue( top ) * alpha + GetGValue( under ) * (255 - alpha)) / 255,
                (GetBValue( top ) * alpha + GetBValue( under ) * (255 - alpha)) / 255 );
}

/* GDI writes no alpha; over the opaque accent the pixels must stay opaque */
static UINT32 *paint_bits;
static int paint_width, paint_height;

static void make_opaque( const RECT *rect )
{
    for (int y = max( (int)rect->top, 0 ); y < min( (int)rect->bottom, paint_height ); y++)
        for (int x = max( (int)rect->left, 0 ); x < min( (int)rect->right, paint_width ); x++)
            paint_bits[y * paint_width + x] |= 0xff000000;
}

static void frame( HDC hdc, const RECT *rect, int width, COLORREF color, BYTE alpha )
{
    RECT side;

    SetRect( &side, rect->left, rect->top, rect->right, rect->top + width ); fill_alpha( hdc, &side, color, alpha );
    SetRect( &side, rect->left, rect->bottom - width, rect->right, rect->bottom ); fill_alpha( hdc, &side, color, alpha );
    SetRect( &side, rect->left, rect->top + width, rect->left + width, rect->bottom - width ); fill_alpha( hdc, &side, color, alpha );
    SetRect( &side, rect->right - width, rect->top + width, rect->right, rect->bottom - width ); fill_alpha( hdc, &side, color, alpha );
}

static void paint_entry( HDC hdc, int i )
{
    const struct entry *entry = &entries[i];
    BOOL chosen = i == selected && entry->kind == ENTRY_WIRELESS;
    COLORREF medium = chosen ? mix( RGB( 255, 255, 255 ), ACCENT, 0x99 ) : RGB( 0x99, 0x99, 0x99 );
    RECT rect, text;

    if (!entry_rect( i, &rect )) return;
    if (entry->kind == ENTRY_MESSAGE)
    {
        /* the Wi-Fi category with its radio off */
        draw_glyph( hdc, GLYPH_WIFI, px( 12 ), rect.top + px( 2 ), GLYPH, 4, RGB( 0x66, 0x66, 0x66 ), 255 );
        SetRect( &text, px( 50 ), rect.top + px( 5 ), rect.right - px( 12 ), rect.top + px( 5 ) + line15 );
        draw_text( hdc, load_string( IDS_TILE_WIFI ), &text, font_name, RGB( 255, 255, 255 ), DT_SINGLELINE );
        OffsetRect( &text, 0, line15 );
        draw_text( hdc, entry->name, &text, font_name, medium, DT_SINGLELINE );
        return;
    }

    /* EntityListViewItem: SystemAltHighColor at 10% under the pointer, 20% pressed, the accent picked */
    if (chosen) fill_alpha( hdc, &rect, ACCENT, 255 );
    if (is_pressed( HIT_ENTRY, i )) fill_alpha( hdc, &rect, RGB( 0, 0, 0 ), 0x33 );
    else if (is_hot( HIT_ENTRY, i )) fill_alpha( hdc, &rect, RGB( 0, 0, 0 ), 0x1a );

    if (entry->kind == ENTRY_WIRED)
        draw_glyph( hdc, GLYPH_ETHERNET, px( 12 ), rect.top + px( 5 ), px( 26 ), 4, RGB( 255, 255, 255 ), 255 );
    else
    {
        draw_glyph( hdc, GLYPH_WIFI, px( 12 ), rect.top + px( 5 ), GLYPH,
                    !entry->quality ? 0 : 1 + min( 3, entry->quality / 25 ), RGB( 255, 255, 255 ), 255 );
        if (entry->secured)
            draw_glyph( hdc, GLYPH_LOCK, px( 12 ), rect.top + px( 5 ), GLYPH, 1, RGB( 255, 255, 255 ), 255 );
    }
    SetRect( &text, text_left( i ), rect.top + px( 5 ), rect.right - px( 10 ), rect.top + px( 5 ) + line15 );
    draw_text( hdc, entry->name, &text, font_name, RGB( 255, 255, 255 ), DT_SINGLELINE );
    OffsetRect( &text, 0, line15 );
    draw_text( hdc, chosen ? entry->detail_selected : entry->detail, &text, font_name, medium, DT_SINGLELINE );

    if (chosen)
    {
        RECT button = button_rect( i );
        BYTE fill = is_pressed( HIT_BUTTON, i ) ? 0x66 : 0x33;

        if (!entry->connected)
        {
            RECT check = check_rect( i ), box;

            /* CheckBox: a 20 box, its 2 border white, the label 8 after it */
            SetRect( &box, check.left, (check.top + check.bottom) / 2 - px( 10 ), check.left + px( 20 ),
                     (check.top + check.bottom) / 2 + px( 10 ) );
            if (is_hot( HIT_CHECK, i )) fill_alpha( hdc, &box, RGB( 255, 255, 255 ), 0x33 );
            frame( hdc, &box, px( 2 ), RGB( 255, 255, 255 ), 0xcc );
            if (autoconnect)
            {
                HPEN pen = CreatePen( PS_SOLID, px( 2 ), RGB( 255, 255, 255 ) );
                HGDIOBJ old = SelectObject( hdc, pen );
                MoveToEx( hdc, box.left + px( 4 ), box.top + px( 10 ), NULL );
                LineTo( hdc, box.left + px( 8 ), box.top + px( 14 ) );
                LineTo( hdc, box.left + px( 16 ), box.top + px( 5 ) );
                SelectObject( hdc, old );
                DeleteObject( pen );
            }
            SetRect( &text, box.right + px( 8 ), check.top, check.right, check.bottom );
            draw_text( hdc, load_string( IDS_AUTOCONNECT ), &text, font_name, RGB( 255, 255, 255 ),
                       DT_SINGLELINE | DT_VCENTER );
        }
        /* Button: BaseLow, its border BaseMediumLow under the pointer, BaseMediumLow pressed */
        fill_alpha( hdc, &button, RGB( 255, 255, 255 ), entry->connecting ? 0x33 : fill );
        if (is_hot( HIT_BUTTON, i ) && !entry->connecting) frame( hdc, &button, px( 2 ), RGB( 255, 255, 255 ), 0x66 );
        draw_text( hdc, load_string( entry->connecting ? IDS_CONNECTING : entry->connected ? IDS_DISCONNECT_PLAIN :
                                     IDS_CONNECT_PLAIN ),
                   &button, font_name, entry->connecting ? mix( RGB( 255, 255, 255 ), ACCENT, 0x66 ) : RGB( 255, 255, 255 ),
                   DT_SINGLELINE | DT_VCENTER | DT_CENTER );
        make_opaque( &rect );
    }
}

static void paint_link( HDC hdc )
{
    RECT link = link_rect(), text;

    /* AccentHyperlinkButtonStyle: SystemAccentColorLight3; HyperlinkButton's padding 0,5,0,6 */
    SetRect( &text, link.left, link.top + px( 5 ), link.right, link.top + px( 5 ) + line15 );
    draw_text( hdc, load_string( IDS_SETTINGS_LINK ), &text, font_name,
               is_pressed( HIT_LINK, -1 ) ? RGB( 0x99, 0x99, 0x99 ) : is_hot( HIT_LINK, -1 ) ? RGB( 0xcc, 0xf5, 0xff ) : LINK_COLOR,
               DT_SINGLELINE );
    if (is_hot( HIT_LINK, -1 ))
    {
        HDC dc = hdc;
        SIZE size;
        HGDIOBJ old = SelectObject( dc, font_name );
        const WCHAR *s = load_string( IDS_SETTINGS_LINK );
        RECT underline;

        GetTextExtentPoint32W( dc, s, lstrlenW( s ), &size );
        SelectObject( dc, old );
        SetRect( &underline, text.left, text.top + line15 - px( 2 ), text.left + size.cx, text.top + line15 - px( 1 ) );
        fill_alpha( dc, &underline, RGB( 0xcc, 0xf5, 0xff ), 255 );
    }
    /* the help: Caption at 60%, margin 12,-5,12,12 */
    SetRect( &text, px( 12 ), link.bottom - px( 5 ), FLYOUT_WIDTH - px( 12 ), tiles_top() - px( 12 ) );
    draw_text( hdc, load_string( IDS_SETTINGS_HELP ), &text, font_tile, RGB( 0x99, 0x99, 0x99 ), DT_WORDBREAK );
}

static void paint_tiles( HDC hdc )
{
    static const UINT names[TILE_COUNT] = { IDS_TILE_WIFI, IDS_TILE_AIRPLANE, IDS_TILE_HOTSPOT };
    static const enum glyph glyphs[TILE_COUNT] = { GLYPH_WIFI, GLYPH_AIRPLANE, GLYPH_HOTSPOT };

    for (int t = 0; t < TILE_COUNT; t++)
    {
        RECT rect = tile_rect( t ), text, measure;
        BOOL enabled = tile_enabled( t ), on = tile_on( t ) && enabled;
        COLORREF fore = enabled ? RGB( 255, 255, 255 ) : RGB( 0x66, 0x66, 0x66 );
        int height;

        /* ToggleButtonRevealStyle: BaseLow off, the accent on, BaseMediumLow pressed;
         * the reveal border lights under the pointer */
        if (is_pressed( HIT_TILE, t )) fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), 0x66 );
        else if (on) fill_alpha( hdc, &rect, ACCENT, 255 );
        else fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), 0x33 );
        if (is_hot( HIT_TILE, t )) frame( hdc, &rect, px( 1 ), RGB( 255, 255, 255 ), 0x99 );

        /* padding 3,7,0,3 inside the 1 border: the 16 glyph top left, the title bottom left */
        draw_glyph( hdc, glyphs[t], rect.left + px( 4 ), rect.top + px( 8 ), px( 16 ), 4, fore, 255 );
        SetRect( &measure, 0, 0, rect.right - rect.left - px( 5 ), 0 );
        {
            HGDIOBJ old = SelectObject( hdc, font_tile );
            DrawTextW( hdc, load_string( names[t] ), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
            SelectObject( hdc, old );
        }
        height = min( (int)measure.bottom, 2 * line12 );
        SetRect( &text, rect.left + px( 4 ), rect.bottom - px( 4 ) - height, rect.right - px( 1 ), rect.bottom - px( 4 ) );
        draw_text( hdc, load_string( names[t] ), &text, font_tile, fore, DT_WORDBREAK );
        if (on && !is_pressed( HIT_TILE, t )) make_opaque( &rect );
    }
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT client, list;
    HDC hdc = BeginPaint( hwnd, &ps ), mem = CreateCompatibleDC( hdc );
    UINT32 *bits;
    HBITMAP bitmap;

    GetClientRect( hwnd, &client );
    if ((bitmap = create_dib( hdc, client.right, client.bottom, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        HRGN clip;

        paint_bits = bits;
        paint_width = client.right;
        paint_height = client.bottom;
        /* black is the shell's backdrop seen through */
        FillRect( mem, &client, GetStockObject( BLACK_BRUSH ) );
        SetBkMode( mem, TRANSPARENT );
        SetRect( &list, 0, 0, client.right, list_height() );
        clip = CreateRectRgnIndirect( &list );
        SelectClipRgn( mem, clip );
        for (UINT i = 0; i < entry_count; i++) paint_entry( mem, i );
        if (separator_y >= 0)
        {
            /* SeparatorBorderStyle: a 1 line of BaseLow, margin 12 */
            RECT line = { px( 12 ), separator_y - scroll, client.right - px( 12 ), separator_y - scroll + 1 };
            fill_alpha( mem, &line, RGB( 255, 255, 255 ), 0x33 );
        }
        SelectClipRgn( mem, NULL );
        DeleteObject( clip );
        if (content_height() > list_height())
        {
            /* the panning indicator of the ScrollViewer */
            int bar = max( px( 24 ), list_height() * list_height() / content_height() );
            int top = (list_height() - bar) * scroll / max( 1, content_height() - list_height() );
            RECT thumb = { client.right - px( 4 ), top, client.right - px( 2 ), top + bar };
            fill_alpha( mem, &thumb, RGB( 255, 255, 255 ), 0x99 );
        }
        paint_link( mem );
        paint_tiles( mem );
        GdiFlush();
        BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    EndPaint( hwnd, &ps );
}

/**********************************************************************
 *          Acting
 */

static void xml_escape( const WCHAR *in, WCHAR *out, int size )
{
    int len = 0;

    for (; *in && len < size - 7; in++)
    {
        const WCHAR *rep = *in == '&' ? L"&amp;" : *in == '<' ? L"&lt;" : *in == '>' ? L"&gt;" :
                           *in == '"' ? L"&quot;" : *in == '\'' ? L"&apos;" : NULL;
        if (rep) len += swprintf( out + len, size - len, L"%s", rep );
        else out[len++] = *in;
    }
    out[len] = 0;
}

static INT_PTR CALLBACK key_dialog_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    static WCHAR *key;

    switch (msg)
    {
    case WM_INITDIALOG:
        key = (WCHAR *)lp;
        CheckDlgButton( dlg, IDC_HIDE_KEY, BST_CHECKED );
        SendDlgItemMessageW( dlg, IDC_KEY, EM_LIMITTEXT, 63, 0 );
        SetForegroundWindow( dlg );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_HIDE_KEY:
            SendDlgItemMessageW( dlg, IDC_KEY, EM_SETPASSWORDCHAR,
                                 IsDlgButtonChecked( dlg, IDC_HIDE_KEY ) ? 0x25cf : 0, 0 );
            InvalidateRect( GetDlgItem( dlg, IDC_KEY ), NULL, TRUE );
            return TRUE;
        case IDOK:
            GetDlgItemTextW( dlg, IDC_KEY, key, 64 );
            /* WPA takes 8 to 63 characters */
            if (wcslen( key ) < 8) return TRUE;
            EndDialog( dlg, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void connect_entry( UINT index )
{
    struct entry *entry = &entries[index];
    WLAN_CONNECTION_PARAMETERS params = { .dot11BssType = dot11_BSS_type_infrastructure };
    WCHAR key[64] = L"", name[128], material[160], xml[2048];
    DWORD reason, ret;

    if (entry->connected)
    {
        WlanDisconnect( wlan, &wifi_guid, NULL );
        flyout_refresh();
        return;
    }
    if (entry->has_profile && entry->profile[0])
    {
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = entry->profile;
    }
    else if (entry->has_profile)
    {
        /* iwd knows it, the registry does not */
        params.wlanConnectionMode = wlan_connection_mode_discovery_secure;
        params.pDot11Ssid = &entry->ssid;
    }
    else
    {
        INT_PTR ok = IDOK;

        if (entry->secured)
        {
            asking_key = TRUE;
            ok = DialogBoxParamW( pnidui_instance, MAKEINTRESOURCEW(IDD_NETWORK_KEY), flyout, key_dialog_proc,
                                  (LPARAM)key );
            asking_key = FALSE;
        }
        if (ok != IDOK) return;
        entry = &entries[index];
        xml_escape( entry->name, name, ARRAY_SIZE(name) );
        xml_escape( key, material, ARRAY_SIZE(material) );
        swprintf( xml, ARRAY_SIZE(xml),
                  L"<?xml version=\"1.0\"?>\n"
                  L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\n"
                  L"<name>%s</name>\n<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>\n"
                  L"<connectionType>ESS</connectionType>\n<connectionMode>%s</connectionMode>\n"
                  L"<MSM><security><authEncryption><authentication>%s</authentication>"
                  L"<encryption>%s</encryption><useOneX>false</useOneX></authEncryption>%s%s%s</security></MSM>\n"
                  L"</WLANProfile>\n",
                  name, name, autoconnect ? L"auto" : L"manual", entry->secured ? L"WPA2PSK" : L"open", entry->secured ? L"AES" : L"none",
                  entry->secured ? L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" : L"",
                  entry->secured ? material : L"", entry->secured ? L"</keyMaterial></sharedKey>" : L"" );
        SecureZeroMemory( key, sizeof(key) );
        SecureZeroMemory( material, sizeof(material) );
        ret = WlanSetProfile( wlan, &wifi_guid, 0, xml, NULL, TRUE, NULL, &reason );
        SecureZeroMemory( xml, sizeof(xml) );
        if (ret) return;
        lstrcpynW( entry->profile, entry->name, ARRAY_SIZE(entry->profile) );
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = entry->profile;
    }
    lstrcpynW( connecting_ssid, entry->name, 64 );
    if (WlanConnect( wlan, &wifi_guid, &params, NULL )) connecting_ssid[0] = 0;
    flyout_refresh();
}

static void set_radio( BOOL on )
{
    WLAN_PHY_RADIO_STATE state = { 0, on ? dot11_radio_state_on : dot11_radio_state_off, dot11_radio_state_on };

    if (have_wifi) WlanSetInterface( wlan, &wifi_guid, wlan_intf_opcode_radio_state, sizeof(state), &state, NULL );
}

static void set_airplane( BOOL on )
{
    DWORD value = on;
    HKEY key;

    if (!RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"AirplaneMode", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
        RegCloseKey( key );
    }
    set_radio( !on );
}

static void activate( enum hit what, int index )
{
    switch (what)
    {
    case HIT_ENTRY:
        if (entries[index].kind != ENTRY_WIRELESS) break;
        selected = selected == index ? -1 : index;
        flyout_layout( FALSE );
        break;
    case HIT_BUTTON:
        if (!entries[index].connecting) connect_entry( index );
        break;
    case HIT_CHECK:
        autoconnect = !autoconnect;
        InvalidateRect( flyout, NULL, FALSE );
        break;
    case HIT_LINK:
        ShowWindow( flyout, SW_HIDE );
        open_connections();
        break;
    case HIT_TILE:
        if (index == TILE_WIFI)
        {
            if (airplane && !radio_on) set_airplane( FALSE );
            else set_radio( !radio_on );
        }
        else if (index == TILE_AIRPLANE) set_airplane( !airplane );
        /* iwd brings the device up or down in a moment */
        Sleep( 300 );
        flyout_refresh();
        break;
    default:
        break;
    }
}

/**********************************************************************
 *          The window
 */

static void set_hot( enum hit what, int index )
{
    if (what == hot && index == hot_index) return;
    hot = what;
    hot_index = index;
    InvalidateRect( flyout, NULL, FALSE );
}

static LRESULT WINAPI flyout_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
    enum hit what;
    int index;

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (hot == HIT_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent( &tme );
        }
        what = hit_test( pt, &index );
        set_hot( what, index );
        return 0;
    case WM_MOUSELEAVE:
        hot = pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        pressed = hit_test( pt, &pressed_index );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        what = hit_test( pt, &index );
        if (what != HIT_NONE && what == pressed && index == pressed_index)
        {
            pressed = HIT_NONE;
            activate( what, index );
        }
        pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDBLCLK:
        what = hit_test( pt, &index );
        if (what == HIT_ENTRY && entries[index].kind == ENTRY_WIRELESS && !entries[index].connected)
        {
            selected = index;
            connect_entry( index );
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        int max_scroll = max( 0, content_height() - list_height() );
        scroll = max( 0, min( scroll - (short)HIWORD( wp ) * px( 48 ) / WHEEL_DELTA, max_scroll ) );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow( hwnd, SW_HIDE );
        return 0;
    case WM_ACTIVATE:
        /* a click anywhere else closes it, as in Windows; not the key dialog it opened */
        if (LOWORD( wp ) == WA_INACTIVE && (HWND)lp != NULL && GetWindow( (HWND)lp, GW_OWNER ) == hwnd) break;
        if (LOWORD( wp ) == WA_INACTIVE && !asking_key)
        {
            ShowWindow( hwnd, SW_HIDE );
            hidden_at = GetTickCount();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void shell_look(void)
{
    static void (WINAPI *apply)( HWND );
    static BOOL looked;

    if (!looked)
    {
        HMODULE module = LoadLibraryW( L"startui.dll" );
        if (module) apply = (void *)GetProcAddress( module, "ArcticApplyShellLook" );
        looked = TRUE;
    }
    if (apply) apply( flyout );
    else
    {
        struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, 0xcc1f1f1f, 0 };
        struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
        SetWindowCompositionAttribute( flyout, &attr );
    }
}

static BOOL create_flyout(void)
{
    WNDCLASSW cls = { .style = CS_DBLCLKS, .lpfnWndProc = flyout_proc, .hInstance = pnidui_instance,
                      .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ), .lpszClassName = L"PniduiFlyout" };
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP;

    if (flyout) return TRUE;
    RegisterClassW( &cls );
    flyout = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, NULL, WS_POPUP,
                              0, 0, FLYOUT_WIDTH, px( 200 ), NULL, NULL, pnidui_instance, NULL );
    if (!flyout) return FALSE;
    DwmSetWindowAttribute( flyout, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    /* the body text of XAML is 15, Caption 12 */
    font_name = make_font( px( 15 ), FW_NORMAL );
    font_tile = make_font( px( 12 ), FW_NORMAL );
    {
        HDC hdc = GetDC( flyout );
        TEXTMETRICW metrics;
        HGDIOBJ old = SelectObject( hdc, font_name );

        GetTextMetricsW( hdc, &metrics );
        line15 = metrics.tmHeight;
        SelectObject( hdc, font_tile );
        GetTextMetricsW( hdc, &metrics );
        line12 = metrics.tmHeight;
        SelectObject( hdc, old );
        ReleaseDC( flyout, hdc );
    }
    return TRUE;
}

/* the entries sized again; reposition: over the icon, on the taskbar's edge */
void flyout_layout( BOOL reposition )
{
    RECT work, rect;
    int height;

    layout_entries();
    height = flyout_height();

    scroll = max( 0, min( scroll, content_height() - list_height() ) );
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    GetWindowRect( flyout, &rect );
    if (reposition)
    {
        rect.right = work.right;
        rect.bottom = work.bottom;
    }
    SetWindowPos( flyout, HWND_TOPMOST, rect.right - FLYOUT_WIDTH, rect.bottom - height, FLYOUT_WIDTH, height,
                  SWP_NOACTIVATE );
    InvalidateRect( flyout, NULL, FALSE );
}

BOOL flyout_visible(void)
{
    return flyout && IsWindowVisible( flyout );
}

void flyout_refresh(void)
{
    WCHAR keep[64] = L"";

    if (!flyout_visible() || asking_key) return;
    /* the network picked stays picked when the list changes around it */
    if (selected >= 0 && selected < (int)entry_count) lstrcpyW( keep, entries[selected].name );
    fill_entries();
    selected = -1;
    for (UINT i = 0; keep[0] && i < entry_count && selected < 0; i++)
        if (entries[i].kind == ENTRY_WIRELESS && !wcscmp( entries[i].name, keep )) selected = i;
    flyout_layout( FALSE );
}

void flyout_toggle(void)
{
    if (!create_flyout()) return;
    if (flyout_visible())
    {
        ShowWindow( flyout, SW_HIDE );
        return;
    }
    /* the click on the icon that took the focus away and closed it */
    if (GetTickCount() - hidden_at < 300) return;
    if (have_wifi) WlanScan( wlan, &wifi_guid, NULL, NULL, NULL );
    selected = -1;
    scroll = 0;
    hot = pressed = HIT_NONE;
    fill_entries();
    shell_look();
    flyout_layout( TRUE );
    ShowWindow( flyout, SW_SHOW );
    SetForegroundWindow( flyout );
    SetFocus( flyout );
}

void flyout_hide(void)
{
    if (flyout_visible()) ShowWindow( flyout, SW_HIDE );
}

void flyout_destroy(void)
{
    if (flyout) DestroyWindow( flyout );
    flyout = NULL;
}
