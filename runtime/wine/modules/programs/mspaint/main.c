/*
 * Paint, as Windows 11 has it
 *
 * One window without the system's title bar: its own has the program's
 * picture, the File, Edit and View menus, Save, Undo and Redo, the name of
 * the picture and the window's buttons. Under it the tools (ribbon.c), the
 * picture (canvas.c) and the status bar.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "paint.h"
#include <wincodec.h>

HINSTANCE instance;
HWND frame;
UINT dpi = 96;
ULONGLONG file_size;

static HWND ribbon, canvas, status, textbar;
static struct surface title_surf;
static HICON small_icon;
static WCHAR file_path[MAX_PATH];
static GUID file_format;
static BOOL modified, quiet, status_shown = TRUE;

#define TITLE_H px( 44 )
#define STATUS_H px( 32 )
#define TEXTBAR_H px( 46 )

/**********************************************************************
 *          Strings and settings
 */

const WCHAR *tr( const WCHAR *en, const WCHAR *uk )
{
    static int ukrainian = -1;

    if (ukrainian < 0) ukrainian = PRIMARYLANGID( GetUserDefaultUILanguage() ) == LANG_UKRAINIAN;
    return ukrainian ? uk : en;
}

#define KEY L"Software\\Arctic\\Paint"

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

#define RECENT_MAX 8

static int recent_files( WCHAR paths[RECENT_MAX][MAX_PATH] )
{
    int n = 0;

    for (int i = 0; i < RECENT_MAX; i++)
    {
        WCHAR name[16];
        DWORD size = MAX_PATH * sizeof(WCHAR);
        swprintf( name, ARRAY_SIZE(name), L"File%d", i );
        if (!RegGetValueW( HKEY_CURRENT_USER, KEY L"\\Recent", name, RRF_RT_REG_SZ, NULL, paths[n], &size ) && paths[n][0])
            n++;
    }
    return n;
}

static void recent_add( const WCHAR *path )
{
    WCHAR paths[RECENT_MAX][MAX_PATH], out[RECENT_MAX][MAX_PATH];
    int n = recent_files( paths ), m = 0;

    lstrcpynW( out[m++], path, MAX_PATH );
    for (int i = 0; i < n && m < RECENT_MAX; i++)
        if (wcsicmp( paths[i], path )) lstrcpyW( out[m++], paths[i] );
    for (int i = 0; i < RECENT_MAX; i++)
    {
        WCHAR name[16];
        swprintf( name, ARRAY_SIZE(name), L"File%d", i );
        if (i < m) RegSetKeyValueW( HKEY_CURRENT_USER, KEY L"\\Recent", name, REG_SZ, out[i], (lstrlenW( out[i] ) + 1) * sizeof(WCHAR) );
        else RegDeleteKeyValueW( HKEY_CURRENT_USER, KEY L"\\Recent", name );
    }
}

/**********************************************************************
 *          The picture's file
 */

static const WCHAR *picture_name(void)
{
    const WCHAR *name;

    if (!file_path[0]) return tr( L"Untitled", L"Без назви" );
    name = wcsrchr( file_path, '\\' );
    return name ? name + 1 : file_path;
}

void frame_title_changed(void)
{
    WCHAR title[MAX_PATH + 32];

    swprintf( title, ARRAY_SIZE(title), L"%s - Paint", picture_name() );
    SetWindowTextW( frame, title );
}

void set_modified( BOOL value )
{
    if (modified == value) return;
    modified = value;
    if (frame) InvalidateRect( frame, NULL, FALSE );
}

static void update_file_size(void)
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    file_size = 0;
    if (file_path[0] && GetFileAttributesExW( file_path, GetFileExInfoStandard, &data ))
        file_size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    status_refresh();
}

static const GUID *format_of_path( const WCHAR *path )
{
    const WCHAR *ext = wcsrchr( path, '.' );

    if (!ext) return &GUID_ContainerFormatPng;
    if (!wcsicmp( ext, L".jpg" ) || !wcsicmp( ext, L".jpeg" ) || !wcsicmp( ext, L".jpe" ) || !wcsicmp( ext, L".jfif" ))
        return &GUID_ContainerFormatJpeg;
    if (!wcsicmp( ext, L".bmp" ) || !wcsicmp( ext, L".dib" )) return &GUID_ContainerFormatBmp;
    if (!wcsicmp( ext, L".gif" )) return &GUID_ContainerFormatGif;
    if (!wcsicmp( ext, L".tif" ) || !wcsicmp( ext, L".tiff" )) return &GUID_ContainerFormatTiff;
    if (!wcsicmp( ext, L".png" )) return &GUID_ContainerFormatPng;
    return NULL;
}

static void error_box( const WCHAR *what, const WCHAR *path )
{
    WCHAR text[MAX_PATH + 160];

    if (quiet) return;
    swprintf( text, ARRAY_SIZE(text), what, path );
    MessageBoxW( frame, text, L"Paint", MB_OK | MB_ICONWARNING );
}

static void new_picture(void)
{
    canvas_cancel();
    history_clear();
    set_picture( image_create( setting( L"NewWidth", 1152 ), setting( L"NewHeight", 648 ), 0xffffffff ) );
    file_path[0] = 0;
    file_format = GUID_ContainerFormatPng;
    modified = FALSE;
    update_file_size();
    frame_title_changed();
    canvas_fit();
    InvalidateRect( frame, NULL, FALSE );
}

void open_file( const WCHAR *path )
{
    struct image *image;
    GUID format = GUID_ContainerFormatPng;
    WCHAR full[MAX_PATH];

    if (!GetFullPathNameW( path, MAX_PATH, full, NULL )) lstrcpynW( full, path, MAX_PATH );
    if (!(image = image_load( full, &format )))
    {
        error_box( tr( L"Paint cannot read this file.\n\n%s", L"Paint не може прочитати цей файл.\n\n%s" ), full );
        return;
    }
    canvas_cancel();
    history_clear();
    set_picture( image );
    lstrcpyW( file_path, full );
    file_format = format;
    modified = FALSE;
    recent_add( full );
    update_file_size();
    frame_title_changed();
    canvas_fit();
    InvalidateRect( frame, NULL, FALSE );
}

static BOOL save_to( const WCHAR *path, const GUID *format )
{
    canvas_commit();
    if (!image_opaque( pic ) && (IsEqualGUID( format, &GUID_ContainerFormatJpeg ) ||
        IsEqualGUID( format, &GUID_ContainerFormatBmp ) || IsEqualGUID( format, &GUID_ContainerFormatGif )) && !quiet)
    {
        if (MessageBoxW( frame, tr( L"This format has no transparency: transparent parts will be saved white. Save anyway?",
                                    L"Цей формат не має прозорості: прозорі частини буде збережено білими. Зберегти?" ),
                         L"Paint", MB_OKCANCEL | MB_ICONINFORMATION ) != IDOK)
            return FALSE;
    }
    if (!image_save( pic, path, format ))
    {
        error_box( tr( L"The picture could not be saved.\n\n%s", L"Не вдалося зберегти зображення.\n\n%s" ), path );
        return FALSE;
    }
    if (path != file_path) lstrcpynW( file_path, path, MAX_PATH );
    file_format = *format;
    modified = FALSE;
    recent_add( file_path );
    update_file_size();
    frame_title_changed();
    InvalidateRect( frame, NULL, FALSE );
    return TRUE;
}

static BOOL save_as( const GUID *want )
{
    static const struct { const GUID *format; const WCHAR *ext; } types[] =
    {
        { &GUID_ContainerFormatPng, L"png" }, { &GUID_ContainerFormatJpeg, L"jpg" }, { &GUID_ContainerFormatBmp, L"bmp" },
        { &GUID_ContainerFormatGif, L"gif" }, { &GUID_ContainerFormatTiff, L"tif" },
    };
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR path[MAX_PATH], filter[512], *p = filter;
    const GUID *format;
    int index = 1;

    if (!want) want = file_path[0] && format_of_path( file_path ) ? format_of_path( file_path ) : &GUID_ContainerFormatPng;
    for (int i = 0; i < ARRAY_SIZE(types); i++) if (IsEqualGUID( want, types[i].format )) index = i + 1;
    p += swprintf( p, 128, L"PNG (*.png)%c*.png%c", 0, 0 );
    p += swprintf( p, 128, L"JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)%c*.jpg;*.jpeg;*.jpe;*.jfif%c", 0, 0 );
    p += swprintf( p, 128, L"BMP (*.bmp;*.dib)%c*.bmp;*.dib%c", 0, 0 );
    p += swprintf( p, 128, L"GIF (*.gif)%c*.gif%c", 0, 0 );
    p += swprintf( p, 128, L"TIFF (*.tif;*.tiff)%c*.tif;*.tiff%c", 0, 0 );
    *p = 0;

    if (file_path[0])
    {
        WCHAR *dot;
        lstrcpynW( path, file_path, MAX_PATH );
        if ((dot = wcsrchr( path, '.' )) && dot > wcsrchr( path, '\\' )) *dot = 0;
    }
    else lstrcpyW( path, tr( L"Untitled", L"Без назви" ) );
    ofn.hwndOwner = frame;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = index;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = types[index - 1].ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetSaveFileNameW( &ofn )) return FALSE;
    format = format_of_path( path );
    if (!format || (ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= ARRAY_SIZE(types) &&
                    !IsEqualGUID( format, types[ofn.nFilterIndex - 1].format )))
        format = types[max( 1, min( (int)ofn.nFilterIndex, (int)ARRAY_SIZE(types) ) ) - 1].format;
    return save_to( path, format );
}

static BOOL save(void)
{
    const GUID *format = file_path[0] ? format_of_path( file_path ) : NULL;

    if (!format) return save_as( NULL );
    return save_to( file_path, format );
}

static BOOL ask_save_changes(void)
{
    WCHAR text[MAX_PATH + 64];
    int answer;

    if (!modified) return TRUE;
    swprintf( text, ARRAY_SIZE(text), tr( L"Do you want to save changes to %s?", L"Зберегти зміни в «%s»?" ), picture_name() );
    answer = MessageBoxW( frame, text, L"Paint", MB_YESNOCANCEL | MB_ICONWARNING );
    if (answer == IDCANCEL) return FALSE;
    if (answer == IDYES) return save();
    return TRUE;
}

static BOOL ask_open( WCHAR *path, const WCHAR *title )
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[512], *p = filter;

    p += swprintf( p, 160, L"%s%c*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.heic;*.webp%c",
                   tr( L"All picture files", L"Усі файли зображень" ), 0, 0 );
    p += swprintf( p, 64, L"PNG (*.png)%c*.png%c", 0, 0 );
    p += swprintf( p, 96, L"JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)%c*.jpg;*.jpeg;*.jpe;*.jfif%c", 0, 0 );
    p += swprintf( p, 64, L"BMP (*.bmp;*.dib)%c*.bmp;*.dib%c", 0, 0 );
    p += swprintf( p, 64, L"GIF (*.gif)%c*.gif%c", 0, 0 );
    p += swprintf( p, 64, L"TIFF (*.tif;*.tiff)%c*.tif;*.tiff%c", 0, 0 );
    p += swprintf( p, 64, L"%s%c*.*%c", tr( L"All files", L"Усі файли" ), 0, 0 );
    *p = 0;
    path[0] = 0;
    ofn.hwndOwner = frame;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    return GetOpenFileNameW( &ofn );
}

/**********************************************************************
 *          Printing, the desktop's background, colours
 */

static void print_picture(void)
{
    PRINTDLGW pd = { sizeof(pd) };
    DOCINFOW doc = { sizeof(doc) };
    struct image *flat;

    canvas_commit();
    pd.hwndOwner = frame;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
    if (!PrintDlgW( &pd ) || !pd.hDC) return;
    doc.lpszDocName = picture_name();
    if ((flat = image_copy( pic )))
    {
        image_flatten( flat, 0xffffffff );
        if (StartDocW( pd.hDC, &doc ) > 0 && StartPage( pd.hDC ) > 0)
        {
            int pw = GetDeviceCaps( pd.hDC, HORZRES ), ph = GetDeviceCaps( pd.hDC, VERTRES );
            int dx = GetDeviceCaps( pd.hDC, LOGPIXELSX ), dy = GetDeviceCaps( pd.hDC, LOGPIXELSY );
            double w = flat->width * dx / 96.0, h = flat->height * dy / 96.0, s = min( 1.0, min( pw / w, ph / h ) );
            BITMAPINFO info = { { sizeof(info.bmiHeader), flat->width, -flat->height, 1, 32 } };
            SetStretchBltMode( pd.hDC, HALFTONE );
            StretchDIBits( pd.hDC, 0, 0, (int)(w * s), (int)(h * s), 0, 0, flat->width, flat->height, flat->bits, &info,
                           DIB_RGB_COLORS, SRCCOPY );
            EndPage( pd.hDC );
            EndDoc( pd.hDC );
        }
        image_free( flat );
    }
    DeleteDC( pd.hDC );
    if (pd.hDevMode) GlobalFree( pd.hDevMode );
    if (pd.hDevNames) GlobalFree( pd.hDevNames );
}

static void set_wallpaper( int how )
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    const WCHAR *style = how == IDM_WALL_FILL ? L"10" : L"0", *tile = how == IDM_WALL_TILE ? L"1" : L"0";
    struct image *flat;
    BOOL ok;

    canvas_commit();
    if (!GetEnvironmentVariableW( L"APPDATA", dir, MAX_PATH )) return;
    swprintf( path, ARRAY_SIZE(path), L"%s\\Microsoft\\Windows\\Themes", dir );
    SHCreateDirectoryExW( NULL, path, NULL );
    lstrcatW( path, L"\\Paint Wallpaper.bmp" );
    if (!(flat = image_copy( pic ))) return;
    image_flatten( flat, 0xffffffff );
    ok = image_save( flat, path, &GUID_ContainerFormatBmp );
    image_free( flat );
    if (!ok) return;
    RegSetKeyValueW( HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", REG_SZ, style, (lstrlenW( style ) + 1) * 2 );
    RegSetKeyValueW( HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", REG_SZ, tile, (lstrlenW( tile ) + 1) * 2 );
    SystemParametersInfoW( SPI_SETDESKWALLPAPER, 0, path, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
}

static void edit_colors(void)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };

    cc.hwndOwner = frame;
    cc.rgbResult = tools.color1;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW( &cc )) return;
    tools.color1 = cc.rgbResult;
    /* the new colour first in the row of custom ones */
    for (int i = 0; i < tools.custom_count; i++)
        if (tools.custom[i] == cc.rgbResult)
        {
            memmove( tools.custom + i, tools.custom + i + 1, (tools.custom_count - i - 1) * sizeof(COLORREF) );
            tools.custom_count--;
            break;
        }
    memmove( tools.custom + 1, tools.custom, min( tools.custom_count, 9 ) * sizeof(COLORREF) );
    tools.custom[0] = cc.rgbResult;
    tools.custom_count = min( tools.custom_count + 1, 10 );
    canvas_style_changed();
    ribbon_refresh();
}

/**********************************************************************
 *          Full screen: the picture alone, until a click or a key
 */

static LRESULT CALLBACK full_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT rc;
        struct surface s = { 0 };
        GetClientRect( hwnd, &rc );
        if (surface_size( &s, rc.right, rc.bottom ))
        {
            double z = min( 1.0, min( (double)rc.right / pic->width, (double)rc.bottom / pic->height ) );
            fill( &s, &rc, RGB( 0, 0, 0 ) );
            blit_image( &s, &rc, pic->bits, pic->width, pic->height, floor( (rc.right - pic->width * z) / 2 ),
                        floor( (rc.bottom - pic->height * z) / 2 ), z, 0 );
            GdiFlush();
            BitBlt( hdc, 0, 0, rc.right, rc.bottom, s.dc, 0, 0, SRCCOPY );
            surface_free( &s );
        }
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        DestroyWindow( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void full_screen(void)
{
    WNDCLASSW cls = { 0, full_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintFull" };
    MONITORINFO mi = { sizeof(mi) };
    HWND hwnd;

    canvas_commit();
    RegisterClassW( &cls );
    GetMonitorInfoW( MonitorFromWindow( frame, MONITOR_DEFAULTTONEAREST ), &mi );
    hwnd = CreateWindowExW( WS_EX_TOPMOST, cls.lpszClassName, L"Paint", WS_POPUP | WS_VISIBLE, mi.rcMonitor.left,
                            mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                            frame, NULL, instance, NULL );
    SetFocus( hwnd );
}

/**********************************************************************
 *          Commands
 */

BOOL command_enabled( int id )
{
    switch (id)
    {
    case IDM_UNDO: return can_undo();
    case IDM_REDO: return can_redo();
    case IDM_CUT:
    case IDM_COPY:
    case IDM_CROP:
    case IDM_DELETE: return canvas_has_selection() && !canvas_text_active();
    case IDM_PASTE: return clipboard_has_image() || (canvas_text_active() && IsClipboardFormatAvailable( CF_UNICODETEXT ));
    }
    return TRUE;
}

BOOL command_checked( int id )
{
    switch (id)
    {
    case IDM_GRID: return show_grid;
    case IDM_STATUSBAR: return status_shown;
    case IDM_TRANSPARENT: return tools.transparent_select;
    }
    return FALSE;
}

static void recent_menu( struct menu_item *items, WCHAR names[RECENT_MAX][MAX_PATH], int *count )
{
    WCHAR paths[RECENT_MAX][MAX_PATH];
    int n = recent_files( paths );

    *count = 0;
    for (int i = 0; i < n; i++)
    {
        const WCHAR *name = wcsrchr( paths[i], '\\' );
        lstrcpynW( names[i], name ? name + 1 : paths[i], MAX_PATH );
        memset( &items[i], 0, sizeof(items[i]) );
        items[i].id = IDM_RECENT_FIRST + i;
        items[i].text = names[i];
        (*count)++;
    }
    if (!n)
    {
        memset( &items[0], 0, sizeof(items[0]) );
        items[0].id = IDM_RECENT_FIRST + 9;
        items[0].text = tr( L"No recent files", L"Немає останніх файлів" );
        items[0].flags = MFS_DISABLED;
        *count = 1;
    }
}

static void file_menu( const RECT *anchor )
{
    struct menu_item recent[RECENT_MAX];
    WCHAR names[RECENT_MAX][MAX_PATH];
    int recent_count, id;
    struct menu_item save_items[] =
    {
        { IDM_SAVE_PNG, L"PNG", NULL }, { IDM_SAVE_JPEG, L"JPEG", NULL }, { IDM_SAVE_BMP, L"BMP", NULL },
        { IDM_SAVE_GIF, L"GIF", NULL }, { 0 }, { IDM_SAVEAS, tr( L"Other format", L"Інший формат" ), NULL, L"F12" },
    };
    struct menu_item wall_items[] =
    {
        { IDM_WALL_FILL, tr( L"Fill", L"Заповнення" ) }, { IDM_WALL_TILE, tr( L"Tile", L"Мозаїка" ) },
        { IDM_WALL_CENTER, tr( L"Centre", L"По центру" ) },
    };
    struct menu_item items[16];
    int n = 0;

    recent_menu( recent, names, &recent_count );
    memset( items, 0, sizeof(items) );
    items[n++] = (struct menu_item){ IDM_NEW, tr( L"New", L"Створити" ), "new", L"Ctrl+N" };
    items[n++] = (struct menu_item){ IDM_OPEN, tr( L"Open", L"Відкрити" ), "open", L"Ctrl+O" };
    items[n++] = (struct menu_item){ IDM_IMPORT, tr( L"Import to canvas", L"Імпортувати на полотно" ), "import" };
    items[n++] = (struct menu_item){ -1, tr( L"Recent", L"Останні" ), "recent", NULL, 0, recent, recent_count };
    items[n++] = (struct menu_item){ 0 };
    items[n++] = (struct menu_item){ IDM_SAVE, tr( L"Save", L"Зберегти" ), "save", L"Ctrl+S" };
    items[n++] = (struct menu_item){ -1, tr( L"Save as", L"Зберегти як" ), "saveas", NULL, 0, save_items, ARRAY_SIZE(save_items) };
    items[n++] = (struct menu_item){ 0 };
    items[n++] = (struct menu_item){ IDM_PRINT, tr( L"Print", L"Друк" ), "print", L"Ctrl+P" };
    items[n++] = (struct menu_item){ -1, tr( L"Set as desktop background", L"Зробити фоном робочого стола" ), "wallpaper", NULL, 0,
                                     wall_items, ARRAY_SIZE(wall_items) };
    items[n++] = (struct menu_item){ IDM_PROPERTIES, tr( L"Image properties", L"Властивості зображення" ), "properties", L"Ctrl+E" };
    items[n++] = (struct menu_item){ 0 };
    items[n++] = (struct menu_item){ IDM_ABOUT, tr( L"About Paint", L"Про Paint" ), "about" };
    items[n++] = (struct menu_item){ IDM_EXIT, tr( L"Exit", L"Вийти" ), NULL };
    for (int i = 0; i < n; i++) if (items[i].id == -1) items[i].id = 0;
    if ((id = menu_run( frame, anchor, items, n ))) command( id );
}

static void edit_menu( const RECT *anchor )
{
    struct menu_item items[] =
    {
        { IDM_UNDO, tr( L"Undo", L"Скасувати" ), "undo", L"Ctrl+Z", command_enabled( IDM_UNDO ) ? 0 : MFS_DISABLED },
        { IDM_REDO, tr( L"Redo", L"Повторити" ), "redo", L"Ctrl+Y", command_enabled( IDM_REDO ) ? 0 : MFS_DISABLED },
        { 0 },
        { IDM_CUT, tr( L"Cut", L"Вирізати" ), "cut", L"Ctrl+X", command_enabled( IDM_CUT ) ? 0 : MFS_DISABLED },
        { IDM_COPY, tr( L"Copy", L"Копіювати" ), "copy", L"Ctrl+C", command_enabled( IDM_COPY ) ? 0 : MFS_DISABLED },
        { IDM_PASTE, tr( L"Paste", L"Вставити" ), "paste", L"Ctrl+V", command_enabled( IDM_PASTE ) ? 0 : MFS_DISABLED },
        { IDM_PASTE_FROM, tr( L"Paste from", L"Вставити з" ), "import" },
        { 0 },
        { IDM_SELECT_ALL, tr( L"Select all", L"Виділити все" ), "selectall", L"Ctrl+A" },
    };
    int id = menu_run( frame, anchor, items, ARRAY_SIZE(items) );
    if (id) command( id );
}

static void view_menu( const RECT *anchor )
{
    struct menu_item items[] =
    {
        { IDM_ZOOM_IN, tr( L"Zoom in", L"Збільшити" ), "zoomin", L"Ctrl+Plus" },
        { IDM_ZOOM_OUT, tr( L"Zoom out", L"Зменшити" ), "zoomout", L"Ctrl+Minus" },
        { IDM_ZOOM_100, L"100%", "zoom100", L"Ctrl+1" },
        { IDM_ZOOM_FIT, tr( L"Fit to window", L"За розміром вікна" ), "fit", L"Ctrl+Shift+0" },
        { 0 },
        { IDM_GRID, tr( L"Gridlines", L"Лінії сітки" ), "grid", L"Ctrl+G", show_grid ? MFS_CHECKED : 0 },
        { IDM_STATUSBAR, tr( L"Status bar", L"Рядок стану" ), NULL, NULL, status_shown ? MFS_CHECKED : 0 },
        { 0 },
        { IDM_FULLSCREEN, tr( L"Full screen", L"На весь екран" ), "fullscreen", L"F11" },
    };
    int id = menu_run( frame, anchor, items, ARRAY_SIZE(items) );
    if (id) command( id );
}

static void settings_menu( const RECT *anchor )
{
    DWORD mode = setting( L"Theme", 0 );
    struct menu_item items[] =
    {
        { IDM_THEME_LIGHT, tr( L"Light", L"Світла тема" ), NULL, NULL, (mode == 1 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        { IDM_THEME_DARK, tr( L"Dark", L"Темна тема" ), NULL, NULL, (mode == 2 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        { IDM_THEME_SYSTEM, tr( L"Use system setting", L"Як у системі" ), NULL, NULL, (mode == 0 ? MFS_CHECKED : 0) | MFT_RADIOCHECK },
        { 0 },
        { IDM_ABOUT, tr( L"About Paint", L"Про Paint" ), "about" },
    };
    int id = menu_run( frame, anchor, items, ARRAY_SIZE(items) );
    if (id) command( id );
}

static void apply_theme(void);

void command( int id )
{
    if (id >= IDM_TOOL_FIRST && id < IDM_TOOL_FIRST + TOOL_COUNT)
    {
        set_tool( id - IDM_TOOL_FIRST );
        return;
    }
    if (id >= IDM_BRUSH_FIRST && id < IDM_BRUSH_FIRST + BR_COUNT)
    {
        tools.brush = id - IDM_BRUSH_FIRST;
        set_tool( TOOL_BRUSH );
        ribbon_refresh();
        return;
    }
    if (id >= IDM_SHAPE_FIRST && id < IDM_SHAPE_FIRST + SH_COUNT)
    {
        canvas_commit();
        tools.shape = id - IDM_SHAPE_FIRST;
        set_tool( TOOL_SHAPE );
        ribbon_refresh();
        return;
    }
    if (id >= IDM_RECENT_FIRST && id < IDM_RECENT_FIRST + RECENT_MAX)
    {
        WCHAR paths[RECENT_MAX][MAX_PATH];
        int n = recent_files( paths );
        if (id - IDM_RECENT_FIRST < n && ask_save_changes()) open_file( paths[id - IDM_RECENT_FIRST] );
        return;
    }

    switch (id)
    {
    case IDM_NEW:
        if (ask_save_changes()) new_picture();
        break;
    case IDM_OPEN:
    {
        WCHAR path[MAX_PATH];
        if (ask_save_changes() && ask_open( path, NULL )) open_file( path );
        break;
    }
    case IDM_IMPORT:
    case IDM_PASTE_FROM:
    {
        WCHAR path[MAX_PATH];
        struct image *image;
        if (!ask_open( path, NULL )) break;
        if ((image = image_load( path, NULL ))) canvas_paste( image );
        else error_box( tr( L"Paint cannot read this file.\n\n%s", L"Paint не може прочитати цей файл.\n\n%s" ), path );
        break;
    }
    case IDM_SAVE: save(); break;
    case IDM_SAVEAS: save_as( NULL ); break;
    case IDM_SAVE_PNG: save_as( &GUID_ContainerFormatPng ); break;
    case IDM_SAVE_JPEG: save_as( &GUID_ContainerFormatJpeg ); break;
    case IDM_SAVE_BMP: save_as( &GUID_ContainerFormatBmp ); break;
    case IDM_SAVE_GIF: save_as( &GUID_ContainerFormatGif ); break;
    case IDM_PRINT: print_picture(); break;
    case IDM_WALL_FILL:
    case IDM_WALL_TILE:
    case IDM_WALL_CENTER: set_wallpaper( id ); break;
    case IDM_PROPERTIES: canvas_commit(); dialog_properties( frame ); break;
    case IDM_ABOUT: ShellAboutW( frame, L"Paint", NULL, LoadIconW( instance, MAKEINTRESOURCEW( IDI_PAINT ) ) ); break;
    case IDM_EXIT: PostMessageW( frame, WM_CLOSE, 0, 0 ); break;

    case IDM_UNDO: undo(); break;
    case IDM_REDO: redo(); break;
    case IDM_CUT:
    case IDM_COPY:
        if (!canvas_text_active()) canvas_copy( id == IDM_CUT );
        break;
    case IDM_PASTE:
        if (canvas_text_active()) canvas_text_paste();
        else canvas_paste( clipboard_get( frame ) );
        break;
    case IDM_SELECT_ALL:
        if (!canvas_text_active()) canvas_select_all();
        break;
    case IDM_INVERT_SEL: canvas_invert_selection(); break;
    case IDM_DELETE: canvas_delete_selection(); break;
    case IDM_TRANSPARENT: tools.transparent_select = !tools.transparent_select; break;
    case IDM_SEL_RECT:
    case IDM_SEL_FREE:
        tools.select = id == IDM_SEL_FREE ? SEL_FREE : SEL_RECT;
        canvas_commit();
        set_tool( TOOL_SELECT );
        ribbon_refresh();
        break;
    case IDM_CROP: canvas_crop(); break;
    case IDM_RESIZE: dialog_resize( frame ); break;
    case IDM_ROTATE_RIGHT:
    case IDM_ROTATE_LEFT:
    case IDM_ROTATE_180:
    case IDM_FLIP_V:
    case IDM_FLIP_H: canvas_rotate( id ); break;
    case IDM_ESCAPE: canvas_commit(); break;

    case IDM_ZOOM_IN: canvas_zoom_step( 1, NULL ); break;
    case IDM_ZOOM_OUT: canvas_zoom_step( -1, NULL ); break;
    case IDM_ZOOM_100: canvas_set_zoom( 1.0, NULL ); break;
    case IDM_ZOOM_FIT: canvas_fit(); break;
    case IDM_GRID: show_grid = !show_grid; canvas_refresh(); break;
    case IDM_STATUSBAR:
        status_shown = !status_shown;
        set_setting( L"StatusBar", status_shown );
        ShowWindow( status, status_shown ? SW_SHOWNA : SW_HIDE );
        layout_frame();
        break;
    case IDM_FULLSCREEN: full_screen(); break;
    case IDM_THEME_LIGHT: set_setting( L"Theme", 1 ); apply_theme(); break;
    case IDM_THEME_DARK: set_setting( L"Theme", 2 ); apply_theme(); break;
    case IDM_THEME_SYSTEM: set_setting( L"Theme", 0 ); apply_theme(); break;

    case IDM_OUTLINE_NONE: tools.outline = ST_NONE; if (tools.fill == ST_NONE) tools.fill = ST_SOLID; canvas_style_changed(); break;
    case IDM_OUTLINE_SOLID: tools.outline = ST_SOLID; canvas_style_changed(); break;
    case IDM_FILL_NONE: tools.fill = ST_NONE; if (tools.outline == ST_NONE) tools.outline = ST_SOLID; canvas_style_changed(); break;
    case IDM_FILL_SOLID: tools.fill = ST_SOLID; canvas_style_changed(); break;
    case IDM_EDIT_COLORS: edit_colors(); break;
    case IDM_SWAP_COLORS:
    {
        COLORREF c = tools.color1;
        tools.color1 = tools.color2;
        tools.color2 = c;
        canvas_style_changed();
        break;
    }
    }
    ribbon_refresh();
    InvalidateRect( frame, NULL, FALSE );
}

/**********************************************************************
 *          The title bar
 */

enum { T_NONE, T_FILE, T_EDIT, T_VIEW, T_SAVE, T_UNDO, T_REDO, T_SETTINGS, T_MIN, T_MAX, T_CLOSE, T_ICON, T_COUNT };
static int title_hot, title_down;
static RECT title_zones[T_COUNT];

static void title_layout( int width )
{
    int x = px( 44 ), h = px( 32 ), y = (TITLE_H - h) / 2;
    const WCHAR *names[3] = { tr( L"File", L"Файл" ), tr( L"Edit", L"Правка" ), tr( L"View", L"Вигляд" ) };

    SetRect( &title_zones[T_ICON], px( 14 ), (TITLE_H - px( 16 )) / 2, px( 30 ), (TITLE_H + px( 16 )) / 2 );
    for (int i = 0; i < 3; i++)
    {
        int w = text_width( get_font( F_UI ), names[i], -1 ) + px( 22 );
        SetRect( &title_zones[T_FILE + i], x, y, x + w, y + h );
        x += w + px( 2 );
    }
    x += px( 13 );
    for (int i = T_SAVE; i <= T_REDO; i++)
    {
        SetRect( &title_zones[i], x, y, x + px( 36 ), y + h );
        x += px( 38 );
    }
    /* the window's buttons as File Explorer's: 46 by 32, at the top */
    SetRect( &title_zones[T_CLOSE], width - px( 46 ), 0, width, px( 32 ) );
    SetRect( &title_zones[T_MAX], width - px( 92 ), 0, width - px( 46 ), px( 32 ) );
    SetRect( &title_zones[T_MIN], width - px( 138 ), 0, width - px( 92 ), px( 32 ) );
    SetRect( &title_zones[T_SETTINGS], width - px( 138 ) - px( 44 ), y, width - px( 138 ) - px( 8 ), y + h );
}

static void title_paint( HDC hdc )
{
    RECT client, r;
    BOOL active = GetActiveWindow() == frame;
    COLORREF text = active ? th.text : th.dim;
    const WCHAR *names[3] = { tr( L"File", L"Файл" ), tr( L"Edit", L"Правка" ), tr( L"View", L"Вигляд" ) };
    static const char *icons[3] = { "save", "undo", "redo" };
    int icon = px( 20 );

    GetClientRect( frame, &client );
    client.bottom = TITLE_H;
    title_layout( client.right );
    if (!surface_size( &title_surf, client.right, client.bottom )) return;
    fill( &title_surf, &client, th.chrome );

    if (small_icon)
    {
        GdiFlush();
        DrawIconEx( title_surf.dc, title_zones[T_ICON].left, title_zones[T_ICON].top, small_icon, px( 16 ), px( 16 ), 0, NULL, DI_NORMAL );
    }
    for (int i = 0; i < 3; i++)
    {
        r = title_zones[T_FILE + i];
        if (title_down == T_FILE + i) fill_round( &title_surf, &r, (float)px( 4 ), th.pressed, 255 );
        else if (title_hot == T_FILE + i) fill_round( &title_surf, &r, (float)px( 4 ), th.hover, 255 );
        draw_text( &title_surf, names[i], -1, &r, get_font( F_UI ), text, DT_CENTER );
    }
    r = title_zones[T_SAVE];
    SetRect( &r, r.left - px( 8 ), r.top + px( 8 ), r.left - px( 7 ), r.bottom - px( 8 ) );
    fill( &title_surf, &r, th.bar_line );
    for (int i = T_SAVE; i <= T_REDO; i++)
    {
        int cmd = i == T_SAVE ? IDM_SAVE : i == T_UNDO ? IDM_UNDO : IDM_REDO;
        BOOL enabled = command_enabled( cmd );
        r = title_zones[i];
        if (enabled && title_down == i) fill_round( &title_surf, &r, (float)px( 4 ), th.pressed, 255 );
        else if (enabled && title_hot == i) fill_round( &title_surf, &r, (float)px( 4 ), th.hover, 255 );
        draw_icon( &title_surf, icons[i - T_SAVE], (r.left + r.right - icon) / 2, (r.top + r.bottom - icon) / 2, icon,
                   enabled ? IS_NORMAL : IS_DISABLED );
    }
    /* the picture's name, and whether it is saved */
    {
        WCHAR name[MAX_PATH + 8];
        int x = title_zones[T_REDO].right + px( 16 ), limit = title_zones[T_SETTINGS].left - px( 16 );
        swprintf( name, ARRAY_SIZE(name), L"%s%s", picture_name(), modified ? L" •" : L"" );
        SetRect( &r, x, 0, limit, TITLE_H );
        if (limit > x + px( 40 )) draw_text( &title_surf, name, -1, &r, get_font( F_TITLE ), th.dim, DT_LEFT );
    }
    r = title_zones[T_SETTINGS];
    if (title_down == T_SETTINGS) fill_round( &title_surf, &r, (float)px( 4 ), th.pressed, 255 );
    else if (title_hot == T_SETTINGS) fill_round( &title_surf, &r, (float)px( 4 ), th.hover, 255 );
    draw_icon( &title_surf, "settings", (r.left + r.right - icon) / 2, (r.top + r.bottom - icon) / 2, icon, IS_NORMAL );

    for (int i = T_MIN; i <= T_CLOSE; i++)
    {
        int glyph = i == T_MIN ? G_MIN : i == T_CLOSE ? G_CLOSE : IsZoomed( frame ) ? G_RESTORE : G_MAX;
        COLORREF color = text;

        r = title_zones[i];
        if (title_hot == i && i == T_CLOSE)
        {
            fill( &title_surf, &r, title_down == i ? RGB( 0xc8, 0x3c, 0x2e ) : RGB( 0xc4, 0x2b, 0x1c ) );
            color = RGB( 255, 255, 255 );
        }
        else if (title_hot == i) fill( &title_surf, &r, title_down == i ? th.pressed : th.hover );
        draw_glyph( &title_surf, glyph, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2, px( 16 ), color );
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, client.right, client.bottom, title_surf.dc, 0, 0, SRCCOPY );
}

static int title_hit( POINT pt )
{
    if (pt.y >= TITLE_H) return T_NONE;
    for (int i = T_FILE; i <= T_CLOSE; i++)
        if (PtInRect( &title_zones[i], pt )) return i;
    return T_NONE;
}

static void invalidate_title(void)
{
    RECT r;
    GetClientRect( frame, &r );
    r.bottom = TITLE_H;
    InvalidateRect( frame, &r, FALSE );
}

static void title_click( int zone )
{
    RECT anchor = title_zones[zone];

    MapWindowPoints( frame, NULL, (POINT *)&anchor, 2 );
    switch (zone)
    {
    case T_FILE: file_menu( &anchor ); break;
    case T_EDIT: edit_menu( &anchor ); break;
    case T_VIEW: view_menu( &anchor ); break;
    case T_SETTINGS: settings_menu( &anchor ); break;
    case T_SAVE: command( IDM_SAVE ); break;
    case T_UNDO: command( IDM_UNDO ); break;
    case T_REDO: command( IDM_REDO ); break;
    case T_MIN: PostMessageW( frame, WM_SYSCOMMAND, SC_MINIMIZE, 0 ); break;
    case T_MAX: PostMessageW( frame, WM_SYSCOMMAND, IsZoomed( frame ) ? SC_RESTORE : SC_MAXIMIZE, 0 ); break;
    case T_CLOSE: PostMessageW( frame, WM_CLOSE, 0, 0 ); break;
    }
}

/**********************************************************************
 *          The window
 */

void layout_frame(void)
{
    RECT client;
    int top = TITLE_H, rib = ribbon_height(), tb = textbar_shown() ? TEXTBAR_H : 0, st = status_shown ? STATUS_H : 0;

    if (!frame) return;
    GetClientRect( frame, &client );
    title_layout( client.right );
    SetWindowPos( ribbon, NULL, 0, top, client.right, rib, SWP_NOZORDER | SWP_NOACTIVATE );
    if (tb) SetWindowPos( textbar, NULL, 0, top + rib, client.right, tb, SWP_NOZORDER | SWP_NOACTIVATE );
    SetWindowPos( canvas, NULL, 0, top + rib + tb, client.right, max( client.bottom - top - rib - tb - st, 0 ),
                  SWP_NOZORDER | SWP_NOACTIVATE );
    if (st) SetWindowPos( status, NULL, 0, client.bottom - st, client.right, st, SWP_NOZORDER | SWP_NOACTIVATE );
    invalidate_title();
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

/* menus of the system dark or light with the program (uxtheme's unnamed
 * SetPreferredAppMode, 135, and FlushMenuThemes, 136) */
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

/* Arctic's dwm draws a backdrop under the title bars uxtheme draws; this
 * window draws its own (DWMWA_ARCTIC_CAPTION, height 0: none) */
static void no_caption_backdrop( HWND hwnd )
{
    UINT32 caption[5] = { 0 };

    DwmSetWindowAttribute( hwnd, 0x1005, caption, sizeof(caption) );
}

static void apply_theme(void)
{
    BOOL dark;

    theme_load();
    menus_follow_theme();
    dark = th.dark;
    DwmSetWindowAttribute( frame, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark) );
    fonts_reset();
    ribbon_theme_changed();
    canvas_theme_changed();
    status_theme_changed();
    textbar_theme_changed();
    paint_frame_edges( frame );
    InvalidateRect( frame, NULL, FALSE );
}

static LRESULT CALLBACK frame_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_CREATE:
        frame = hwnd;
        ribbon = ribbon_create( hwnd );
        textbar = textbar_create( hwnd );
        if (!(canvas = canvas_create( hwnd ))) return -1;
        status = status_create( hwnd );
        if (!status_shown) ShowWindow( status, SW_HIDE );
        no_caption_backdrop( hwnd );
        return 0;

    case WM_NCCALCSIZE:
    {
        RECT *rect = wparam ? &((NCCALCSIZE_PARAMS *)lparam)->rgrc[0] : (RECT *)lparam;
        int top = rect->top;
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );

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

        if (GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &monitor ))
        {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left - bx;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top - by;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left + 2 * bx;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top + 2 * by;
        }
        info->ptMinTrackSize.x = px( 520 );
        info->ptMinTrackSize.y = px( 360 );
        return result;
    }

    case WM_NCPAINT:
        paint_frame_edges( hwnd );
        return 0;

    case WM_NCACTIVATE:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, (LPARAM)-1 );
        no_caption_backdrop( hwnd );
        paint_frame_edges( hwnd );
        invalidate_title();
        return result;
    }

    case WM_SETTEXT:
    case WM_SETICON:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );
        paint_frame_edges( hwnd );
        invalidate_title();
        return result;
    }

    case WM_NCHITTEST:
    {
        LRESULT hit;
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        RECT rect;
        int zone;

        /* the title bar's buttons are this window's own */
        ScreenToClient( hwnd, &pt );
        zone = title_hit( pt );
        if (zone >= T_FILE && zone <= T_CLOSE) return HTCLIENT;
        hit = DefWindowProcW( hwnd, msg, wparam, lparam );
        if (hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE || hit == HTHELP) return HTCAPTION;
        if (hit != HTCLIENT) return hit;
        GetClientRect( hwnd, &rect );
        if (!IsZoomed( hwnd ) && pt.y < px( 4 ))
        {
            if (pt.x < px( 8 )) return HTTOPLEFT;
            if (pt.x >= rect.right - px( 8 )) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y >= TITLE_H) return HTCLIENT;
        if (PtInRect( &title_zones[T_ICON], pt )) return HTSYSMENU;
        return HTCAPTION;
    }

    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTSYSMENU)
        {
            PostMessageW( hwnd, WM_CLOSE, 0, 0 );
            return 0;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        title_paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        int hot = title_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (hot != title_hot)
        {
            title_hot = hot;
            invalidate_title();
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (title_hot)
        {
            title_hot = T_NONE;
            invalidate_title();
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        int zone = title_hit( pt );

        /* the menus open on the press, as a menu bar's */
        if (zone == T_FILE || zone == T_EDIT || zone == T_VIEW || zone == T_SETTINGS)
        {
            title_down = zone;
            invalidate_title();
            UpdateWindow( hwnd );
            title_click( zone );
            title_down = title_hot = T_NONE;
            invalidate_title();
            return 0;
        }
        title_down = zone;
        if (title_down) SetCapture( hwnd );
        invalidate_title();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        int was = title_down;

        title_down = T_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        invalidate_title();
        if (was && title_hit( pt ) == was) title_click( was );
        return 0;
    }
    case WM_RBUTTONUP:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        if (pt.y < TITLE_H)
        {
            HMENU menu = GetSystemMenu( hwnd, FALSE );
            int cmd;
            ClientToScreen( hwnd, &pt );
            cmd = TrackPopupMenu( menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL );
            if (cmd) PostMessageW( hwnd, WM_SYSCOMMAND, cmd, 0 );
        }
        return 0;
    }

    case WM_SIZE:
        no_caption_backdrop( hwnd );
        layout_frame();
        return 0;

    case WM_ACTIVATE:
        invalidate_title();
        if (LOWORD( wparam ) != WA_INACTIVE) SetFocus( canvas );
        return 0;
    case WM_SETFOCUS:
        SetFocus( canvas );
        return 0;
    case WM_APP:
        SetFocus( canvas );
        return 0;

    case WM_COMMAND:
        if (lparam) return 0;
        command( LOWORD( wparam ) );
        return 0;

    case WM_SETTINGCHANGE:
        if (lparam && !lstrcmpW( (const WCHAR *)lparam, L"ImmersiveColorSet" )) apply_theme();
        break;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        apply_theme();
        break;

    case WM_DROPFILES:
    {
        WCHAR path[MAX_PATH];
        if (DragQueryFileW( (HDROP)wparam, 0, path, MAX_PATH ) && ask_save_changes()) open_file( path );
        DragFinish( (HDROP)wparam );
        return 0;
    }

    case WM_CLOSE:
        if (!ask_save_changes()) return 0;
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
    HACCEL accel;
    WCHAR **argv;
    int argc, x, y, w, h;
    const WCHAR *open = NULL;
    BOOL print = FALSE;
    MSG msg;
    HDC hdc;

    instance = inst;
    SetProcessDPIAware();
    hdc = GetDC( 0 );
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    ReleaseDC( 0, hdc );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    InitCommonControlsEx( &controls );
    status_shown = setting( L"StatusBar", 1 ) != 0;
    theme_load();
    menus_follow_theme();

    argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    for (int i = 1; i < argc; i++)
    {
        if (!wcsicmp( argv[i], L"/p" ) || !wcsicmp( argv[i], L"-p" )) print = TRUE;
        else if (argv[i][0] != '/' || GetFileAttributesW( argv[i] ) != INVALID_FILE_ATTRIBUTES) open = argv[i];
    }

    /* a check without a window: /convert in out */
    if (argc == 4 && !wcsicmp( argv[1], L"/convert" ))
    {
        struct image *image = image_load( argv[2], NULL );
        const GUID *format = format_of_path( argv[3] );
        return image && format && image_save( image, argv[3], format ) ? 0 : 3;
    }

    small_icon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_PAINT ), IMAGE_ICON, px( 16 ), px( 16 ), 0 );
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = frame_proc;
    cls.hInstance = inst;
    cls.hIcon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_PAINT ), IMAGE_ICON, GetSystemMetrics( SM_CXICON ),
                            GetSystemMetrics( SM_CYICON ), 0 );
    cls.hIconSm = small_icon;
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    cls.lpszClassName = L"MSPaintApp";
    RegisterClassExW( &cls );

    w = setting( L"Width", px( 1280 ) );
    h = setting( L"Height", px( 820 ) );
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
    if (!CreateWindowExW( WS_EX_ACCEPTFILES, cls.lpszClassName, L"Paint", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          x, y, w, h, NULL, NULL, inst, NULL ))
        return 1;
    {
        BOOL dark = th.dark;
        DwmSetWindowAttribute( frame, 20, &dark, sizeof(dark) );
    }
    new_picture();
    layout_frame();
    ShowWindow( frame, setting( L"Maximized", 0 ) && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show );
    UpdateWindow( frame );
    if (open) open_file( open );
    else canvas_fit();
    if (print && open)
    {
        print_picture();
        DestroyWindow( frame );
    }
    LocalFree( argv );

    accel = LoadAcceleratorsW( inst, MAKEINTRESOURCEW( IDA_ACCEL ) );
    while (GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        /* the text's fields keep their keys */
        if (!(textbar && IsChild( textbar, msg.hwnd )) && TranslateAcceleratorW( frame, accel, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    CoUninitialize();
    return (int)msg.wParam;
}
