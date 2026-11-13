/*
 * The taskbar's buttons: how they look, what a right click on one shows,
 * the picture of the Start button, and the page "Кнопки" that chooses these
 *
 * Explorer draws the buttons of Windows 10 with the theme. Those of Windows
 * 11 are drawn here: a rounded rectangle, 40 by 40 in a slot of 44 by 48,
 * under the pointer and behind the active window, and a pill along its
 * bottom, 6 wide for a running program and 16 in the accent colour for the
 * active one. HKCU\Software\Arctic\Taskbar: ButtonStyle (0 Windows 10, 1
 * Windows 11), Alignment (0 left, 1 centre), StartImage (a file, or empty).
 *
 * A picture for the Start button is drawn as it is, no larger than the
 * button is high; as for Classic Shell, one taller than wide may hold the
 * normal, hot and pressed states, stacked.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "winver.h"
#include "commctrl.h"
#include "commdlg.h"
#include "shellapi.h"
#include "objbase.h"
#include "wincodec.h"

#define REG_KEY L"Software\\Arctic\\Taskbar"

/* widgets.c: 0 off, 1 the icon, 2 the icon and the text */
DWORD ArcticWidgetShow(void);
void ArcticWidgetSetShow( DWORD show );

/* in no import library, and initguid.h would define all that wincodec.h brings */
const GUID wic_factory_clsid = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
const GUID wic_factory_iid = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
const GUID wic_format_pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

/* as explorer passes them */
#define BUTTON_HOT       1
#define BUTTON_PRESSED   2
#define BUTTON_ACTIVE    4
#define BUTTON_ATTENTION 8
#define BUTTON_START     16

#define ID_BUTTON_STYLE  1500
#define ID_ALIGNMENT     1501
#define ID_WIDGET        1510
#define ID_START_KIND    1502
#define ID_START_PATH    1503
#define ID_START_BROWSE  1504

BOOL WINAPI ArcticTaskbarIsLight(void);

/* ButtonStyle and Alignment: as Windows 11 has them until the user says otherwise */
static DWORD read_option( const WCHAR *name )
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size );
    return value;
}

BOOL taskbar_centered(void)
{
    return read_option( L"Alignment" ) == 1;
}

BOOL taskbar_style11(void)
{
    return read_option( L"ButtonStyle" ) == 1;
}

/* a ring: the rounded rectangle less the one a pixel inside it */
static void fill_round_ring( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    enum { SAMPLES = 4 };
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    float inner = radius > 1.f ? radius - 1.f : 0.f;
    UINT32 *pixels;

    if (width <= 2 || height <= 2 || !(pixels = malloc( width * height * sizeof(*pixels) ))) return;
    for (int row = 0; row < height; row++)
    {
        for (int col = 0; col < width; col++)
        {
            int hits = 0;
            UINT32 a;

            for (int j = 0; j < SAMPLES; j++)
            {
                for (int i = 0; i < SAMPLES; i++)
                {
                    float x = col + (i + 0.5f) / SAMPLES, y = row + (j + 0.5f) / SAMPLES;
                    float dx = max( max( radius - x, x - (width - radius) ), 0.f );
                    float dy = max( max( radius - y, y - (height - radius) ), 0.f );
                    float ix = max( max( radius - x, x - (width - radius) ), 0.f );
                    float iy = max( max( radius - y, y - (height - radius) ), 0.f );
                    BOOL in = dx * dx + dy * dy <= radius * radius;
                    BOOL within = x >= 1 && y >= 1 && x <= width - 1 && y <= height - 1 &&
                                  ix * ix + iy * iy <= inner * inner;

                    if (in && !within) hits++;
                }
            }
            a = hits * alpha / (SAMPLES * SAMPLES);
            pixels[row * width + col] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                        ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    blend_bits( hdc, rect->left, rect->top, width, height, pixels );
    free( pixels );
}

/* the accent as Windows 11 shows it on the taskbar: lighter over a dark
 * one, darker over a light one */
static COLORREF accent_for_taskbar( BOOL light )
{
    COLORREF accent = accent_color();
    int r = GetRValue( accent ), g = GetGValue( accent ), b = GetBValue( accent );

    if (light) return RGB( r * 85 / 100, g * 85 / 100, b * 85 / 100 );
    return RGB( r + (255 - r) * 45 / 100, g + (255 - g) * 45 / 100, b + (255 - b) * 45 / 100 );
}

/* A button of Windows 11 */
void WINAPI ArcticTaskbarPaintButton( HDC hdc, const RECT *button, DWORD flags )
{
    BOOL light = ArcticTaskbarIsLight();
    BOOL hot = flags & BUTTON_HOT, pressed = flags & BUTTON_PRESSED, active = flags & BUTTON_ACTIVE;
    BOOL attention = flags & BUTTON_ATTENTION;
    COLORREF ink = light ? RGB( 0, 0, 0 ) : RGB( 255, 255, 255 );
    COLORREF critical = light ? RGB( 0xc4, 0x2b, 0x1c ) : RGB( 0xff, 0x99, 0xa4 );
    int width = button->right - button->left, height = button->bottom - button->top;
    int side = min( width - 2 * px( 2 ), px( 40 ) ), high = min( height, px( 40 ) );
    float radius = (float)px( 4 );
    RECT back;

    if (flags & BUTTON_START) active = attention = FALSE;
    /* a wider button, with a picture, keeps its width */
    if (width > px( 44 )) side = width - 2 * px( 2 );
    SetRect( &back, button->left + (width - side) / 2, button->top + (height - high) / 2, 0, 0 );
    back.right = back.left + side;
    back.bottom = back.top + high;

    if (attention) fill_round_rect( hdc, &back, radius, critical, hot ? 0x38 : 0x28 );
    else if (active && light)
    {
        fill_round_rect( hdc, &back, radius, RGB( 255, 255, 255 ), hot || pressed ? 0x80 : 0xb3 );
        fill_round_ring( hdc, &back, radius, ink, 0x0f );
    }
    else if (active)
    {
        fill_round_rect( hdc, &back, radius, ink, pressed ? 0x0c : hot ? 0x1c : 0x12 );
        fill_round_ring( hdc, &back, radius, ink, 0x10 );
    }
    else if (pressed) fill_round_rect( hdc, &back, radius, ink, light ? 0x06 : 0x0c );
    else if (hot) fill_round_rect( hdc, &back, radius, ink, light ? 0x09 : 0x15 );

    if (!(flags & BUTTON_START))
    {
        int long_pill = active || attention, w = px( long_pill ? 16 : 6 ), h = px( 3 );
        RECT pill;

        /* with the program's name shown the button is wide, and the pill goes with it */
        if (side > px( 60 )) w = long_pill ? side - px( 12 ) : side * 3 / 4;
        pill.left = (back.left + back.right - w) / 2;
        pill.right = pill.left + w;
        pill.bottom = back.bottom - px( 1 );
        pill.top = pill.bottom - h;
        if (attention) fill_round_rect( hdc, &pill, h / 2.f, critical, 255 );
        else if (active) fill_round_rect( hdc, &pill, h / 2.f, accent_for_taskbar( light ), 255 );
        else fill_round_rect( hdc, &pill, h / 2.f, ink, light ? 0x73 : 0x8a );
    }
}

/* The picture of the Start button */

static struct
{
    WCHAR path[MAX_PATH];
    int height;           /* of the button it was made for */
    int width, frame;     /* of a state */
    int frames;
    UINT32 *pixels;       /* premultiplied, the states one under the other */
} start_image;

/* made smaller, each pixel the average of what it covers */
static void shrink_pixels( const UINT32 *from, int from_w, int from_h, UINT32 *to, int to_w, int to_h )
{
    for (int y = 0; y < to_h; y++)
    {
        int y0 = y * from_h / to_h, y1 = max( (y + 1) * from_h / to_h, y0 + 1 );

        for (int x = 0; x < to_w; x++)
        {
            int x0 = x * from_w / to_w, x1 = max( (x + 1) * from_w / to_w, x0 + 1 );
            unsigned int a = 0, r = 0, g = 0, b = 0, n = (x1 - x0) * (y1 - y0);

            for (int j = y0; j < y1; j++)
            {
                for (int i = x0; i < x1; i++)
                {
                    UINT32 p = from[j * from_w + i];

                    a += p >> 24;
                    r += (p >> 16) & 0xff;
                    g += (p >> 8) & 0xff;
                    b += p & 0xff;
                }
            }
            to[y * to_w + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

static BOOL decode_start_image( const WCHAR *path, int button_height )
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT width = 0, height = 0;
    UINT32 *pixels = NULL;
    BOOL done = FALSE;
    HRESULT init = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    HRESULT hr;

    hr = CoCreateInstance( &wic_factory_clsid, NULL, CLSCTX_INPROC_SERVER, &wic_factory_iid,
                           (void **)&factory );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                                          WICDecodeMetadataCacheOnDemand, &decoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame( decoder, 0, &frame );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter( factory, &converter );
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)frame,
                                                            &wic_format_pbgra, WICBitmapDitherTypeNone,
                                                            NULL, 0.0, WICBitmapPaletteTypeCustom );
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize( converter, &width, &height );
    if (SUCCEEDED(hr) && width && height && width <= 4096 && height <= 4096 &&
        (pixels = malloc( width * height * sizeof(*pixels) )))
        hr = IWICFormatConverter_CopyPixels( converter, NULL, width * sizeof(*pixels),
                                             width * height * sizeof(*pixels), (BYTE *)pixels );
    if (SUCCEEDED(hr) && pixels)
    {
        int frames = height > width && !(height % 3) ? 3 : height >= 2 * width && !(height % 2) ? 2 : 1;
        int frame_height = height / frames, to_h = min( frame_height, button_height );
        int to_w = max( (int)width * to_h / frame_height, 1 );
        UINT32 *scaled = malloc( to_w * to_h * frames * sizeof(*scaled) );

        if (scaled)
        {
            for (int i = 0; i < frames; i++)
                shrink_pixels( pixels + i * frame_height * width, width, frame_height,
                               scaled + i * to_h * to_w, to_w, to_h );
            free( start_image.pixels );
            start_image.pixels = scaled;
            start_image.width = to_w;
            start_image.frame = to_h;
            start_image.frames = frames;
            done = TRUE;
        }
    }
    free( pixels );
    if (converter) IWICFormatConverter_Release( converter );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (factory) IWICImagingFactory_Release( factory );
    if (SUCCEEDED(init)) CoUninitialize();
    return done;
}

/* what the registry says now, read once per button height and file */
static BOOL load_start_image( int height )
{
    WCHAR path[MAX_PATH] = L"";
    DWORD size = sizeof(path);

    if (height <= 0) return FALSE;
    RegGetValueW( HKEY_CURRENT_USER, REG_KEY, L"StartImage", RRF_RT_REG_SZ, NULL, path, &size );
    if (!path[0])
    {
        free( start_image.pixels );
        memset( &start_image, 0, sizeof(start_image) );
        return FALSE;
    }
    if (height == start_image.height && !lstrcmpiW( path, start_image.path )) return start_image.pixels != NULL;

    free( start_image.pixels );
    memset( &start_image, 0, sizeof(start_image) );
    lstrcpynW( start_image.path, path, ARRAY_SIZE(start_image.path) );
    start_image.height = height;
    return decode_start_image( path, height );
}

BOOL WINAPI ArcticStartButtonImageSize( int height, SIZE *size )
{
    if (!load_start_image( height )) return FALSE;
    size->cx = start_image.width;
    size->cy = start_image.frame;
    return TRUE;
}

BOOL WINAPI ArcticStartButtonImagePaint( HDC hdc, const RECT *button, DWORD flags )
{
    int state = 0, x, y;

    if (!load_start_image( button->bottom - button->top )) return FALSE;

    /* a single picture is lit as the button is */
    if (start_image.frames == 1)
    {
        if (read_option( L"ButtonStyle" ) == 1) ArcticTaskbarPaintButton( hdc, button, flags | BUTTON_START );
        else if (flags & (BUTTON_HOT | BUTTON_PRESSED))
            fill_alpha( hdc, button, ArcticTaskbarIsLight() ? RGB( 0, 0, 0 ) : RGB( 255, 255, 255 ),
                        flags & BUTTON_PRESSED ? 0x26 : 0x1a );
    }
    else if (flags & BUTTON_PRESSED) state = start_image.frames - 1;
    else if (flags & BUTTON_HOT) state = 1;

    x = (button->left + button->right - start_image.width) / 2;
    y = (button->top + button->bottom - start_image.frame) / 2;
    blend_bits( hdc, x, y, start_image.width, start_image.frame,
                start_image.pixels + state * start_image.frame * start_image.width );
    return TRUE;
}

/* What a right click on a button shows */

/* the name a program gives itself, or that of its file */
void program_name( const WCHAR *path, WCHAR *name, int size )
{
    DWORD handle, length = GetFileVersionInfoSizeW( path, &handle );
    const WCHAR *file = wcsrchr( path, '\\' );
    BYTE *data;
    WCHAR *dot;

    lstrcpynW( name, file ? file + 1 : path, size );
    if ((dot = wcsrchr( name, '.' ))) *dot = 0;
    if (!length || !(data = malloc( length ))) return;
    if (GetFileVersionInfoW( path, 0, length, data ))
    {
        struct { WORD language, codepage; } *translate;
        UINT bytes;
        WCHAR query[64], *text;

        if (VerQueryValueW( data, L"\\VarFileInfo\\Translation", (void **)&translate, &bytes ) && bytes >= sizeof(*translate))
        {
            swprintf( query, ARRAY_SIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                      translate->language, translate->codepage );
            if (VerQueryValueW( data, query, (void **)&text, &bytes ) && bytes > 1 && text[0])
                lstrcpynW( name, text, size );
        }
    }
    free( data );
}

BOOL WINAPI ArcticTaskbarJumpList( HWND owner, const HWND *windows, UINT count, const RECT *button )
{
    struct popup_item items[3];
    struct look look;
    MONITORINFO info = { sizeof(info) };
    WCHAR path[MAX_PATH] = L"", name[MAX_PATH] = L"", link[MAX_PATH] = L"";
    HWND previous = GetForegroundWindow(), root = GetAncestor( owner, GA_ROOT );
    BOOL pinned = FALSE;
    HBITMAP icon = NULL;
    DWORD pid = 0, size = ARRAY_SIZE(path);
    HANDLE process;
    int style = read_option( L"ButtonStyle" ) == 1 ? POPUP_WIN11 : POPUP_WIN10;
    UINT n = 0, align = TPM_CENTERALIGN, chosen;
    POINT pt;

    if (!count) return FALSE;
    GetWindowThreadProcessId( windows[0], &pid );
    if ((process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
    {
        if (!QueryFullProcessImageNameW( process, 0, path, &size )) path[0] = 0;
        CloseHandle( process );
    }
    if (path[0])
    {
        SHFILEINFOW file = { 0 };

        program_name( path, name, ARRAY_SIZE(name) );
        if (SHGetFileInfoW( path, 0, &file, sizeof(file), SHGFI_ICON | SHGFI_SMALLICON ) && file.hIcon)
        {
            icon = icon_bitmap( file.hIcon, px( 16 ) );
            DestroyIcon( file.hIcon );
        }
        items[n++] = (struct popup_item){ .id = 1, .text = name, .glyph = GLYPH_NONE, .icon = icon };
        /* as Windows 11's: pin the program, or unpin it */
        pinned = taskbar_pin_of( path, link );
        items[n++] = (struct popup_item){ .id = 3, .text = load_string( pinned ? IDS_UNPIN_TASKBAR : IDS_PIN_TASKBAR ),
                                          .glyph = pinned ? GLYPH_UNPIN : GLYPH_PIN };
    }
    items[n++] = (struct popup_item){ .id = 2, .text = load_string( count > 1 ? IDS_CLOSE_ALL_WINDOWS : IDS_CLOSE_WINDOW ),
                                      .glyph = GLYPH_CLOSE };

    /* over the button, against the taskbar */
    GetMonitorInfoW( MonitorFromRect( button, MONITOR_DEFAULTTONEAREST ), &info );
    pt.x = (button->left + button->right) / 2;
    if ((button->top + button->bottom) / 2 > (info.rcMonitor.top + info.rcMonitor.bottom) / 2)
    {
        pt.y = button->top - (style == POPUP_WIN11 ? px( 8 ) : 0);
        align |= TPM_BOTTOMALIGN;
    }
    else pt.y = button->bottom + (style == POPUP_WIN11 ? px( 8 ) : 0);

    look_load_taskbar( &look );
    chosen = popup_track( root, pt, align, items, n, style, &look );
    if (icon) DeleteObject( icon );

    if (previous && previous != root && IsWindow( previous )) SetForegroundWindow( previous );
    if (chosen == 1) ShellExecuteW( NULL, NULL, path, NULL, NULL, SW_SHOWNORMAL );
    else if (chosen == 2)
        for (UINT i = 0; i < count; i++) PostMessageW( windows[i], WM_SYSCOMMAND, SC_CLOSE, 0 );
    else if (chosen == 3 && pinned) ArcticTaskbarUnpinFile( link );
    else if (chosen == 3) taskbar_pin_named( path, NULL );
    return TRUE;
}

/* The taskbar's own menu: a menu of USER's, its items as they are, drawn as
 * the menus of the style chosen */

#define MENU_MAX 40

static struct popup_item menu_items[MENU_MAX];
static WCHAR menu_texts[MENU_MAX][128];
static UINT menu_used;

/* without the & of its access key and what follows a tab */
static void plain_text( WCHAR *to, const WCHAR *from, int size )
{
    int n = 0;

    for (; *from && *from != '\t' && n < size - 1; from++)
    {
        if (*from == '&' && *++from == 0) break;
        to[n++] = *from;
    }
    to[n] = 0;
}

static const struct popup_item *convert_menu( HMENU menu, UINT *count, int depth )
{
    int total = GetMenuItemCount( menu ), n = 0;
    struct popup_item *block;

    *count = 0;
    if (total <= 0 || menu_used + total > MENU_MAX) return NULL;
    block = &menu_items[menu_used];
    menu_used += total;
    for (int i = 0; i < total; i++)
    {
        MENUITEMINFOW info = { sizeof(info) };
        struct popup_item *item = &block[n];
        WCHAR text[128] = L"";
        int index = item - menu_items;

        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = text;
        info.cch = ARRAY_SIZE(text);
        if (!GetMenuItemInfoW( menu, i, TRUE, &info )) continue;
        memset( item, 0, sizeof(*item) );
        item->glyph = GLYPH_NONE;
        if (info.fType & MFT_SEPARATOR)
        {
            /* never first, nor two in a row */
            if (n && (block[n - 1].id || block[n - 1].sub)) n++;
            continue;
        }
        if (!text[0]) continue;
        plain_text( menu_texts[index], text, ARRAY_SIZE(menu_texts[index]) );
        item->text = menu_texts[index];
        item->id = info.wID;
        item->checked = (info.fState & MFS_CHECKED) != 0;
        item->disabled = (info.fState & MFS_DISABLED) != 0;
        if (info.hSubMenu && depth < 2) item->sub = convert_menu( info.hSubMenu, &item->sub_count, depth + 1 );
        if (!item->id && !item->sub) continue;
        n++;
    }
    while (n && !block[n - 1].id && !block[n - 1].sub) n--;
    *count = n;
    return n ? block : NULL;
}

BOOL WINAPI ArcticTrackTaskbarMenu( HMENU menu, HWND owner, const POINT *where, UINT *command )
{
    int style = read_option( L"ButtonStyle" ) == 1 ? POPUP_WIN11 : POPUP_WIN10;
    int gap = style == POPUP_WIN11 ? px( 8 ) : 0;
    MONITORINFO info = { sizeof(info) };
    const struct popup_item *items;
    UINT count, align = TPM_LEFTALIGN;
    struct look look;
    POINT pt = *where;

    menu_used = 0;
    if (!(items = convert_menu( menu, &count, 0 ))) return FALSE;

    /* against the taskbar, whichever edge it is on */
    GetMonitorInfoW( MonitorFromPoint( pt, MONITOR_DEFAULTTONEAREST ), &info );
    if (pt.y >= info.rcWork.bottom)
    {
        pt.y = info.rcWork.bottom - gap;
        align |= TPM_BOTTOMALIGN;
    }
    else if (pt.y < info.rcWork.top) pt.y = info.rcWork.top + gap;
    else align |= TPM_BOTTOMALIGN;

    look_load_taskbar( &look );
    *command = popup_track( owner, pt, align, items, count, style, &look );
    return TRUE;
}

/* The page */

struct page
{
    HWND tray;
    BOOL filling;
};

static void show_start_kind( HWND dialog )
{
    BOOL image = SendDlgItemMessageW( dialog, ID_START_KIND, CB_GETCURSEL, 0, 0 ) == 1;

    EnableWindow( GetDlgItem( dialog, ID_START_PATH ), image );
    EnableWindow( GetDlgItem( dialog, ID_START_BROWSE ), image );
}

static void add_combo( HWND dialog, int y, UINT label, int id, const UINT *names, int count )
{
    HWND combo;

    add_dialog_control( dialog, L"STATIC", load_string( label ), SS_CENTERIMAGE, 7, y, 66, 14, -1 );
    combo = add_dialog_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, y, 169, 80, id );
    for (int i = 0; i < count; i++) SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( names[i] ) );
}

static void init_page( HWND dialog, struct page *page )
{
    static const UINT styles[] = { IDS_TASKBAR_WIN10, IDS_TASKBAR_WIN11 };
    static const UINT alignments[] = { IDS_ALIGN_LEFT, IDS_ALIGN_CENTER };
    static const UINT kinds[] = { IDS_START_STANDARD, IDS_START_IMAGE };
    static const UINT widget_modes[] = { IDS_WIDGET_BOTH, IDS_WIDGET_ICON, IDS_WIDGET_OFF };
    WCHAR path[MAX_PATH] = L"";
    DWORD size = sizeof(path);

    add_combo( dialog, 7, IDS_TASK_BUTTONS, ID_BUTTON_STYLE, styles, ARRAY_SIZE(styles) );
    add_combo( dialog, 27, IDS_ALIGNMENT, ID_ALIGNMENT, alignments, ARRAY_SIZE(alignments) );
    add_combo( dialog, 47, IDS_START_BUTTON, ID_START_KIND, kinds, ARRAY_SIZE(kinds) );
    add_dialog_control( dialog, L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 76, 67, 116, 14, ID_START_PATH );
    add_dialog_control( dialog, L"BUTTON", load_string( IDS_BROWSE ), BS_PUSHBUTTON | WS_TABSTOP, 195, 67, 50, 14, ID_START_BROWSE );
    add_combo( dialog, 87, IDS_WIDGET_SHOW, ID_WIDGET, widget_modes, ARRAY_SIZE(widget_modes) );

    RegGetValueW( HKEY_CURRENT_USER, REG_KEY, L"StartImage", RRF_RT_REG_SZ, NULL, path, &size );
    page->filling = TRUE;
    SendDlgItemMessageW( dialog, ID_BUTTON_STYLE, CB_SETCURSEL, read_option( L"ButtonStyle" ) == 1, 0 );
    SendDlgItemMessageW( dialog, ID_ALIGNMENT, CB_SETCURSEL, read_option( L"Alignment" ) == 1, 0 );
    SendDlgItemMessageW( dialog, ID_START_KIND, CB_SETCURSEL, path[0] != 0, 0 );
    SendDlgItemMessageW( dialog, ID_WIDGET, CB_SETCURSEL, 2 - min( ArcticWidgetShow(), 2 ), 0 );
    SetDlgItemTextW( dialog, ID_START_PATH, path );
    show_start_kind( dialog );
    page->filling = FALSE;
}

static void browse_image( HWND dialog, struct page *page )
{
    WCHAR path[MAX_PATH] = L"", filter[128];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    int len = lstrlenW( lstrcpynW( filter, load_string( IDS_IMAGES ), 64 ) );

    /* name, NUL, patterns, NUL, NUL */
    lstrcpyW( filter + len + 1, L"*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.ico" );
    filter[len + 1 + lstrlenW( filter + len + 1 ) + 1] = 0;
    GetDlgItemTextW( dialog, ID_START_PATH, path, ARRAY_SIZE(path) );
    ofn.hwndOwner = dialog;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAY_SIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW( &ofn )) return;
    SetDlgItemTextW( dialog, ID_START_PATH, path );
    SendMessageW( GetParent( dialog ), PSM_CHANGED, (WPARAM)dialog, 0 );
}

static void apply_page( HWND dialog, struct page *page )
{
    DWORD style = SendDlgItemMessageW( dialog, ID_BUTTON_STYLE, CB_GETCURSEL, 0, 0 ) == 1;
    DWORD alignment = SendDlgItemMessageW( dialog, ID_ALIGNMENT, CB_GETCURSEL, 0, 0 ) == 1;
    WCHAR path[MAX_PATH] = L"";
    HKEY key;

    if (SendDlgItemMessageW( dialog, ID_START_KIND, CB_GETCURSEL, 0, 0 ) == 1)
        GetDlgItemTextW( dialog, ID_START_PATH, path, ARRAY_SIZE(path) );
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"ButtonStyle", 0, REG_DWORD, (const BYTE *)&style, sizeof(style) );
        RegSetValueExW( key, L"Alignment", 0, REG_DWORD, (const BYTE *)&alignment, sizeof(alignment) );
        RegSetValueExW( key, L"StartImage", 0, REG_SZ, (const BYTE *)path, (lstrlenW( path ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    ArcticWidgetSetShow( 2 - SendDlgItemMessageW( dialog, ID_WIDGET, CB_GETCURSEL, 0, 0 ) );
    if (page->tray) SendMessageW( page->tray, RegisterWindowMessageW( L"ArcticTaskbarOptionsChanged" ), 0, 0 );
    /* the icons of the notification area and the flyouts follow the buttons' style */
    PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
}

static INT_PTR WINAPI page_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct page *page = (struct page *)GetWindowLongPtrW( dialog, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        page = (struct page *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW( dialog, DWLP_USER, (LONG_PTR)page );
        init_page( dialog, page );
        return TRUE;

    case WM_COMMAND:
        if (!page || page->filling) break;
        if (LOWORD( wparam ) == ID_START_BROWSE && HIWORD( wparam ) == BN_CLICKED)
        {
            browse_image( dialog, page );
            return TRUE;
        }
        if (HIWORD( wparam ) == CBN_SELCHANGE &&
            (LOWORD( wparam ) == ID_BUTTON_STYLE || LOWORD( wparam ) == ID_ALIGNMENT || LOWORD( wparam ) == ID_START_KIND ||
             LOWORD( wparam ) == ID_WIDGET))
        {
            show_start_kind( dialog );
            SendMessageW( GetParent( dialog ), PSM_CHANGED, (WPARAM)dialog, 0 );
            return TRUE;
        }
        break;

    case WM_NOTIFY:
        if (page && ((NMHDR *)lparam)->code == PSN_APPLY)
        {
            apply_page( dialog, page );
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static UINT CALLBACK page_callback( HWND hwnd, UINT msg, PROPSHEETPAGEW *psp )
{
    if (msg == PSPCB_RELEASE) free( (struct page *)psp->lParam );
    return 1;
}

/* For the Control Panel's page (taskbarcpl.dll): [0] the buttons of Windows
 * 11, [1] in the middle, [2] the widgets on the taskbar (0 none, 1 the icon,
 * 2 the weather too); the Start button's picture, "" for the standard one */
void WINAPI ArcticTaskbarButtonsGet( DWORD *values, WCHAR *image, UINT count )
{
    DWORD size = count * sizeof(WCHAR);

    values[0] = read_option( L"ButtonStyle" ) == 1;
    values[1] = read_option( L"Alignment" ) == 1;
    values[2] = min( ArcticWidgetShow(), 2 );
    if (!image || !count) return;
    image[0] = 0;
    RegGetValueW( HKEY_CURRENT_USER, REG_KEY, L"StartImage", RRF_RT_REG_SZ, NULL, image, &size );
}

void WINAPI ArcticTaskbarButtonsSet( HWND tray, const DWORD *values, const WCHAR *image )
{
    DWORD style = values[0] != 0, alignment = values[1] != 0;
    HKEY key;

    if (!image) image = L"";
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"ButtonStyle", 0, REG_DWORD, (const BYTE *)&style, sizeof(style) );
        RegSetValueExW( key, L"Alignment", 0, REG_DWORD, (const BYTE *)&alignment, sizeof(alignment) );
        RegSetValueExW( key, L"StartImage", 0, REG_SZ, (const BYTE *)image, (lstrlenW( image ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    if (values[2] != min( ArcticWidgetShow(), 2 )) ArcticWidgetSetShow( min( values[2], 2 ) );
    if (tray) SendMessageW( tray, RegisterWindowMessageW( L"ArcticTaskbarOptionsChanged" ), 0, 0 );
    PostMessageW( HWND_BROADCAST, RegisterWindowMessageW( L"ArcticShellLookChanged" ), 0, 0 );
}

/* the page "Кнопки" of the taskbar's properties, for explorer to add */
HPROPSHEETPAGE WINAPI ArcticTaskbarButtonsPage( HWND tray )
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page = calloc( 1, sizeof(*page) );

    if (!page) return NULL;
    page->tray = tray;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = startui_instance;
    psp.pResource = &page_template.header;
    psp.pfnDlgProc = page_proc;
    psp.pszTitle = load_string( IDS_BUTTONS_TITLE );
    psp.lParam = (LPARAM)page;
    psp.pfnCallback = page_callback;
    return CreatePropertySheetPageW( &psp );
}
