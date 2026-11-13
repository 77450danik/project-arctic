/*
 * Display of the Control Panel: the desktop's background
 *
 * What desk.cpl's Background page wrote, written the same way: the picture
 * (HKCU\Control Panel\Desktop, Wallpaper), how it fills the desktop
 * (WallpaperStyle, TileWallpaper: 10 fill, 6 fit, 2 stretch, 0 centre or
 * tile), the colour under it (COLOR_BACKGROUND, Control Panel\Colors). The
 * picture stays the file chosen, in any format: user32 reads it with WIC
 * (Wine patch 0026), there is nothing to convert. As in Windows 11, the
 * pictures chosen last are kept (Explorer\Wallpapers, BackgroundHistoryPath0
 * to 4) and the kind of background (BackgroundType: 0 a picture, 1 a colour).
 *
 * And a desktop in small for the pages: the wallpaper as it fills the
 * screen, a window in the modes' colours, the taskbar.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"
#include "shlwapi.h"

#define DESKTOP_KEY     L"Control Panel\\Desktop"
#define COLORS_KEY      L"Control Panel\\Colors"
#define WALLPAPERS_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers"

static int desktop_number( const WCHAR *name, int def )
{
    WCHAR value[16];
    DWORD size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, DESKTOP_KEY, name, RRF_RT_REG_SZ, NULL, value, &size )) return def;
    return _wtoi( value );
}

static BOOL desktop_string( const WCHAR *name, WCHAR *value, DWORD count )
{
    DWORD size = count * sizeof(WCHAR);

    value[0] = 0;
    return !RegGetValueW( HKEY_CURRENT_USER, DESKTOP_KEY, name, RRF_RT_REG_SZ, NULL, value,
                          &size ) && value[0];
}

/* the file the desktop shows; desk.cpl kept a copy as a BMP, and the file
 * chosen beside it (ConvertedWallpaper, OriginalWallpaper) */
BOOL wallpaper_current( WCHAR *path, UINT count )
{
    WCHAR original[MAX_PATH], converted[MAX_PATH];

    if (!desktop_string( L"Wallpaper", path, count )) return FALSE;
    if (desktop_string( L"OriginalWallpaper", original, ARRAY_SIZE(original) ) && !wcsicmp( original, path ) &&
        desktop_string( L"ConvertedWallpaper", converted, ARRAY_SIZE(converted) ) &&
        GetFileAttributesW( converted ) != INVALID_FILE_ATTRIBUTES)
        lstrcpynW( path, converted, count );
    return TRUE;
}

int wallpaper_fit(void)
{
    switch (desktop_number( L"WallpaperStyle", 10 ))
    {
    case 6: return FIT_FIT;
    case 2: return FIT_STRETCH;
    case 0: return desktop_number( L"TileWallpaper", 0 ) ? FIT_TILE : FIT_CENTER;
    default: return FIT_FILL;
    }
}

DWORD background_type(void)
{
    WCHAR path[MAX_PATH];

    return wallpaper_current( path, ARRAY_SIZE(path) ) ? BACKGROUND_PICTURE : BACKGROUND_COLOR;
}

static void set_type( DWORD type )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, WALLPAPERS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"BackgroundType", 0, REG_DWORD, (const BYTE *)&type, sizeof(type) );
    RegCloseKey( key );
}

UINT wallpaper_history( WCHAR paths[][MAX_PATH], UINT max )
{
    UINT count = 0;

    for (UINT i = 0; i < 5 && count < max; i++)
    {
        WCHAR name[32];
        DWORD size = MAX_PATH * sizeof(WCHAR);

        swprintf( name, ARRAY_SIZE(name), L"BackgroundHistoryPath%u", i );
        if (RegGetValueW( HKEY_CURRENT_USER, WALLPAPERS_KEY, name, RRF_RT_REG_SZ, NULL, paths[count], &size ) ||
            !paths[count][0] || GetFileAttributesW( paths[count] ) == INVALID_FILE_ATTRIBUTES)
            continue;
        count++;
    }
    return count;
}

static void history_add( const WCHAR *path )
{
    WCHAR (*list)[MAX_PATH] = calloc( 6, MAX_PATH * sizeof(WCHAR) );
    UINT count, out = 1;
    HKEY key;

    if (!list) return;
    count = wallpaper_history( list + 1, 5 );
    lstrcpynW( list[0], path, MAX_PATH );
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, WALLPAPERS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        for (UINT i = 1; i <= count && out < 5; i++)
            if (wcsicmp( list[i], path )) lstrcpynW( list[out++], list[i], MAX_PATH );
        for (UINT i = 0; i < 5; i++)
        {
            WCHAR name[32];

            swprintf( name, ARRAY_SIZE(name), L"BackgroundHistoryPath%u", i );
            if (i < out) RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)list[i], (lstrlenW( list[i] ) + 1) * sizeof(WCHAR) );
            else RegDeleteValueW( key, name );
        }
        RegCloseKey( key );
    }
    free( list );
}

/* Windows' own: %WINDIR%\Web\Wallpaper\<theme>\*, as Windows 11 lists them */
UINT wallpaper_windows( WCHAR paths[][MAX_PATH], UINT max )
{
    static const WCHAR * const types[] = { L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp" };
    WCHAR dir[MAX_PATH], mask[MAX_PATH];
    WIN32_FIND_DATAW folder, file;
    HANDLE folders, files;
    UINT count = 0;

    GetWindowsDirectoryW( dir, ARRAY_SIZE(dir) );
    lstrcatW( dir, L"\\Web\\Wallpaper" );
    swprintf( mask, ARRAY_SIZE(mask), L"%s\\*", dir );
    if ((folders = FindFirstFileW( mask, &folder )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (!(folder.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || folder.cFileName[0] == '.') continue;
        for (UINT t = 0; t < ARRAY_SIZE(types) && count < max; t++)
        {
            swprintf( mask, ARRAY_SIZE(mask), L"%s\\%s\\%s", dir, folder.cFileName, types[t] );
            if ((files = FindFirstFileW( mask, &file )) == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
                swprintf( paths[count], MAX_PATH, L"%s\\%s\\%s", dir, folder.cFileName, file.cFileName );
                count++;
            } while (count < max && FindNextFileW( files, &file ));
            FindClose( files );
        }
    } while (count < max && FindNextFileW( folders, &folder ));
    FindClose( folders );
    return count;
}

static void write_fit( HKEY key, int fit )
{
    static const WCHAR * const styles[FIT_COUNT] = { L"10", L"6", L"2", L"0", L"0" };
    const WCHAR *tile = fit == FIT_TILE ? L"1" : L"0";

    RegSetValueExW( key, L"WallpaperStyle", 0, REG_SZ, (const BYTE *)styles[fit], (lstrlenW( styles[fit] ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"TileWallpaper", 0, REG_SZ, (const BYTE *)tile, 2 * sizeof(WCHAR) );
}

/* a picture (or the one shown, path NULL), filling the desktop so */
BOOL wallpaper_set( const WCHAR *path, int fit )
{
    WCHAR current[MAX_PATH];
    HKEY key;

    if (!path)
    {
        if (!wallpaper_current( current, ARRAY_SIZE(current) )) return FALSE;
        path = current;
    }
    if (fit < 0 || fit >= FIT_COUNT) fit = wallpaper_fit();
    if (RegCreateKeyExW( HKEY_CURRENT_USER, DESKTOP_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return FALSE;
    write_fit( key, fit );
    /* the file itself from now on, not desk.cpl's copy of it */
    RegDeleteValueW( key, L"OriginalWallpaper" );
    RegDeleteValueW( key, L"ConvertedWallpaper" );
    RegCloseKey( key );
    if (!SystemParametersInfoW( SPI_SETDESKWALLPAPER, 0, (void *)path, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE ))
        return FALSE;
    history_add( path );
    set_type( BACKGROUND_PICTURE );
    return TRUE;
}

COLORREF desktop_color(void)
{
    return GetSysColor( COLOR_BACKGROUND );
}

/* the colour under the picture; no picture with picture FALSE */
void desktop_color_set( COLORREF color, BOOL remove_picture )
{
    INT element = COLOR_BACKGROUND;
    WCHAR text[16];
    HKEY key;

    SetSysColors( 1, &element, &color );
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, COLORS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        swprintf( text, ARRAY_SIZE(text), L"%d %d %d", GetRValue( color ), GetGValue( color ), GetBValue( color ) );
        RegSetValueExW( key, L"Background", 0, REG_SZ, (const BYTE *)text, (lstrlenW( text ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    if (!remove_picture) return;
    SystemParametersInfoW( SPI_SETDESKWALLPAPER, 0, (void *)L"", SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE );
    set_type( BACKGROUND_COLOR );
}

/**********************************************************************
 *          A desktop in small
 */

static void fill( HDC hdc, int left, int top, int right, int bottom, COLORREF color )
{
    RECT rect = { left, top, right, bottom };

    FillRect( hdc, &rect, view_brush( color ) );
}

static void round_box( HDC hdc, int left, int top, int right, int bottom, int radius, COLORREF color )
{
    HGDIOBJ old_pen = SelectObject( hdc, GetStockObject( NULL_PEN ) ), old_brush = SelectObject( hdc, view_brush( color ) );

    RoundRect( hdc, left, top, right + 1, bottom + 1, radius, radius );
    SelectObject( hdc, old_brush );
    SelectObject( hdc, old_pen );
}

/* a window with its title bar in the mode of Windows and its contents in the
 * programs', the taskbar under it in the mode of Windows */
void desktop_overlay( HDC hdc, const RECT *rect, UINT_PTR param )
{
    BOOL system_light = (param & OVERLAY_SYSTEM_LIGHT) != 0, apps_light = (param & OVERLAY_APPS_LIGHT) != 0;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    int bar = max( 4, height / 9 ), unit = max( 1, height / 30 );
    COLORREF accent = view_accent();

    if (param & OVERLAY_WINDOW)
    {
        int x0 = rect->left + width / 5, y0 = rect->top + height / 6, x1 = rect->right - width / 5;
        int y1 = rect->bottom - bar - height / 8, caption = max( 3, height / 11 );
        COLORREF body = apps_light ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x1f, 0x1f, 0x1f );
        COLORREF ink = apps_light ? RGB( 0xd6, 0xd6, 0xd6 ) : RGB( 0x48, 0x48, 0x48 );

        round_box( hdc, x0, y0, x1, y1, unit * 3, system_light ? RGB( 0xf3, 0xf3, 0xf3 ) : RGB( 0x20, 0x20, 0x20 ) );
        fill( hdc, x0, y0 + caption, x1, y1 - unit, body );
        fill( hdc, x0 + unit, y1 - unit * 2, x1 - unit, y1, body );
        for (int i = 0; i < 3; i++)
        {
            int ly = y0 + caption + unit * 3 + i * unit * 3;

            if (ly + unit > y1 - unit * 5) break;
            fill( hdc, x0 + unit * 3, ly, x1 - unit * (3 + i * 6), ly + unit, ink );
        }
        round_box( hdc, x1 - unit * 12, y1 - unit * 5, x1 - unit * 3, y1 - unit * 2, unit, accent );
    }
    /* the taskbar, its buttons in the middle */
    fill( hdc, rect->left, rect->bottom - bar, rect->right, rect->bottom,
          system_light ? RGB( 0xee, 0xee, 0xee ) : RGB( 0x1c, 0x1c, 0x1c ) );
    for (int i = -2; i <= 2; i++)
    {
        int cx = (rect->left + rect->right) / 2 + i * (bar * 3 / 4), cy = rect->bottom - bar / 2, half = max( 1, bar / 4 );

        round_box( hdc, cx - half, cy - half, cx + half, cy + half, max( 1, half / 2 ),
                   i == -2 ? accent : system_light ? RGB( 0x70, 0x70, 0x70 ) : RGB( 0xa0, 0xa0, 0xa0 ) );
    }
}

static void blit( HDC hdc, HBITMAP bitmap, int x, int y, int w, int h, int sx, int sy, int sw, int sh )
{
    HDC mem = CreateCompatibleDC( hdc );
    HGDIOBJ old = SelectObject( mem, bitmap );

    SetStretchBltMode( hdc, HALFTONE );
    SetBrushOrgEx( hdc, 0, 0, NULL );
    StretchBlt( hdc, x, y, w, h, mem, sx, sy, sw, sh, SRCCOPY );
    SelectObject( mem, old );
    DeleteDC( mem );
}

/* the size of the picture in small: the whole of it, twice the preview's
 * width at most, so that a part of it fills the preview sharp */
void preview_thumb_size( const struct preview *p, int *width, int *height )
{
    int w = p->width * 2, h;

    if (!p->image_width || !p->image_height)
    {
        *width = *height = 0;
        return;
    }
    h = MulDiv( w, p->image_height, p->image_width );
    if (h > p->height * 2)
    {
        h = p->height * 2;
        w = max( 1, MulDiv( h, p->image_width, p->image_height ) );
    }
    *width = max( 1, w );
    *height = max( 1, h );
}

/* the desktop as the background makes it: param a struct preview */
void preview_paint( HDC hdc, const RECT *rect, UINT_PTR param )
{
    const struct preview *p = (const struct preview *)param;
    int width = rect->right - rect->left, height = rect->bottom - rect->top, tw, th;
    HBITMAP bitmap = NULL;
    BITMAP bm;

    fill( hdc, rect->left, rect->top, rect->right, rect->bottom, p->color );
    preview_thumb_size( p, &tw, &th );
    if (p->picture && tw && (bitmap = picture_peek( p->path, tw, th )) && GetObjectW( bitmap, sizeof(bm), &bm ))
    {
        /* the screen's pixels in the preview's */
        double scale = (double)width / max( 1, p->screen_width );
        int iw = max( 1, (int)(p->image_width * scale) ), ih = max( 1, (int)(p->image_height * scale) );

        switch (p->fit)
        {
        case FIT_FILL:
        {
            int sw = bm.bmWidth, sh = bm.bmHeight;

            if ((LONGLONG)bm.bmWidth * height > (LONGLONG)bm.bmHeight * width) sw = max( 1, MulDiv( bm.bmHeight, width, height ) );
            else sh = max( 1, MulDiv( bm.bmWidth, height, width ) );
            blit( hdc, bitmap, rect->left, rect->top, width, height, (bm.bmWidth - sw) / 2, (bm.bmHeight - sh) / 2, sw, sh );
            break;
        }
        case FIT_FIT:
        {
            int w = width, h = MulDiv( width, bm.bmHeight, bm.bmWidth );

            if (h > height)
            {
                h = height;
                w = MulDiv( height, bm.bmWidth, bm.bmHeight );
            }
            blit( hdc, bitmap, rect->left + (width - w) / 2, rect->top + (height - h) / 2, w, h, 0, 0, bm.bmWidth, bm.bmHeight );
            break;
        }
        case FIT_STRETCH:
            blit( hdc, bitmap, rect->left, rect->top, width, height, 0, 0, bm.bmWidth, bm.bmHeight );
            break;
        case FIT_CENTER:
            blit( hdc, bitmap, rect->left + (width - iw) / 2, rect->top + (height - ih) / 2, iw, ih, 0, 0, bm.bmWidth,
                  bm.bmHeight );
            break;
        case FIT_TILE:
            for (int y = rect->top; y < rect->bottom; y += ih)
                for (int x = rect->left; x < rect->right; x += iw)
                    blit( hdc, bitmap, x, y, iw, ih, 0, 0, bm.bmWidth, bm.bmHeight );
            break;
        }
    }
    desktop_overlay( hdc, rect, p->overlay );
}

/* the background as it is now, for a preview width × height */
void preview_init( struct preview *p, int width, int height )
{
    WCHAR path[MAX_PATH];

    memset( p, 0, sizeof(*p) );
    p->width = width;
    p->height = height;
    p->screen_width = max( 1, GetSystemMetrics( SM_CXSCREEN ) );
    p->color = desktop_color();
    p->fit = wallpaper_fit();
    if ((p->picture = wallpaper_current( path, ARRAY_SIZE(path) )))
    {
        lstrcpynW( p->path, path, ARRAY_SIZE(p->path) );
        picture_size( p->path, &p->image_width, &p->image_height );
    }
    p->overlay = OVERLAY_WINDOW | (modes_get() & (OVERLAY_SYSTEM_LIGHT | OVERLAY_APPS_LIGHT));
}

/* the screen's shape, at height h */
int preview_width( int height )
{
    int w = GetSystemMetrics( SM_CXSCREEN ), h = GetSystemMetrics( SM_CYSCREEN );

    if (w <= 0 || h <= 0) return height * 16 / 9;
    return max( height, min( MulDiv( height, w, h ), height * 3 ) );
}
