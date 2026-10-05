/*
 * Snipping Tool: the screen, the clipboard and image files
 *
 * The screen comes from dwm.exe, as PrintScreen's does (wine/arctic_dwm.h):
 * the pixels the monitors show, whatever DPI a program sees. A snip goes to
 * the clipboard as CF_DIB and as "PNG", which browsers and Electron programs
 * take first, transparency and all.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stddef.h>
#include <stdio.h>

#define COBJMACROS
#include "snippingtool.h"
#include "objbase.h"
#include "initguid.h"  /* the WIC GUIDs are in no import library */
#include "wincodec.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(snippingtool);

/* what the monitors show of rect, from dwm.exe */
static BOOL capture_from_dwm( const RECT *rect, struct image *image )
{
    SIZE_T size = offsetof( struct arctic_capture, pixels[0] ) + (SIZE_T)image->width * image->height * sizeof(UINT32);
    HANDLE lock, section = 0, request = 0, done = 0;
    struct arctic_capture *capture = NULL;
    BOOL ret = FALSE;

    if (!(lock = CreateMutexW( NULL, FALSE, ARCTIC_CAPTURE_LOCK ))) return FALSE;
    if (WaitForSingleObject( lock, 5000 ) == WAIT_TIMEOUT)
    {
        CloseHandle( lock );
        return FALSE;
    }
    if (!(request = OpenEventW( EVENT_MODIFY_STATE, FALSE, ARCTIC_CAPTURE_REQUEST )) ||
        !(done = OpenEventW( SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, ARCTIC_CAPTURE_DONE )) ||
        !(section = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((UINT64)size >> 32),
                                        (DWORD)size, ARCTIC_CAPTURE_SECTION )) ||
        !(capture = MapViewOfFile( section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0 )))
    {
        WARN( "dwm.exe takes no screenshots: %lu\n", GetLastError() );
        goto done;
    }
    capture->x = rect->left;
    capture->y = rect->top;
    capture->width = image->width;
    capture->height = image->height;
    capture->status = -1;
    ResetEvent( done );
    SetEvent( request );
    if (WaitForSingleObject( done, 5000 )) WARN( "dwm.exe did not answer\n" );
    else if (capture->status) WARN( "dwm.exe could not draw %s: %#lx\n", wine_dbgstr_rect( rect ), capture->status );
    else
    {
        memcpy( image->bits, capture->pixels, (SIZE_T)image->width * image->height * sizeof(UINT32) );
        ret = TRUE;
    }

done:
    if (capture) UnmapViewOfFile( capture );
    if (section) CloseHandle( section );
    if (request) CloseHandle( request );
    if (done) CloseHandle( done );
    ReleaseMutex( lock );
    CloseHandle( lock );
    return ret;
}

/* without dwm.exe: what GDI has of the screen */
static BOOL capture_from_gdi( const RECT *rect, struct image *image )
{
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biWidth = image->width,
                                       .biHeight = -image->height, .biPlanes = 1, .biBitCount = 32 } };
    HDC screen = GetDC( 0 ), dc = CreateCompatibleDC( screen );
    void *bits;
    HBITMAP bitmap = CreateDIBSection( screen, &info, DIB_RGB_COLORS, &bits, NULL, 0 );
    BOOL ret = FALSE;

    if (bitmap)
    {
        HGDIOBJ old = SelectObject( dc, bitmap );
        if ((ret = BitBlt( dc, 0, 0, image->width, image->height, screen, rect->left, rect->top, SRCCOPY )))
        {
            GdiFlush();
            memcpy( image->bits, bits, (SIZE_T)image->width * image->height * sizeof(UINT32) );
        }
        SelectObject( dc, old );
        DeleteObject( bitmap );
    }
    DeleteDC( dc );
    ReleaseDC( 0, screen );
    return ret;
}

/* all the monitors, in the virtual screen's pixels */
struct image *capture_screen( RECT *screen )
{
    struct image *image;

    SetRect( screen, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
             GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ),
             GetSystemMetrics( SM_YVIRTUALSCREEN ) + GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    if (!(image = image_create( screen->right - screen->left, screen->bottom - screen->top ))) return NULL;
    if (!capture_from_dwm( screen, image ) && !capture_from_gdi( screen, image ))
    {
        ERR( "no picture of the screen %s\n", wine_dbgstr_rect( screen ) );
        image_release( image );
        return NULL;
    }
    for (SIZE_T i = 0; i < (SIZE_T)image->width * image->height; i++) image->bits[i] |= 0xff000000;
    return image;
}

struct window_list
{
    RECT *rects;
    int   count, max;
    DWORD pid;
};

static BOOL CALLBACK add_window( HWND hwnd, LPARAM lparam )
{
    struct window_list *list = (struct window_list *)lparam;
    LONG exstyle = GetWindowLongW( hwnd, GWL_EXSTYLE );
    DWORD pid;
    RECT rect;

    if (list->count == list->max) return FALSE;
    if (!IsWindowVisible( hwnd ) || IsIconic( hwnd )) return TRUE;
    GetWindowThreadProcessId( hwnd, &pid );
    if (pid == list->pid) return TRUE;
    /* what the pointer goes through is not a window to the user */
    if ((exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED)) return TRUE;
    if (!GetWindowRect( hwnd, &rect ) || rect.right - rect.left < 4 || rect.bottom - rect.top < 4) return TRUE;
    list->rects[list->count++] = rect;
    return TRUE;
}

/* the top-level windows that show, the front one first */
int list_windows( RECT *rects, int max )
{
    struct window_list list = { rects, 0, max, GetCurrentProcessId() };

    EnumWindows( add_window, (LPARAM)&list );
    return list.count;
}

static IWICImagingFactory *wic_factory(void)
{
    IWICImagingFactory *factory = NULL;

    if (FAILED(CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                 (void **)&factory )))
        return NULL;
    return factory;
}

/* image into stream as PNG, or JPEG (24 bits, transparency on white) */
static HRESULT encode( IWICImagingFactory *factory, IStream *stream, const struct image *image, BOOL jpeg )
{
    WICPixelFormatGUID format = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    IWICBitmapFrameEncode *frame = NULL;
    IWICBitmapEncoder *encoder = NULL;
    UINT stride = jpeg ? (image->width * 3 + 3) & ~3 : image->width * 4;
    BYTE *pixels = (BYTE *)image->bits, *rgb = NULL;
    HRESULT hr;

    if (jpeg)
    {
        if (!(rgb = calloc( (SIZE_T)stride, image->height ))) return E_OUTOFMEMORY;
        for (int y = 0; y < image->height; y++)
            for (int x = 0; x < image->width; x++)
            {
                UINT32 p = image->bits[(SIZE_T)y * image->width + x];
                unsigned int a = p >> 24;
                BYTE *out = rgb + (SIZE_T)y * stride + x * 3;
                for (int c = 0; c < 3; c++) out[c] = (((p >> (8 * c)) & 0xff) * a + 255 * (255 - a)) / 255;
            }
        pixels = rgb;
    }
    hr = IWICImagingFactory_CreateEncoder( factory, jpeg ? &GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng,
                                           NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, image->width, image->height );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &format );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels( frame, image->height, stride, stride * image->height,
                                                                pixels );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    free( rgb );
    return hr;
}

static HGLOBAL png_global( const struct image *image )
{
    IWICImagingFactory *factory = wic_factory();
    IStream *stream = NULL;
    HGLOBAL ret = 0, data;
    STATSTG stat;

    if (!factory) return 0;
    if (SUCCEEDED(CreateStreamOnHGlobal( NULL, TRUE, &stream )) &&
        SUCCEEDED(encode( factory, stream, image, FALSE )) &&
        SUCCEEDED(IStream_Stat( stream, &stat, STATFLAG_NONAME )) &&
        SUCCEEDED(GetHGlobalFromStream( stream, &data )) &&
        (ret = GlobalAlloc( GMEM_MOVEABLE, stat.cbSize.LowPart )))
    {
        memcpy( GlobalLock( ret ), GlobalLock( data ), stat.cbSize.LowPart );
        GlobalUnlock( data );
        GlobalUnlock( ret );
    }
    if (stream) IStream_Release( stream );
    IWICImagingFactory_Release( factory );
    return ret;
}

/* CF_DIB, bottom row first, what is transparent white */
static HGLOBAL dib_global( const struct image *image )
{
    SIZE_T row = (SIZE_T)image->width * sizeof(UINT32);
    BITMAPINFOHEADER *header;
    HGLOBAL dib;

    if (!(dib = GlobalAlloc( GMEM_MOVEABLE, sizeof(*header) + row * image->height ))) return 0;
    header = GlobalLock( dib );
    memset( header, 0, sizeof(*header) );
    header->biSize = sizeof(*header);
    header->biWidth = image->width;
    header->biHeight = image->height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = row * image->height;
    for (int y = 0; y < image->height; y++)
    {
        const UINT32 *src = image->bits + (SIZE_T)y * image->width;
        UINT32 *dst = (UINT32 *)((BYTE *)(header + 1) + row * (image->height - 1 - y));
        for (int x = 0; x < image->width; x++)
        {
            UINT32 p = src[x];
            unsigned int a = p >> 24, out = 0xff000000;
            if (a == 255) out = p;
            else for (int c = 0; c < 3; c++) out |= ((((p >> (8 * c)) & 0xff) * a + 255 * (255 - a)) / 255) << (8 * c);
            dst[x] = out;
        }
    }
    GlobalUnlock( dib );
    return dib;
}

BOOL clipboard_set_image( HWND owner, const struct image *image )
{
    HGLOBAL dib = dib_global( image ), png = png_global( image );
    BOOL opened = FALSE;

    for (int i = 0; i < 10 && !(opened = OpenClipboard( owner )); i++) Sleep( 50 );  /* another program has it */
    if (!opened)
    {
        WARN( "the clipboard is busy\n" );
        if (dib) GlobalFree( dib );
        if (png) GlobalFree( png );
        return FALSE;
    }
    EmptyClipboard();
    if (dib && !SetClipboardData( CF_DIB, dib )) GlobalFree( dib );
    if (png && !SetClipboardData( RegisterClipboardFormatW( L"PNG" ), png )) GlobalFree( png );
    CloseClipboard();
    return dib != 0;
}

/* PNG, or JPEG when the name says so */
HRESULT save_image( const WCHAR *path, const struct image *image )
{
    IWICImagingFactory *factory = wic_factory();
    const WCHAR *ext = wcsrchr( path, '.' );
    BOOL jpeg = ext && (!wcsicmp( ext, L".jpg" ) || !wcsicmp( ext, L".jpeg" ));
    IWICStream *stream = NULL;
    HRESULT hr;

    if (!factory) return E_FAIL;
    hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename( stream, path, GENERIC_WRITE );
    if (SUCCEEDED(hr)) hr = encode( factory, (IStream *)stream, image, jpeg );
    if (stream) IWICStream_Release( stream );
    IWICImagingFactory_Release( factory );
    if (FAILED(hr))
    {
        ERR( "%s: %#lx\n", debugstr_w(path), hr );
        DeleteFileW( path );
    }
    return hr;
}

struct image *load_image( const WCHAR *path )
{
    IWICImagingFactory *factory = wic_factory();
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *source = NULL;
    struct image *image = NULL;
    UINT width = 0, height = 0;
    HRESULT hr;

    if (!factory) return NULL;
    hr = IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame( decoder, 0, &frame );
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource( &GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)frame, &source );
    if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize( source, &width, &height );
    if (SUCCEEDED(hr) && (image = image_create( width, height )))
    {
        hr = IWICBitmapSource_CopyPixels( source, NULL, width * 4, width * height * 4, (BYTE *)image->bits );
        if (FAILED(hr))
        {
            image_release( image );
            image = NULL;
        }
    }
    if (FAILED(hr)) ERR( "%s: %#lx\n", debugstr_w(path), hr );
    if (source) IWICBitmapSource_Release( source );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    IWICImagingFactory_Release( factory );
    return image;
}

/* Pictures\Screenshots, made when it is not there, as PrintScreen's */
BOOL screenshots_folder( WCHAR *path, DWORD size )
{
    WCHAR value[MAX_PATH] = L"%USERPROFILE%\\Pictures";
    DWORD len = sizeof(value);

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                  L"My Pictures", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, value, &len );
    if (!ExpandEnvironmentStringsW( value, path, size )) return FALSE;
    CreateDirectoryW( path, NULL );
    if (wcslen( path ) + 13 >= size) return FALSE;
    wcscat( path, L"\\Screenshots" );
    return CreateDirectoryW( path, NULL ) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* where the Snipping Tool of Windows 11 keeps each snip on its own:
 * "Знімок екрана 2026-10-05 143012.png" */
BOOL autosave_path( WCHAR *path, DWORD size )
{
    WCHAR folder[MAX_PATH], name[MAX_PATH];
    SYSTEMTIME now;

    if (!screenshots_folder( folder, ARRAY_SIZE(folder) )) return FALSE;
    GetLocalTime( &now );
    swprintf( name, ARRAY_SIZE(name), string( IDS_AUTOSAVE_NAME ), now.wYear, now.wMonth, now.wDay, now.wHour,
              now.wMinute, now.wSecond );
    swprintf( path, size, L"%s\\%s", folder, name );
    for (int i = 2; GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES && i < 100; i++)
    {
        WCHAR *dot = wcsrchr( name, '.' );
        if (dot) *dot = 0;
        swprintf( path, size, L"%s\\%s (%d).png", folder, name, i );
        if (dot) *dot = '.';
    }
    return TRUE;
}
