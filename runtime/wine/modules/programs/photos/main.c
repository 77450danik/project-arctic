/*
 * Photos, as Windows 11 has it: the viewer
 *
 * One window without the system's title bar: the picture's name above, the
 * commands in a row under it, the picture in the middle with arrows to the
 * ones beside it, a strip of the folder's pictures and the zoom at the
 * bottom, and what is known of the file in a pane at the right.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "photos.h"
#include <wincodec.h>

HINSTANCE instance;
HWND frame;
UINT dpi = 96;

#define TITLE_H px( 40 )
#define TOOL_H px( 48 )
#define FILM_H px( 84 )
#define BAR_H px( 44 )
#define INFO_W px( 320 )

static struct folder folder;
static int current = -1;
static struct picture *pic;
static int frame_index;
static WCHAR path[MAX_PATH * 2];
static WCHAR load_error[256];

static double zoom = 1.0, center_x, center_y;   /* the picture's point in the middle of the view */
static BOOL fit = TRUE, show_film, show_info, full, slideshow;
static RECT view_rect;
static struct surface surf, view_cache;
static BOOL view_dirty = TRUE;
static HICON small_icon;
static HWND tooltip;
static int film_scroll;          /* the strip's first pixel */

enum zone
{
    Z_NONE, Z_MIN, Z_MAX, Z_CLOSE,
    Z_ZOOM, Z_ROTATE, Z_EDIT, Z_DELETE, Z_FAV, Z_INFO, Z_MORE,
    Z_PREV, Z_NEXT,
    Z_FILMTOGGLE, Z_ZOOMOUT, Z_SLIDER, Z_ZOOMIN, Z_ZOOMTEXT, Z_FIT, Z_FULL,
    Z_INFO_CLOSE, Z_INFO_FOLDER, Z_OPEN,
    Z_FILM, Z_VIEW, Z_COUNT
};
static RECT zones[Z_COUNT];
static int hot = Z_NONE, down = Z_NONE, film_hot = -1;
static BOOL arrows_shown, slider_drag, panning;
static POINT pan_from;
static double pan_cx, pan_cy;

/**********************************************************************
 *          Strings and settings
 */

const WCHAR *tr( const WCHAR *en, const WCHAR *uk )
{
    static int ukrainian = -1;

    if (ukrainian < 0) ukrainian = PRIMARYLANGID( GetUserDefaultUILanguage() ) == LANG_UKRAINIAN;
    return ukrainian ? uk : en;
}

#define KEY L"Software\\Arctic\\Photos"

DWORD setting( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

void set_setting( const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, KEY, name, REG_DWORD, &value, sizeof(value) );
}

static BOOL is_favorite( const WCHAR *p )
{
    DWORD v = 0, size = sizeof(v);
    return !RegGetValueW( HKEY_CURRENT_USER, KEY L"\\Favorites", p, RRF_RT_REG_DWORD, NULL, &v, &size ) && v;
}

static void set_favorite( const WCHAR *p, BOOL on )
{
    DWORD v = 1;
    if (on) RegSetKeyValueW( HKEY_CURRENT_USER, KEY L"\\Favorites", p, REG_DWORD, &v, sizeof(v) );
    else RegDeleteKeyValueW( HKEY_CURRENT_USER, KEY L"\\Favorites", p );
}

/**********************************************************************
 *          The picture shown
 */

static const UINT32 *shown_bits(void)
{
    if (!pic) return NULL;
    return pic->frame_bits ? pic->frame_bits[frame_index] : pic->bits;
}

static void invalidate(void) { InvalidateRect( frame, NULL, FALSE ); }
static void view_changed(void) { view_dirty = TRUE; invalidate(); }

static double fit_zoom(void)
{
    int w = view_rect.right - view_rect.left, h = view_rect.bottom - view_rect.top;
    double margin = full ? 0 : px( 16 );

    if (!pic || w <= 0 || h <= 0) return 1.0;
    /* a small picture stays at its size, as Photos shows it */
    return min( 1.0, min( (w - 2 * margin) / pic->width, (h - 2 * margin) / pic->height ) );
}

static void clamp_center(void)
{
    double vw = (view_rect.right - view_rect.left) / zoom, vh = (view_rect.bottom - view_rect.top) / zoom;

    if (!pic) return;
    if (pic->width <= vw) center_x = pic->width / 2.0;
    else center_x = max( vw / 2, min( pic->width - vw / 2, center_x ) );
    if (pic->height <= vh) center_y = pic->height / 2.0;
    else center_y = max( vh / 2, min( pic->height - vh / 2, center_y ) );
}

static void set_zoom( double z, const POINT *around )
{
    double fx, fy;
    POINT at;

    if (!pic) return;
    z = max( min( fit_zoom(), 0.05 ), min( 16.0, z ) );
    at.x = around ? around->x : (view_rect.left + view_rect.right) / 2;
    at.y = around ? around->y : (view_rect.top + view_rect.bottom) / 2;
    fx = center_x + (at.x - (view_rect.left + view_rect.right) / 2.0) / zoom;
    fy = center_y + (at.y - (view_rect.top + view_rect.bottom) / 2.0) / zoom;
    zoom = z;
    center_x = fx - (at.x - (view_rect.left + view_rect.right) / 2.0) / zoom;
    center_y = fy - (at.y - (view_rect.top + view_rect.bottom) / 2.0) / zoom;
    fit = fabs( zoom - fit_zoom() ) < 1e-6;
    clamp_center();
    view_changed();
}

static void zoom_fit(void)
{
    fit = TRUE;
    zoom = fit_zoom();
    if (pic)
    {
        center_x = pic->width / 2.0;
        center_y = pic->height / 2.0;
    }
    view_changed();
}

static void zoom_step( int dir, const POINT *around )
{
    static const double steps[] = { 0.05, 0.1, 0.125, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.5, 2, 3, 4, 6, 8, 12, 16 };
    double z = zoom;

    if (dir > 0)
    {
        for (int i = 0; i < ARRAY_SIZE(steps); i++) if (steps[i] > zoom * 1.01) { z = steps[i]; break; }
    }
    else
    {
        for (int i = ARRAY_SIZE(steps) - 1; i >= 0; i--) if (steps[i] < zoom * 0.99) { z = steps[i]; break; }
        if (z < fit_zoom()) z = fit_zoom();
    }
    set_zoom( z, around );
}

static void title_changed(void)
{
    WCHAR title[MAX_PATH + 32];

    if (current >= 0) swprintf( title, ARRAY_SIZE(title), L"%s - %s", folder.names[current], tr( L"Photos", L"Фотографії" ) );
    else lstrcpyW( title, tr( L"Photos", L"Фотографії" ) );
    SetWindowTextW( frame, title );
}

static void film_scroll_to_current(void);

static void show_index( int index )
{
    if (index < 0 || index >= folder.count) return;
    KillTimer( frame, 2 );
    picture_free( pic );
    pic = NULL;
    frame_index = 0;
    current = index;
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", folder.dir, folder.names[index] );
    load_error[0] = 0;
    if (!(pic = picture_load( path )))
        swprintf( load_error, ARRAY_SIZE(load_error), L"%s", tr( L"This file can't be opened.", L"Не вдалося відкрити цей файл." ) );
    else if (pic->frames > 1)
        SetTimer( frame, 2, pic->delays[0], NULL );
    zoom_fit();
    title_changed();
    film_scroll_to_current();
    thumbs_want( index );
    invalidate();
}

static void open_path( const WCHAR *p )
{
    WCHAR full_path[MAX_PATH];
    int index;

    if (!GetFullPathNameW( p, MAX_PATH, full_path, NULL )) lstrcpynW( full_path, p, MAX_PATH );
    folder_load( &folder, full_path, &index );
    thumbs_reset( &folder );
    film_scroll = 0;
    show_index( index );
}

static void open_dialog(void)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR file[MAX_PATH] = { 0 }, filter[256], *f = filter;

    f += swprintf( f, 160, L"%s%c*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.webp;*.heic%c",
                   tr( L"Pictures", L"Зображення" ), 0, 0 );
    f += swprintf( f, 64, L"%s%c*.*%c", tr( L"All files", L"Усі файли" ), 0, 0 );
    *f = 0;
    ofn.hwndOwner = frame;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW( &ofn )) open_path( file );
}

/**********************************************************************
 *          Commands
 */

static void go( int dir )
{
    if (folder.count < 2) return;
    show_index( (current + dir + folder.count) % folder.count );
}

static void rotate(void)
{
    UINT32 *bits;
    int w, h;

    if (!pic || pic->frames > 1) return;
    /* the screen first, the file after */
    w = pic->height;
    h = pic->width;
    if (!(bits = malloc( (size_t)w * h * 4 ))) return;
    for (int y = 0; y < pic->height; y++)
        for (int x = 0; x < pic->width; x++)
            bits[(size_t)x * w + (pic->height - 1 - y)] = pic->bits[(size_t)y * pic->width + x];
    if (!picture_save_rotated( path, pic ))
    {
        free( bits );
        MessageBoxW( frame, tr( L"The picture could not be saved.", L"Не вдалося зберегти зображення." ),
                     tr( L"Photos", L"Фотографії" ), MB_OK | MB_ICONWARNING );
        return;
    }
    blit_forget( pic->bits );
    free( pic->bits );
    pic->bits = bits;
    pic->width = w;
    pic->height = h;
    zoom_fit();
}

static void delete_file(void)
{
    WCHAR from[MAX_PATH * 2 + 2] = { 0 };
    SHFILEOPSTRUCTW op = { 0 };
    int index = current;

    if (current < 0) return;
    lstrcpynW( from, path, MAX_PATH * 2 );
    op.hwnd = frame;
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO;
    picture_free( pic );   /* the file is not held open; let go of it anyway */
    pic = NULL;
    if (SHFileOperationW( &op ) || op.fAnyOperationsAborted || GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES)
    {
        show_index( index );
        return;
    }
    free( folder.names[index] );
    memmove( folder.names + index, folder.names + index + 1, (folder.count - index - 1) * sizeof(WCHAR *) );
    folder.count--;
    thumbs_reset( &folder );
    if (!folder.count)
    {
        current = -1;
        load_error[0] = 0;
        title_changed();
        invalidate();
        return;
    }
    show_index( min( index, folder.count - 1 ) );
}

static void edit_picture(void)
{
    WCHAR args[MAX_PATH * 2 + 4];

    if (current < 0) return;
    /* the "Edit" of the picture's type: Paint, unless another program has it */
    if ((INT_PTR)ShellExecuteW( frame, L"edit", path, NULL, NULL, SW_SHOWNORMAL ) > 32) return;
    swprintf( args, ARRAY_SIZE(args), L"\"%s\"", path );
    ShellExecuteW( frame, NULL, L"mspaint.exe", args, NULL, SW_SHOWNORMAL );
}

static HGLOBAL png_of( const UINT32 *bits, int w, int h )
{
    IWICImagingFactory *factory = NULL;
    IStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    HGLOBAL ret = 0, data;
    STATSTG stat;
    HRESULT hr;

    if (FAILED(CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory )))
        return 0;
    hr = CreateStreamOnHGlobal( NULL, TRUE, &stream );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, &GUID_ContainerFormatPng, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &fe, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( fe, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( fe, w, h );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( fe, &pf );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels( fe, h, w * 4, w * 4 * h, (BYTE *)bits );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( fe );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (SUCCEEDED(hr) && SUCCEEDED(IStream_Stat( stream, &stat, STATFLAG_NONAME )) &&
        SUCCEEDED(GetHGlobalFromStream( stream, &data )) && (ret = GlobalAlloc( GMEM_MOVEABLE, stat.cbSize.LowPart )))
    {
        memcpy( GlobalLock( ret ), GlobalLock( data ), stat.cbSize.LowPart );
        GlobalUnlock( data );
        GlobalUnlock( ret );
    }
    if (fe) IWICBitmapFrameEncode_Release( fe );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IStream_Release( stream );
    IWICImagingFactory_Release( factory );
    return ret;
}

/* the picture as a picture (DIB, PNG) and as a file, for whatever pastes */
static void copy_picture(void)
{
    const UINT32 *bits = shown_bits();
    SIZE_T row, files_len;
    BITMAPINFOHEADER *header;
    HGLOBAL dib = 0, png, drop;
    DROPFILES *df;

    if (!bits) return;
    row = (SIZE_T)pic->width * 4;
    if ((dib = GlobalAlloc( GMEM_MOVEABLE, sizeof(*header) + row * pic->height )))
    {
        header = GlobalLock( dib );
        memset( header, 0, sizeof(*header) );
        header->biSize = sizeof(*header);
        header->biWidth = pic->width;
        header->biHeight = pic->height;
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biSizeImage = row * pic->height;
        for (int y = 0; y < pic->height; y++)
        {
            const UINT32 *src = bits + (SIZE_T)y * pic->width;
            UINT32 *dst = (UINT32 *)((BYTE *)(header + 1) + row * (pic->height - 1 - y));
            for (int x = 0; x < pic->width; x++)
            {
                UINT32 p = src[x], a = p >> 24, out = 0xff000000;
                for (int k = 0; k < 24; k += 8) out |= ((((p >> k) & 0xff) * a + 255 * (255 - a)) / 255) << k;
                dst[x] = out;
            }
        }
        GlobalUnlock( dib );
    }
    png = png_of( bits, pic->width, pic->height );
    files_len = (lstrlenW( path ) + 2) * sizeof(WCHAR);
    if ((drop = GlobalAlloc( GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + files_len )))
    {
        df = GlobalLock( drop );
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        lstrcpyW( (WCHAR *)(df + 1), path );
        GlobalUnlock( drop );
    }
    if (!OpenClipboard( frame )) return;
    EmptyClipboard();
    if (dib) SetClipboardData( CF_DIB, dib );
    if (png) SetClipboardData( RegisterClipboardFormatW( L"PNG" ), png );
    if (drop) SetClipboardData( CF_HDROP, drop );
    CloseClipboard();
}

static void copy_path(void)
{
    HGLOBAL text;
    WCHAR *p;

    if (current < 0 || !(text = GlobalAlloc( GMEM_MOVEABLE, (lstrlenW( path ) + 3) * sizeof(WCHAR) ))) return;
    p = GlobalLock( text );
    swprintf( p, lstrlenW( path ) + 3, L"\"%s\"", path );
    GlobalUnlock( text );
    if (!OpenClipboard( frame )) { GlobalFree( text ); return; }
    EmptyClipboard();
    SetClipboardData( CF_UNICODETEXT, text );
    CloseClipboard();
}

static void show_in_folder(void)
{
    WCHAR args[MAX_PATH * 2 + 16];

    if (current < 0) return;
    swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", path );
    ShellExecuteW( frame, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
}

static void print_picture(void)
{
    PRINTDLGW pd = { sizeof(pd) };
    DOCINFOW doc = { sizeof(doc) };
    const UINT32 *bits = shown_bits();

    if (!bits) return;
    pd.hwndOwner = frame;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
    if (!PrintDlgW( &pd ) || !pd.hDC) return;
    doc.lpszDocName = folder.names[current];
    if (StartDocW( pd.hDC, &doc ) > 0 && StartPage( pd.hDC ) > 0)
    {
        int pw = GetDeviceCaps( pd.hDC, HORZRES ), ph = GetDeviceCaps( pd.hDC, VERTRES );
        double s = min( (double)pw / pic->width, (double)ph / pic->height );
        BITMAPINFO info = { { sizeof(info.bmiHeader), pic->width, -pic->height, 1, 32 } };
        int w = (int)(pic->width * s), h = (int)(pic->height * s);
        SetStretchBltMode( pd.hDC, HALFTONE );
        StretchDIBits( pd.hDC, (pw - w) / 2, (ph - h) / 2, w, h, 0, 0, pic->width, pic->height, bits, &info,
                       DIB_RGB_COLORS, SRCCOPY );
        EndPage( pd.hDC );
        EndDoc( pd.hDC );
    }
    DeleteDC( pd.hDC );
    if (pd.hDevMode) GlobalFree( pd.hDevMode );
    if (pd.hDevNames) GlobalFree( pd.hDevNames );
}

static void set_wallpaper(void)
{
    static const WCHAR fill_style[] = L"10", no_tile[] = L"0";

    if (current < 0) return;
    RegSetKeyValueW( HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", REG_SZ, fill_style, sizeof(fill_style) );
    RegSetKeyValueW( HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", REG_SZ, no_tile, sizeof(no_tile) );
    SystemParametersInfoW( SPI_SETDESKWALLPAPER, 0, path, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
}

static void layout(void);

static void set_full( BOOL on )
{
    static WINDOWPLACEMENT place = { sizeof(place) };
    MONITORINFO mi = { sizeof(mi) };

    if (full == on) return;
    full = on;
    if (on)
    {
        GetWindowPlacement( frame, &place );
        GetMonitorInfoW( MonitorFromWindow( frame, MONITOR_DEFAULTTONEAREST ), &mi );
        SetWindowLongW( frame, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN );
        SetWindowPos( frame, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW );
    }
    else
    {
        slideshow = FALSE;
        KillTimer( frame, 3 );
        SetWindowLongW( frame, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN );
        SetWindowPlacement( frame, &place );
        SetWindowPos( frame, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER );
    }
    layout();
    zoom_fit();
}

static void start_slideshow(void)
{
    if (current < 0) return;
    set_full( TRUE );
    slideshow = TRUE;
    SetTimer( frame, 3, 4000, NULL );
}

enum
{
    IDM_OPEN = 100, IDM_OPENWITH, IDM_COPY, IDM_COPYPATH, IDM_SHOWFOLDER, IDM_PRINT, IDM_WALLPAPER, IDM_SLIDESHOW,
    IDM_FULL, IDM_ABOUT, IDM_FIT, IDM_ACTUAL, IDM_THEME_LIGHT, IDM_THEME_DARK, IDM_THEME_SYSTEM, IDM_ZOOM_FIRST = 200,
};

static void apply_theme(void);

static void more_menu( const RECT *anchor )
{
    DWORD mode = setting( L"Theme", 0 );
    UINT has = current >= 0 ? 0 : MFS_DISABLED;
    struct menu_item theme[] =
    {
        { IDM_THEME_LIGHT, tr( L"Light", L"Світла" ), NULL, NULL, (mode == 1 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        { IDM_THEME_DARK, tr( L"Dark", L"Темна" ), NULL, NULL, (mode == 2 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        { IDM_THEME_SYSTEM, tr( L"Use system setting", L"Як у системі" ), NULL, NULL, (mode == 0 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
    };
    struct menu_item items[] =
    {
        { IDM_OPEN, tr( L"Open", L"Відкрити" ), "open", L"Ctrl+O" },
        { IDM_OPENWITH, tr( L"Open with", L"Відкрити за допомогою" ), "openwith", NULL, has },
        { 0 },
        { IDM_COPY, tr( L"Copy", L"Копіювати" ), "copy", L"Ctrl+C", has },
        { IDM_COPYPATH, tr( L"Copy as path", L"Копіювати як шлях" ), "copypath", NULL, has },
        { IDM_SHOWFOLDER, tr( L"Show in folder", L"Показати в папці" ), "folder", NULL, has },
        { 0 },
        { IDM_PRINT, tr( L"Print", L"Друк" ), "print", L"Ctrl+P", has },
        { IDM_WALLPAPER, tr( L"Set as desktop background", L"Зробити фоном робочого стола" ), "wallpaper", NULL, has },
        { IDM_SLIDESHOW, tr( L"Slideshow", L"Слайд-шоу" ), "slideshow", L"F5", has },
        { IDM_FULL, tr( L"Full screen", L"На весь екран" ), "fullscreen", L"F11", has },
        { 0 },
        { 0, tr( L"Theme", L"Тема" ), NULL, NULL, 0, theme, ARRAY_SIZE(theme) },
        { IDM_ABOUT, tr( L"About Photos", L"Про Фотографії" ), "about" },
    };
    int id = menu_run( frame, anchor, items, ARRAY_SIZE(items) );
    WCHAR args[MAX_PATH * 2 + 40];

    switch (id)
    {
    case IDM_OPEN: open_dialog(); break;
    case IDM_OPENWITH:
        swprintf( args, ARRAY_SIZE(args), L"shell32.dll,OpenAs_RunDLL %s", path );
        ShellExecuteW( frame, NULL, L"rundll32.exe", args, NULL, SW_SHOWNORMAL );
        break;
    case IDM_COPY: copy_picture(); break;
    case IDM_COPYPATH: copy_path(); break;
    case IDM_SHOWFOLDER: show_in_folder(); break;
    case IDM_PRINT: print_picture(); break;
    case IDM_WALLPAPER: set_wallpaper(); break;
    case IDM_SLIDESHOW: start_slideshow(); break;
    case IDM_FULL: set_full( TRUE ); break;
    case IDM_THEME_LIGHT: set_setting( L"Theme", 1 ); apply_theme(); break;
    case IDM_THEME_DARK: set_setting( L"Theme", 2 ); apply_theme(); break;
    case IDM_THEME_SYSTEM: set_setting( L"Theme", 0 ); apply_theme(); break;
    case IDM_ABOUT:
        ShellAboutW( frame, tr( L"Photos", L"Фотографії" ), NULL, LoadIconW( instance, MAKEINTRESOURCEW( IDI_PHOTOS ) ) );
        break;
    }
}

static void zoom_menu( const RECT *anchor )
{
    static const int levels[] = { 1600, 800, 400, 200, 100, 50, 25 };
    WCHAR names[ARRAY_SIZE(levels)][16];
    struct menu_item items[ARRAY_SIZE(levels) + 3];
    int n = 0, id;

    memset( items, 0, sizeof(items) );
    items[n++] = (struct menu_item){ IDM_FIT, tr( L"Fit to window", L"За розміром вікна" ), "fit", NULL, fit ? MFS_CHECKED : 0 };
    items[n++] = (struct menu_item){ IDM_ACTUAL, tr( L"Actual size", L"Справжній розмір" ), "zoom100" };
    items[n++] = (struct menu_item){ 0 };
    for (int i = 0; i < ARRAY_SIZE(levels); i++)
    {
        swprintf( names[i], 16, L"%d%%", levels[i] );
        items[n++] = (struct menu_item){ IDM_ZOOM_FIRST + i, names[i] };
    }
    id = menu_run( frame, anchor, items, n );
    if (id == IDM_FIT) zoom_fit();
    else if (id == IDM_ACTUAL) set_zoom( 1.0, NULL );
    else if (id >= IDM_ZOOM_FIRST) set_zoom( levels[id - IDM_ZOOM_FIRST] / 100.0, NULL );
}

/**********************************************************************
 *          Layout
 */

static void layout(void)
{
    RECT rc;
    int top, bottom, x, y, bw = px( 40 ), bh = px( 36 );
    static const int toolbar[] = { Z_ZOOM, Z_ROTATE, Z_EDIT, Z_DELETE, Z_FAV, Z_INFO, Z_MORE };
    int total = 0;

    GetClientRect( frame, &rc );
    memset( zones, 0, sizeof(zones) );
    if (full)
    {
        view_rect = rc;
    }
    else
    {
        SetRect( &zones[Z_CLOSE], rc.right - px( 46 ), 0, rc.right, px( 32 ) );
        SetRect( &zones[Z_MAX], rc.right - px( 92 ), 0, rc.right - px( 46 ), px( 32 ) );
        SetRect( &zones[Z_MIN], rc.right - px( 138 ), 0, rc.right - px( 92 ), px( 32 ) );

        /* the commands in the middle of their row */
        for (int i = 0; i < ARRAY_SIZE(toolbar); i++) total += (toolbar[i] == Z_ZOOM ? bw + px( 18 ) : bw) + px( 4 );
        total += px( 17 );   /* the line before the information */
        x = (rc.right - total) / 2;
        y = TITLE_H + (TOOL_H - bh) / 2;
        for (int i = 0; i < ARRAY_SIZE(toolbar); i++)
        {
            int w = toolbar[i] == Z_ZOOM ? bw + px( 18 ) : bw;
            if (toolbar[i] == Z_INFO) x += px( 17 );
            SetRect( &zones[toolbar[i]], x, y, x + w, y + bh );
            x += w + px( 4 );
        }
        top = TITLE_H + TOOL_H;
        bottom = rc.bottom - BAR_H - (show_film ? FILM_H : 0);
        SetRect( &view_rect, 0, top, rc.right - (show_info ? INFO_W : 0), max( bottom, top + 1 ) );
        if (show_film) SetRect( &zones[Z_FILM], 0, bottom, view_rect.right, bottom + FILM_H );

        /* the bottom row */
        y = rc.bottom - BAR_H + (BAR_H - px( 32 )) / 2;
        SetRect( &zones[Z_FILMTOGGLE], px( 12 ), y, px( 12 ) + px( 36 ), y + px( 32 ) );
        x = rc.right - px( 12 );
        SetRect( &zones[Z_FULL], x - px( 32 ), y, x, y + px( 32 ) ); x -= px( 36 );
        SetRect( &zones[Z_FIT], x - px( 32 ), y, x, y + px( 32 ) ); x -= px( 40 );
        SetRect( &zones[Z_ZOOMTEXT], x - px( 52 ), y, x, y + px( 32 ) ); x -= px( 56 );
        SetRect( &zones[Z_ZOOMIN], x - px( 32 ), y, x, y + px( 32 ) ); x -= px( 32 );
        SetRect( &zones[Z_SLIDER], x - px( 130 ), y, x, y + px( 32 ) ); x -= px( 130 );
        SetRect( &zones[Z_ZOOMOUT], x - px( 32 ), y, x, y + px( 32 ) );

        if (show_info)
        {
            SetRect( &zones[Z_INFO_CLOSE], rc.right - px( 48 ), top + px( 12 ), rc.right - px( 16 ), top + px( 44 ) );
        }
    }
    {
        int ah = px( 48 ), aw = px( 40 ), cy = (view_rect.top + view_rect.bottom) / 2;
        SetRect( &zones[Z_PREV], view_rect.left + px( 16 ), cy - ah / 2, view_rect.left + px( 16 ) + aw, cy + ah / 2 );
        SetRect( &zones[Z_NEXT], view_rect.right - px( 16 ) - aw, cy - ah / 2, view_rect.right - px( 16 ), cy + ah / 2 );
    }
    if (current < 0)
    {
        int cx = (view_rect.left + view_rect.right) / 2, cy = (view_rect.top + view_rect.bottom) / 2;
        SetRect( &zones[Z_OPEN], cx - px( 80 ), cy + px( 16 ), cx + px( 80 ), cy + px( 48 ) );
    }
    zones[Z_VIEW] = view_rect;
    if (fit) zoom = fit_zoom();
    clamp_center();
    view_dirty = TRUE;

    if (tooltip)
    {
        static const struct { int zone; const WCHAR *en, *uk; } tips[] =
        {
            { Z_ZOOM, L"Zoom", L"Масштаб" }, { Z_ROTATE, L"Rotate (Ctrl+R)", L"Обернути (Ctrl+R)" },
            { Z_EDIT, L"Edit image (Ctrl+E)", L"Редагувати (Ctrl+E)" }, { Z_DELETE, L"Delete (Del)", L"Видалити (Del)" },
            { Z_FAV, L"Add to favourites (Ctrl+D)", L"Додати до обраного (Ctrl+D)" },
            { Z_INFO, L"File information (Alt+Enter)", L"Відомості про файл (Alt+Enter)" },
            { Z_MORE, L"See more", L"Інші дії" }, { Z_PREV, L"Previous", L"Попереднє" }, { Z_NEXT, L"Next", L"Наступне" },
            { Z_FILMTOGGLE, L"Filmstrip", L"Стрічка" }, { Z_ZOOMOUT, L"Zoom out", L"Зменшити" },
            { Z_ZOOMIN, L"Zoom in", L"Збільшити" }, { Z_FIT, L"Fit to window", L"За розміром вікна" },
            { Z_FULL, L"Full screen (F11)", L"На весь екран (F11)" },
        };
        int count = (int)SendMessageW( tooltip, TTM_GETTOOLCOUNT, 0, 0 );
        for (int i = count - 1; i >= 0; i--)
        {
            TTTOOLINFOW ti = { sizeof(ti) };
            if (SendMessageW( tooltip, TTM_ENUMTOOLSW, i, (LPARAM)&ti )) SendMessageW( tooltip, TTM_DELTOOLW, 0, (LPARAM)&ti );
        }
        for (int i = 0; i < ARRAY_SIZE(tips); i++)
        {
            TTTOOLINFOW ti = { sizeof(ti), TTF_SUBCLASS, frame, tips[i].zone };
            if (IsRectEmpty( &zones[tips[i].zone] )) continue;
            ti.rect = zones[tips[i].zone];
            ti.lpszText = (WCHAR *)tr( tips[i].en, tips[i].uk );
            SendMessageW( tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti );
        }
    }
}

/**********************************************************************
 *          The filmstrip
 */

static int film_item_w(void) { return px( 64 ); }
static int film_gap(void) { return px( 6 ); }

static RECT film_item( int i )
{
    RECT r, f = zones[Z_FILM];
    int w = film_item_w(), h = px( 64 ), x = f.left + px( 16 ) + i * (w + film_gap()) - film_scroll;

    SetRect( &r, x, f.top + (f.bottom - f.top - h) / 2, x + w, f.top + (f.bottom - f.top - h) / 2 + h );
    return r;
}

static void film_scroll_to_current(void)
{
    RECT rc;
    int width, total, item;

    GetClientRect( frame, &rc );
    width = view_rect.right - view_rect.left;   /* the strip ends where the pane begins */
    item = film_item_w() + film_gap();
    total = px( 32 ) + folder.count * item - film_gap();
    film_scroll = current * item + item / 2 + px( 16 ) - width / 2;
    film_scroll = max( 0, min( film_scroll, total - width ) );
}

static int film_hit( POINT pt )
{
    if (!show_film || !PtInRect( &zones[Z_FILM], pt )) return -1;
    for (int i = 0; i < folder.count; i++)
    {
        RECT r = film_item( i );
        if (PtInRect( &r, pt )) return i;
    }
    return -1;
}

static void paint_film(void)
{
    RECT f = zones[Z_FILM];

    fill( &surf, &f, th.chrome );
    for (int i = 0; i < folder.count; i++)
    {
        RECT r = film_item( i ), inner;
        UINT32 *bits;
        int tw, tht;

        if (r.right < f.left || r.left > f.right) continue;
        thumbs_want( i );
        inner = r;
        if (i == current)
        {
            RECT ring_r = r;
            InflateRect( &ring_r, px( 3 ), px( 3 ) );
            fill_round( &surf, &ring_r, (float)px( 7 ), th.accent, 255 );
            InflateRect( &ring_r, -px( 2 ), -px( 2 ) );
            fill_round( &surf, &ring_r, (float)px( 5 ), th.chrome, 255 );
        }
        fill_round( &surf, &inner, (float)px( 4 ), th.dark ? RGB( 0x2d, 0x2d, 0x2d ) : RGB( 0xe5, 0xe5, 0xe5 ), 255 );
        if (thumb_get( i, &bits, &tw, &tht ) && bits)
        {
            /* the middle of the picture, filling the square */
            double s = max( (double)(r.right - r.left) / tw, (double)(r.bottom - r.top) / tht );
            double ox = (r.left + r.right) / 2.0 - tw * s / 2, oy = (r.top + r.bottom) / 2.0 - tht * s / 2;
            blit_image( &surf, &r, bits, tw, tht, ox, oy, s, BLIT_SMOOTH );
            /* rounded corners: the strip's colour over them */
            {
                int rad = px( 4 );
                GdiFlush();
                for (int y = 0; y < rad; y++)
                    for (int x = 0; x < rad; x++)
                    {
                        double d = hypot( rad - x - 0.5, rad - y - 0.5 );
                        if (d <= rad) continue;
                        {
                            int xs[2] = { r.left + x, r.right - 1 - x }, ys[2] = { r.top + y, r.bottom - 1 - y };
                            for (int a = 0; a < 2; a++)
                                for (int b = 0; b < 2; b++)
                                    if (xs[a] >= 0 && xs[a] < surf.width && ys[b] >= 0 && ys[b] < surf.height)
                                        surf.bits[(size_t)ys[b] * surf.width + xs[a]] =
                                            0xff000000 | (GetRValue( th.chrome ) << 16) | (GetGValue( th.chrome ) << 8) | GetBValue( th.chrome );
                        }
                    }
            }
        }
        else draw_icon( &surf, "photo", (r.left + r.right - px( 24 )) / 2, (r.top + r.bottom - px( 24 )) / 2, px( 24 ), IS_DISABLED );
        if (film_hot == i && i != current)
        {
            RECT h = r;
            outline_round( &surf, &h, (float)px( 4 ), th.dim, 255 );
        }
    }
}

/**********************************************************************
 *          Painting
 */

static void button( int z, const char *icon, enum icon_state state )
{
    RECT r = zones[z];
    int size = px( 20 );

    if (IsRectEmpty( &r )) return;
    if (down == z) fill_round( &surf, &r, (float)px( 4 ), th.pressed, 255 );
    else if (hot == z) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
    draw_icon( &surf, icon, (r.left + r.right - size) / 2, (r.top + r.bottom - size) / 2, size, state );
}

static void render_view(void)
{
    RECT r = view_rect;
    int w = r.right - r.left, h = r.bottom - r.top;
    const UINT32 *bits = shown_bits();
    COLORREF back = full ? RGB( 0, 0, 0 ) : th.workspace;

    if (!surface_size( &view_cache, max( w, 1 ), max( h, 1 ) )) return;
    OffsetRect( &r, -r.left, -r.top );
    fill( &view_cache, &r, back );
    if (bits)
    {
        double ox = w / 2.0 - center_x * zoom, oy = h / 2.0 - center_y * zoom;
        RECT img = { (LONG)floor( ox ), (LONG)floor( oy ), (LONG)ceil( ox + pic->width * zoom ), (LONG)ceil( oy + pic->height * zoom ) };
        /* see-through parts over the background, not a chequer */
        fill( &view_cache, &img, back );
        blit_image( &view_cache, &r, bits, pic->width, pic->height, ox, oy, zoom, BLIT_SMOOTH | BLIT_LAYER );
    }
    view_dirty = FALSE;
}

static void paint( HDC hdc )
{
    RECT rc, r;
    BOOL active = GetActiveWindow() == frame;
    COLORREF text = active ? th.text : th.dim;
    WCHAR buf[128];

    GetClientRect( frame, &rc );
    if (!surface_size( &surf, rc.right, rc.bottom )) return;
    fill( &surf, &rc, full ? RGB( 0, 0, 0 ) : th.chrome );

    if (view_dirty) render_view();
    GdiFlush();
    BitBlt( surf.dc, view_rect.left, view_rect.top, view_rect.right - view_rect.left, view_rect.bottom - view_rect.top,
            view_cache.dc, 0, 0, SRCCOPY );

    if (current < 0 || load_error[0])
    {
        RECT t = view_rect;
        t.bottom = (view_rect.top + view_rect.bottom) / 2;
        t.top = t.bottom - px( 40 );
        draw_text( &surf, load_error[0] ? load_error : tr( L"Open a picture to see it here.", L"Відкрийте зображення, щоб переглянути його тут." ),
                   -1, &t, get_font( F_UI_SEMI ), th.dim, DT_CENTER );
        if (current < 0)
        {
            r = zones[Z_OPEN];
            fill_round( &surf, &r, (float)px( 4 ), hot == Z_OPEN ? mix( th.accent, th.text, 30 ) : th.accent, 255 );
            draw_text( &surf, tr( L"Open file", L"Відкрити файл" ), -1, &r, get_font( F_UI ), th.on_accent, DT_CENTER );
        }
    }

    /* the arrows to the pictures beside, while the mouse moves over it */
    if (arrows_shown && folder.count > 1)
    {
        for (int z = Z_PREV; z <= Z_NEXT; z++)
        {
            r = zones[z];
            fill_round( &surf, &r, (float)px( 6 ), th.dark ? RGB( 0x2c, 0x2c, 0x2c ) : RGB( 0xfb, 0xfb, 0xfb ), hot == z ? 255 : 220 );
            outline_round( &surf, &r, (float)px( 6 ), th.dark ? RGB( 0x3c, 0x3c, 0x3c ) : RGB( 0xe2, 0xe2, 0xe2 ), 255 );
            draw_glyph( &surf, z == Z_PREV ? G_LEFT : G_RIGHT, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2,
                        px( 16 ), th.text );
        }
    }

    if (!full)
    {
        /* the title */
        if (small_icon)
        {
            GdiFlush();
            DrawIconEx( surf.dc, px( 16 ), (TITLE_H - px( 16 )) / 2, small_icon, px( 16 ), px( 16 ), 0, NULL, DI_NORMAL );
        }
        SetRect( &r, px( 44 ), 0, zones[Z_MIN].left - px( 16 ), TITLE_H );
        draw_text( &surf, current >= 0 ? folder.names[current] : tr( L"Photos", L"Фотографії" ), -1, &r, get_font( F_TITLE ), text, DT_LEFT );
        for (int z = Z_MIN; z <= Z_CLOSE; z++)
        {
            int glyph = z == Z_MIN ? G_MIN : z == Z_CLOSE ? G_CLOSE : IsZoomed( frame ) ? G_RESTORE : G_MAX;
            COLORREF color = text;
            r = zones[z];
            if (hot == z && z == Z_CLOSE)
            {
                fill( &surf, &r, down == z ? RGB( 0xc8, 0x3c, 0x2e ) : RGB( 0xc4, 0x2b, 0x1c ) );
                color = RGB( 255, 255, 255 );
            }
            else if (hot == z) fill( &surf, &r, down == z ? th.pressed : th.hover );
            draw_glyph( &surf, glyph, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2, px( 16 ), color );
        }

        /* the commands */
        {
            enum icon_state st = pic ? IS_NORMAL : IS_DISABLED;
            r = zones[Z_ZOOM];
            if (down == Z_ZOOM) fill_round( &surf, &r, (float)px( 4 ), th.pressed, 255 );
            else if (hot == Z_ZOOM) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
            draw_icon( &surf, "zoomin", r.left + px( 10 ), (r.top + r.bottom - px( 20 )) / 2, px( 20 ), st );
            draw_glyph( &surf, G_DOWN_SMALL, r.right - px( 22 ), (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.dim );
            button( Z_ROTATE, "rotright", pic && pic->frames == 1 ? IS_NORMAL : IS_DISABLED );
            button( Z_EDIT, "editimage", st );
            button( Z_DELETE, "delete", current >= 0 ? IS_NORMAL : IS_DISABLED );
            if (current >= 0 && is_favorite( path )) button( Z_FAV, "heart_on", IS_NORMAL );
            else button( Z_FAV, "heart", current >= 0 ? IS_NORMAL : IS_DISABLED );
            r = zones[Z_INFO];
            SetRect( &r, r.left - px( 9 ), r.top + px( 8 ), r.left - px( 8 ), r.bottom - px( 8 ) );
            fill( &surf, &r, th.bar_line );
            if (show_info) fill_round( &surf, &zones[Z_INFO], (float)px( 4 ), th.checked, 255 );
            button( Z_INFO, "info", IS_NORMAL );
            r = zones[Z_MORE];
            if (down == Z_MORE) fill_round( &surf, &r, (float)px( 4 ), th.pressed, 255 );
            else if (hot == Z_MORE) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
            draw_glyph( &surf, G_MORE, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2, px( 16 ), th.text );
        }

        if (show_film) paint_film();

        /* the bottom row */
        r = rc;
        r.top = r.bottom - BAR_H;
        fill( &surf, &r, th.chrome );
        if (show_film) fill_round( &surf, &zones[Z_FILMTOGGLE], (float)px( 4 ), th.checked, 255 );
        button( Z_FILMTOGGLE, "filmstrip", IS_NORMAL );
        if (folder.count)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%d / %d", current + 1, folder.count );
            SetRect( &r, zones[Z_FILMTOGGLE].right + px( 12 ), zones[Z_FILMTOGGLE].top, zones[Z_FILMTOGGLE].right + px( 140 ),
                     zones[Z_FILMTOGGLE].bottom );
            draw_text( &surf, buf, -1, &r, get_font( F_UI ), th.dim, DT_LEFT );
        }
        if (pic)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%d × %d", pic->width, pic->height );
            SetRect( &r, zones[Z_FILMTOGGLE].right + px( 120 ), zones[Z_FILMTOGGLE].top, zones[Z_ZOOMOUT].left - px( 16 ),
                     zones[Z_FILMTOGGLE].bottom );
            draw_text( &surf, buf, -1, &r, get_font( F_UI ), th.dim, DT_LEFT );
        }
        r = zones[Z_ZOOMOUT];
        if (hot == Z_ZOOMOUT) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
        draw_glyph( &surf, G_MINUS, (r.left + r.right - px( 14 )) / 2, (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.text );
        r = zones[Z_ZOOMIN];
        if (hot == Z_ZOOMIN) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
        draw_glyph( &surf, G_PLUS, (r.left + r.right - px( 14 )) / 2, (r.top + r.bottom - px( 14 )) / 2, px( 14 ), th.text );
        {
            /* the slider: the zoom on a scale of doublings, 5 % to 1600 % */
            int cy, l, w, tx;
            double t = pic ? (log( zoom / 0.05 ) / log( 16.0 / 0.05 )) : 0;
            r = zones[Z_SLIDER];
            cy = (r.top + r.bottom) / 2;
            l = r.left + px( 8 );
            w = r.right - r.left - px( 16 );
            tx = l + (int)(max( 0.0, min( 1.0, t ) ) * w);
            {
                RECT track = { l, cy - px( 2 ), l + w, cy + px( 2 ) }, done = { l, cy - px( 2 ), tx, cy + px( 2 ) };
                fill_round( &surf, &track, (float)px( 2 ), th.dark ? RGB( 0x9a, 0x9a, 0x9a ) : RGB( 0x86, 0x86, 0x86 ), 255 );
                fill_round( &surf, &done, (float)px( 2 ), th.accent, 255 );
            }
            fill_circle( &surf, tx + 0.5f, cy + 0.5f, (float)px( 10 ), th.dark ? RGB( 0x45, 0x45, 0x45 ) : RGB( 0xff, 0xff, 0xff ), 255 );
            ring( &surf, tx + 0.5f, cy + 0.5f, (float)px( 10 ), 1.0f, th.dark ? RGB( 0x55, 0x55, 0x55 ) : RGB( 0xd0, 0xd0, 0xd0 ), 255 );
            fill_circle( &surf, tx + 0.5f, cy + 0.5f, (float)px( hot == Z_SLIDER || slider_drag ? 7 : 5 ), th.accent, 255 );
        }
        swprintf( buf, ARRAY_SIZE(buf), L"%d%%", pic ? (int)floor( zoom * 100 + 0.5 ) : 100 );
        draw_text( &surf, buf, -1, &zones[Z_ZOOMTEXT], get_font( F_UI ), th.text, DT_CENTER );
        if (fit) fill_round( &surf, &zones[Z_FIT], (float)px( 4 ), th.checked, 255 );
        button( Z_FIT, "fit", IS_NORMAL );
        button( Z_FULL, "fullscreen", IS_NORMAL );

        /* what is known of the file */
        if (show_info)
        {
            RECT pane = { view_rect.right, view_rect.top, rc.right, rc.bottom - BAR_H };
            int y = pane.top + px( 16 ), x = pane.left + px( 20 ), right = pane.right - px( 20 );
            WIN32_FILE_ATTRIBUTE_DATA data;
            SYSTEMTIME st;
            FILETIME local;
            WCHAR date[64], time_s[32];

            fill( &surf, &pane, th.bar );
            r = pane; r.right = r.left + 1;
            fill( &surf, &r, th.bar_line );
            SetRect( &r, x, y, right - px( 40 ), y + px( 28 ) );
            draw_text( &surf, tr( L"File information", L"Відомості про файл" ), -1, &r, get_font( F_UI_SEMI ), th.text, DT_LEFT );
            r = zones[Z_INFO_CLOSE];
            if (hot == Z_INFO_CLOSE) fill_round( &surf, &r, (float)px( 4 ), th.hover, 255 );
            draw_glyph( &surf, G_CLOSE, (r.left + r.right - px( 12 )) / 2, (r.top + r.bottom - px( 12 )) / 2, px( 12 ), th.text );
            y += px( 48 );
            if (current >= 0)
            {
                struct { const WCHAR *label; WCHAR value[MAX_PATH]; } rows[6];
                int n = 0;

                rows[n].label = tr( L"Name", L"Ім'я" );
                lstrcpynW( rows[n++].value, folder.names[current], MAX_PATH );
                if (pic && pic->taken[0])
                {
                    /* "2024:05:01 12:30:00" as the camera writes it */
                    int Y, M, D, h, m;
                    rows[n].label = tr( L"Date taken", L"Дата зйомки" );
                    if (swscanf( pic->taken, L"%d:%d:%d %d:%d", &Y, &M, &D, &h, &m ) == 5)
                        swprintf( rows[n].value, MAX_PATH, L"%02d.%02d.%04d %02d:%02d", D, M, Y, h, m );
                    else lstrcpynW( rows[n].value, pic->taken, MAX_PATH );
                    n++;
                }
                if (GetFileAttributesExW( path, GetFileExInfoStandard, &data ))
                {
                    ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
                    FileTimeToLocalFileTime( &data.ftLastWriteTime, &local );
                    FileTimeToSystemTime( &local, &st );
                    GetDateFormatW( LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, ARRAY_SIZE(date) );
                    GetTimeFormatW( LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time_s, ARRAY_SIZE(time_s) );
                    rows[n].label = tr( L"Date modified", L"Дата зміни" );
                    swprintf( rows[n++].value, MAX_PATH, L"%s %s", date, time_s );
                    rows[n].label = tr( L"Size", L"Розмір" );
                    if (size < 1024 * 1024) swprintf( rows[n].value, MAX_PATH, L"%.1f %s", size / 1024.0, tr( L"KB", L"КБ" ) );
                    else swprintf( rows[n].value, MAX_PATH, L"%.1f %s", size / 1048576.0, tr( L"MB", L"МБ" ) );
                    if (*tr( L".", L"," ) == ',') for (WCHAR *c = rows[n].value; *c; c++) if (*c == '.') *c = ',';
                    if (pic)
                    {
                        WCHAR extra[64];
                        swprintf( extra, ARRAY_SIZE(extra), L" · %d × %d", pic->width, pic->height );
                        lstrcatW( rows[n].value, extra );
                    }
                    n++;
                }
                if (pic && pic->camera[0])
                {
                    rows[n].label = tr( L"Camera", L"Камера" );
                    lstrcpynW( rows[n++].value, pic->camera, MAX_PATH );
                }
                rows[n].label = tr( L"Folder", L"Папка" );
                lstrcpynW( rows[n++].value, folder.dir, MAX_PATH );
                for (int i = 0; i < n; i++)
                {
                    BOOL link = i == n - 1;
                    SetRect( &r, x, y, right, y + px( 18 ) );
                    draw_text( &surf, rows[i].label, -1, &r, get_font( F_SMALL ), th.dim, DT_LEFT );
                    SetRect( &r, x, y + px( 18 ), right, y + px( 18 + 34 ) );
                    if (link) zones[Z_INFO_FOLDER] = r;
                    draw_text( &surf, rows[i].value, -1, &r, get_font( F_UI ), link ? th.accent : th.text,
                               DT_LEFT | DT_WORDBREAK | DT_PATH_ELLIPSIS );
                    y += px( 64 );
                }
            }
        }
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, rc.right, rc.bottom, surf.dc, 0, 0, SRCCOPY );
}

/**********************************************************************
 *          The window
 */

static int hit( POINT pt )
{
    static const int order[] =
    {
        Z_MIN, Z_MAX, Z_CLOSE, Z_ZOOM, Z_ROTATE, Z_EDIT, Z_DELETE, Z_FAV, Z_INFO, Z_MORE, Z_FILMTOGGLE, Z_ZOOMOUT,
        Z_SLIDER, Z_ZOOMIN, Z_ZOOMTEXT, Z_FIT, Z_FULL, Z_INFO_CLOSE, Z_INFO_FOLDER, Z_OPEN,
    };

    if (arrows_shown && folder.count > 1)
    {
        if (PtInRect( &zones[Z_PREV], pt )) return Z_PREV;
        if (PtInRect( &zones[Z_NEXT], pt )) return Z_NEXT;
    }
    for (int i = 0; i < ARRAY_SIZE(order); i++)
        if (!IsRectEmpty( &zones[order[i]] ) && PtInRect( &zones[order[i]], pt )) return order[i];
    if (PtInRect( &zones[Z_FILM], pt )) return Z_FILM;
    if (PtInRect( &view_rect, pt )) return Z_VIEW;
    return Z_NONE;
}

static void slider_set( POINT pt )
{
    RECT r = zones[Z_SLIDER];
    int l = r.left + px( 8 ), w = r.right - r.left - px( 16 );
    double t = max( 0.0, min( 1.0, (double)(pt.x - l) / w ) );
    double z = 0.05 * pow( 16.0 / 0.05, t );

    if (fabs( z - 1.0 ) < 0.05) z = 1.0;
    set_zoom( z, NULL );
}

static void click( int z, POINT pt )
{
    RECT anchor = zones[z];

    MapWindowPoints( frame, NULL, (POINT *)&anchor, 2 );
    switch (z)
    {
    case Z_MIN: PostMessageW( frame, WM_SYSCOMMAND, SC_MINIMIZE, 0 ); break;
    case Z_MAX: PostMessageW( frame, WM_SYSCOMMAND, IsZoomed( frame ) ? SC_RESTORE : SC_MAXIMIZE, 0 ); break;
    case Z_CLOSE: PostMessageW( frame, WM_CLOSE, 0, 0 ); break;
    case Z_ZOOM: if (pic) zoom_menu( &anchor ); break;
    case Z_ROTATE: rotate(); break;
    case Z_EDIT: edit_picture(); break;
    case Z_DELETE: delete_file(); break;
    case Z_FAV:
        if (current >= 0) set_favorite( path, !is_favorite( path ) );
        break;
    case Z_INFO:
        show_info = !show_info;
        set_setting( L"Info", show_info );
        layout();
        film_scroll_to_current();
        break;
    case Z_INFO_CLOSE:
        show_info = FALSE;
        set_setting( L"Info", 0 );
        layout();
        film_scroll_to_current();
        break;
    case Z_INFO_FOLDER: show_in_folder(); break;
    case Z_MORE: more_menu( &anchor ); break;
    case Z_PREV: go( -1 ); break;
    case Z_NEXT: go( 1 ); break;
    case Z_FILMTOGGLE:
        show_film = !show_film;
        set_setting( L"Filmstrip", show_film );
        layout();
        film_scroll_to_current();
        break;
    case Z_ZOOMOUT: zoom_step( -1, NULL ); break;
    case Z_ZOOMIN: zoom_step( 1, NULL ); break;
    case Z_ZOOMTEXT: if (pic) zoom_menu( &anchor ); break;
    case Z_FIT: zoom_fit(); break;
    case Z_FULL: set_full( TRUE ); break;
    case Z_OPEN: open_dialog(); break;
    }
    invalidate();
}

static void paint_frame_edges( HWND hwnd )
{
    RECT window, client;
    POINT origin = { 0, 0 };
    HDC hdc = GetWindowDC( hwnd );
    HBRUSH brush;

    if (!hdc) return;
    GetWindowRect( hwnd, &window );
    GetClientRect( hwnd, &client );
    ClientToScreen( hwnd, &origin );
    OffsetRect( &client, origin.x - window.left, origin.y - window.top );
    OffsetRect( &window, -window.left, -window.top );
    ExcludeClipRect( hdc, client.left, client.top, client.right, client.bottom );
    brush = CreateSolidBrush( th.chrome );
    FillRect( hdc, &window, brush );
    DeleteObject( brush );
    ReleaseDC( hwnd, hdc );
}

static void menus_follow_theme(void)
{
    HMODULE uxtheme = GetModuleHandleW( L"uxtheme.dll" );
    int (WINAPI *set_mode)( int );
    void (WINAPI *flush)(void);

    if (!uxtheme && !(uxtheme = LoadLibraryW( L"uxtheme.dll" ))) return;
    set_mode = (void *)GetProcAddress( uxtheme, MAKEINTRESOURCEA( 135 ) );
    flush = (void *)GetProcAddress( uxtheme, MAKEINTRESOURCEA( 136 ) );
    if (set_mode) set_mode( th.dark ? 2 : 3 );
    if (flush) flush();
}

static void no_caption_backdrop( HWND hwnd )
{
    UINT32 caption[5] = { 0 };

    DwmSetWindowAttribute( hwnd, 0x1005, caption, sizeof(caption) );
}

static void apply_theme(void)
{
    BOOL dark;

    theme_load();
    /* Photos keeps its picture on a darker ground than Paint */
    th.workspace = th.dark ? RGB( 0x1c, 0x1c, 0x1c ) : RGB( 0xf3, 0xf3, 0xf3 );
    th.chrome = th.dark ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf3, 0xf3 );
    menus_follow_theme();
    dark = th.dark;
    DwmSetWindowAttribute( frame, 20, &dark, sizeof(dark) );
    fonts_reset();
    paint_frame_edges( frame );
    view_changed();
}

static LRESULT CALLBACK frame_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_CREATE:
        frame = hwnd;
        no_caption_backdrop( hwnd );
        tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd,
                                   NULL, instance, NULL );
        return 0;

    case WM_NCCALCSIZE:
    {
        RECT *rect = wparam ? &((NCCALCSIZE_PARAMS *)lparam)->rgrc[0] : (RECT *)lparam;
        int top = rect->top;
        LRESULT result;

        if (full) return 0;
        result = DefWindowProcW( hwnd, msg, wparam, lparam );
        rect->top = top;
        if (IsZoomed( hwnd ))
        {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &mi ))
                rect->top = max( top, mi.rcWork.top );
        }
        return result;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        MONITORINFO monitor = { sizeof(monitor) };
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );
        int bx = max( 0, -info->ptMaxPosition.x ), by = max( 0, -info->ptMaxPosition.y );

        if (!full && GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &monitor ))
        {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left - bx;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top - by;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left + 2 * bx;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top + 2 * by;
        }
        info->ptMinTrackSize.x = px( 500 );
        info->ptMinTrackSize.y = px( 360 );
        return result;
    }

    case WM_NCPAINT:
        if (!full) paint_frame_edges( hwnd );
        return 0;

    case WM_NCACTIVATE:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, (LPARAM)-1 );
        no_caption_backdrop( hwnd );
        if (!full) paint_frame_edges( hwnd );
        invalidate();
        return result;
    }

    case WM_SETTEXT:
    case WM_SETICON:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );
        if (!full) paint_frame_edges( hwnd );
        invalidate();
        return result;
    }

    case WM_NCHITTEST:
    {
        LRESULT h;
        RECT rect;

        if (full) return HTCLIENT;
        ScreenToClient( hwnd, &pt );
        if (hit( pt ) >= Z_MIN && hit( pt ) <= Z_CLOSE) return HTCLIENT;
        h = DefWindowProcW( hwnd, msg, wparam, lparam );
        if (h == HTMINBUTTON || h == HTMAXBUTTON || h == HTCLOSE || h == HTHELP) return HTCAPTION;
        if (h != HTCLIENT) return h;
        GetClientRect( hwnd, &rect );
        if (!IsZoomed( hwnd ) && pt.y < px( 4 ))
        {
            if (pt.x < px( 8 )) return HTTOPLEFT;
            if (pt.x >= rect.right - px( 8 )) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y < TITLE_H) return pt.x < px( 36 ) ? HTSYSMENU : HTCAPTION;
        /* the toolbar's empty places move the window too */
        if (pt.y < TITLE_H + TOOL_H && hit( pt ) == Z_NONE) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        no_caption_backdrop( hwnd );
        layout();
        invalidate();
        return 0;

    case WM_MOUSEMOVE:
    {
        int h = hit( pt ), fh = film_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (slider_drag) { slider_set( pt ); return 0; }
        if (panning)
        {
            center_x = pan_cx - (pt.x - pan_from.x) / zoom;
            center_y = pan_cy - (pt.y - pan_from.y) / zoom;
            clamp_center();
            view_changed();
            return 0;
        }
        if (PtInRect( &view_rect, pt ) && !arrows_shown && folder.count > 1)
        {
            arrows_shown = TRUE;
            h = hit( pt );
            invalidate();
        }
        if (arrows_shown) SetTimer( hwnd, 1, 2000, NULL );
        if (h != hot || fh != film_hot)
        {
            hot = h;
            film_hot = fh;
            invalidate();
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        hot = Z_NONE;
        film_hot = -1;
        invalidate();
        return 0;
    case WM_TIMER:
        if (wparam == 1)
        {
            POINT cur;
            KillTimer( hwnd, 1 );
            GetCursorPos( &cur );
            ScreenToClient( hwnd, &cur );
            if (hot != Z_PREV && hot != Z_NEXT) { arrows_shown = FALSE; invalidate(); }
        }
        else if (wparam == 2 && pic && pic->frames > 1)
        {
            frame_index = (frame_index + 1) % pic->frames;
            SetTimer( hwnd, 2, pic->delays[frame_index], NULL );
            view_changed();
        }
        else if (wparam == 3 && slideshow) go( 1 );
        return 0;
    case WM_SETCURSOR:
        if (LOWORD( lparam ) == HTCLIENT)
        {
            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            if (hit( pt ) == Z_VIEW && pic && (pic->width * zoom > view_rect.right - view_rect.left ||
                                               pic->height * zoom > view_rect.bottom - view_rect.top))
            {
                SetCursor( LoadCursorW( NULL, (LPCWSTR)(panning ? (UINT_PTR)IDC_SIZEALL : (UINT_PTR)IDC_HAND) ) );
                return TRUE;
            }
            SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_ARROW ) );
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    {
        int h = hit( pt );
        if (slideshow) { set_full( FALSE ); return 0; }
        if (h == Z_SLIDER)
        {
            slider_drag = TRUE;
            SetCapture( hwnd );
            slider_set( pt );
            return 0;
        }
        if (h == Z_FILM)
        {
            int i = film_hit( pt );
            if (i >= 0 && i != current) show_index( i );
            return 0;
        }
        if (h == Z_VIEW)
        {
            if (pic && (pic->width * zoom > view_rect.right - view_rect.left ||
                        pic->height * zoom > view_rect.bottom - view_rect.top))
            {
                panning = TRUE;
                pan_from = pt;
                pan_cx = center_x;
                pan_cy = center_y;
                SetCapture( hwnd );
                SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_SIZEALL ) );
            }
            return 0;
        }
        down = h;
        if (h != Z_NONE) SetCapture( hwnd );
        invalidate();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int was = down;
        down = Z_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (slider_drag) { slider_drag = FALSE; invalidate(); return 0; }
        if (panning) { panning = FALSE; return 0; }
        if (was != Z_NONE && hit( pt ) == was) click( was, pt );
        invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (hit( pt ) == Z_VIEW && pic)
        {
            if (fit && fit_zoom() < 1.0) set_zoom( 1.0, &pt );
            else zoom_fit();
        }
        else if (hit( pt ) == Z_NONE && pt.y < TITLE_H + TOOL_H && !full)
            PostMessageW( hwnd, WM_SYSCOMMAND, IsZoomed( hwnd ) ? SC_RESTORE : SC_MAXIMIZE, 0 );
        return 0;
    case WM_RBUTTONUP:
        if (hit( pt ) == Z_VIEW && current >= 0)
        {
            RECT anchor = { pt.x, pt.y, pt.x, pt.y };
            MapWindowPoints( hwnd, NULL, (POINT *)&anchor, 2 );
            more_menu( &anchor );
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        int delta = (short)HIWORD( wparam );
        ScreenToClient( hwnd, &pt );
        if (show_film && PtInRect( &zones[Z_FILM], pt ))
        {
            RECT rc;
            int total = px( 32 ) + folder.count * (film_item_w() + film_gap()) - film_gap();
            GetClientRect( hwnd, &rc );
            film_scroll = max( 0, min( film_scroll - delta * px( 80 ) / WHEEL_DELTA, total - (view_rect.right - view_rect.left) ) );
            invalidate();
            return 0;
        }
        /* the wheel zooms, round the mouse, as in Photos */
        if (pic) set_zoom( zoom * (delta > 0 ? 1.15 : 1 / 1.15), &pt );
        return 0;
    }
    case WM_KEYDOWN:
    {
        BOOL ctrl = GetKeyState( VK_CONTROL ) < 0;
        if (slideshow && wparam != VK_RIGHT && wparam != VK_LEFT) { set_full( FALSE ); return 0; }
        switch (wparam)
        {
        case VK_LEFT: case VK_PRIOR: case VK_UP:
            if (!fit && wparam == VK_UP) { center_y -= px( 60 ) / zoom; clamp_center(); view_changed(); }
            else go( -1 );
            break;
        case VK_RIGHT: case VK_NEXT: case VK_DOWN:
            if (!fit && wparam == VK_DOWN) { center_y += px( 60 ) / zoom; clamp_center(); view_changed(); }
            else go( 1 );
            break;
        case VK_HOME: show_index( 0 ); break;
        case VK_END: show_index( folder.count - 1 ); break;
        case VK_DELETE: delete_file(); break;
        case VK_ESCAPE: if (full) set_full( FALSE ); break;
        case VK_F11: set_full( !full ); break;
        case VK_F5: start_slideshow(); break;
        case VK_OEM_PLUS: case VK_ADD: if (ctrl || wparam == VK_ADD) zoom_step( 1, NULL ); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: if (ctrl || wparam == VK_SUBTRACT) zoom_step( -1, NULL ); break;
        case '0': if (ctrl) zoom_fit(); break;
        case '1': if (ctrl) set_zoom( 1.0, NULL ); break;
        case 'R': if (ctrl) rotate(); break;
        case 'E': if (ctrl) edit_picture(); break;
        case 'C': if (ctrl) copy_picture(); break;
        case 'P': if (ctrl) print_picture(); break;
        case 'O': if (ctrl) open_dialog(); break;
        case 'D': if (ctrl && current >= 0) { set_favorite( path, !is_favorite( path ) ); invalidate(); } break;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN)
        {
            click( Z_INFO, pt );
            return 0;
        }
        break;
    case WM_APP_THUMB:
        if (show_film) invalidate();
        return 0;
    case WM_DROPFILES:
    {
        WCHAR file[MAX_PATH];
        if (DragQueryFileW( (HDROP)wparam, 0, file, MAX_PATH )) open_path( file );
        DragFinish( (HDROP)wparam );
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lparam && !lstrcmpW( (const WCHAR *)lparam, L"ImmersiveColorSet" )) apply_theme();
        break;
    case WM_THEMECHANGED:
        apply_theme();
        break;
    case WM_ACTIVATE:
        invalidate();
        break;
    case WM_CLOSE:
        if (full) set_full( FALSE );
        {
            WINDOWPLACEMENT place = { sizeof(place) };
            GetWindowPlacement( hwnd, &place );
            set_setting( L"Left", place.rcNormalPosition.left );
            set_setting( L"Top", place.rcNormalPosition.top );
            set_setting( L"Width", place.rcNormalPosition.right - place.rcNormalPosition.left );
            set_setting( L"Height", place.rcNormalPosition.bottom - place.rcNormalPosition.top );
            set_setting( L"Maximized", place.showCmd == SW_SHOWMAXIMIZED );
        }
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSEXW cls = { sizeof(cls) };
    WCHAR **argv;
    int argc, x, y, w, h;
    const WCHAR *open = NULL;
    MSG msg;
    HDC hdc;

    instance = inst;
    SetProcessDPIAware();
    hdc = GetDC( 0 );
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    ReleaseDC( 0, hdc );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    InitCommonControlsEx( &controls );
    theme_load();
    menus_follow_theme();
    show_film = setting( L"Filmstrip", 1 ) != 0;
    show_info = setting( L"Info", 0 ) != 0;

    argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '/' || GetFileAttributesW( argv[i] ) != INVALID_FILE_ATTRIBUTES) open = argv[i];

    small_icon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_PHOTOS ), IMAGE_ICON, px( 16 ), px( 16 ), 0 );
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = frame_proc;
    cls.hInstance = inst;
    cls.hIcon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_PHOTOS ), IMAGE_ICON, GetSystemMetrics( SM_CXICON ),
                            GetSystemMetrics( SM_CYICON ), 0 );
    cls.hIconSm = small_icon;
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    cls.lpszClassName = L"ArcticPhotos";
    RegisterClassExW( &cls );

    w = setting( L"Width", px( 1100 ) );
    h = setting( L"Height", px( 760 ) );
    x = setting( L"Left", CW_USEDEFAULT );
    y = setting( L"Top", CW_USEDEFAULT );
    {
        RECT r = { x, y, x + w, y + h }, work;
        SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
        if (x == (int)CW_USEDEFAULT || !IntersectRect( &r, &r, &work ) || r.right - r.left < px( 200 ))
        {
            w = min( w, work.right - work.left );
            h = min( h, work.bottom - work.top );
            x = work.left + (work.right - work.left - w) / 2;
            y = work.top + (work.bottom - work.top - h) / 2;
        }
    }
    if (!CreateWindowExW( WS_EX_ACCEPTFILES, cls.lpszClassName, tr( L"Photos", L"Фотографії" ),
                          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, inst, NULL ))
        return 1;
    apply_theme();
    thumbs_start( frame );
    layout();
    ShowWindow( frame, setting( L"Maximized", 0 ) && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show );
    UpdateWindow( frame );
    if (open) open_path( open );
    title_changed();
    LocalFree( argv );

    while (GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    CoUninitialize();
    return (int)msg.wParam;
}
