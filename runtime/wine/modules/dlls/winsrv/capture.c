/*
 * Arctic window server: screenshots
 *
 * As win32k does it in Windows: PrintScreen puts the screen on the
 * clipboard, Alt+PrintScreen the active window, and Win+PrintScreen also
 * keeps the screen as "Знімок екрана (N).png" in Pictures\Screenshots, the
 * screen dimming for a moment. dwm.exe draws what the monitors show
 * (wine/arctic_dwm.h); the raw input thread sees the key first.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "objbase.h"
#include "initguid.h"  /* the WIC GUIDs are in no import library */
#include "wincodec.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

#include "winsrv_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsrv);

static const WCHAR explorer_keyW[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";

static LONG busy;  /* one screenshot at a time, however long the key is held */

/* What the monitors show of rect, from dwm.exe: BGRA, top row first; free()
 * it. NULL when dwm.exe cannot draw it. */
static UINT32 *capture_pixels( const RECT *rect )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    SIZE_T size = offsetof( struct arctic_capture, pixels[0] ) + (SIZE_T)width * height * sizeof(UINT32);
    HANDLE lock, section = 0, request = 0, done = 0;
    struct arctic_capture *capture = NULL;
    UINT32 *pixels = NULL;

    if (width <= 0 || height <= 0) return NULL;
    if (!(lock = CreateMutexW( NULL, FALSE, ARCTIC_CAPTURE_LOCK ))) return NULL;
    if (WaitForSingleObject( lock, 5000 ) == WAIT_TIMEOUT)
    {
        CloseHandle( lock );
        return NULL;
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
    capture->width = width;
    capture->height = height;
    capture->status = -1;
    ResetEvent( done );
    SetEvent( request );
    if (WaitForSingleObject( done, 5000 ))
        WARN( "dwm.exe did not answer\n" );
    else if (capture->status)
        WARN( "dwm.exe could not draw %s: %#lx\n", wine_dbgstr_rect( rect ), capture->status );
    else if ((pixels = malloc( (SIZE_T)width * height * sizeof(UINT32) )))
        memcpy( pixels, capture->pixels, (SIZE_T)width * height * sizeof(UINT32) );

done:
    if (capture) UnmapViewOfFile( capture );
    if (section) CloseHandle( section );
    if (request) CloseHandle( request );
    if (done) CloseHandle( done );
    ReleaseMutex( lock );
    CloseHandle( lock );
    return pixels;
}

/* CF_DIB, bottom row first, as programs expect it */
static BOOL set_clipboard( HWND owner, const UINT32 *pixels, int width, int height )
{
    SIZE_T row = (SIZE_T)width * sizeof(UINT32);
    BITMAPINFOHEADER *header;
    HGLOBAL dib;

    if (!(dib = GlobalAlloc( GMEM_MOVEABLE, sizeof(*header) + row * height ))) return FALSE;
    header = GlobalLock( dib );
    memset( header, 0, sizeof(*header) );
    header->biSize = sizeof(*header);
    header->biWidth = width;
    header->biHeight = height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = row * height;
    for (int y = 0; y < height; y++)
        memcpy( (BYTE *)(header + 1) + row * (height - 1 - y), pixels + (SIZE_T)y * width, row );
    GlobalUnlock( dib );

    if (!OpenClipboard( owner ))
    {
        GlobalFree( dib );
        return FALSE;
    }
    EmptyClipboard();
    if (!SetClipboardData( CF_DIB, dib )) GlobalFree( dib );
    CloseClipboard();
    return TRUE;
}

/* Pictures\Screenshots, made when it is not there */
static BOOL screenshots_folder( WCHAR *path, DWORD size )
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

/* "Знімок екрана (N).png", N going on from where it was, as Windows counts */
static BOOL screenshot_name( WCHAR *path, DWORD size )
{
    WCHAR folder[MAX_PATH], format[64], name[96];
    DWORD index = 1, len = sizeof(index);
    HKEY key;

    if (!screenshots_folder( folder, ARRAY_SIZE(folder) )) return FALSE;
    if (!LoadStringW( GetModuleHandleW( L"winsrv.dll" ), IDS_SCREENSHOT_NAME, format, ARRAY_SIZE(format) ))
        wcscpy( format, L"Screenshot (%u).png" );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, explorer_keyW, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key,
                         NULL ))
        key = 0;
    if (key) RegQueryValueExW( key, L"ScreenshotIndex", NULL, NULL, (BYTE *)&index, &len );
    for (;; index++)
    {
        swprintf( name, ARRAY_SIZE(name), format, index );
        swprintf( path, size, L"%s\\%s", folder, name );
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) break;
    }
    index++;
    if (key)
    {
        RegSetValueExW( key, L"ScreenshotIndex", 0, REG_DWORD, (BYTE *)&index, sizeof(index) );
        RegCloseKey( key );
    }
    return TRUE;
}

static HRESULT save_png( const WCHAR *path, const UINT32 *pixels, int width, int height )
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICBitmapEncoder *encoder = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    IWICStream *stream = NULL;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                           (void **)&factory );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename( stream, path, GENERIC_WRITE );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, &GUID_ContainerFormatPng, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, (IStream *)stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, width, height );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &format );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels( frame, height, width * sizeof(UINT32),
                                                                width * height * sizeof(UINT32), (BYTE *)pixels );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IWICStream_Release( stream );
    if (factory) IWICImagingFactory_Release( factory );
    if (FAILED(hr)) DeleteFileW( path );
    return hr;
}

/* the screen dims for a moment: the screenshot was taken */
static void dim_screen( const RECT *rect )
{
    HWND hwnd = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                 WS_EX_TRANSPARENT, L"Static", NULL, WS_POPUP | SS_BLACKRECT, rect->left, rect->top,
                                 rect->right - rect->left, rect->bottom - rect->top, 0, 0, 0, NULL );
    MSG msg;

    if (!hwnd) return;
    SetLayeredWindowAttributes( hwnd, 0, 96, LWA_ALPHA );
    ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    for (DWORD start = GetTickCount(); GetTickCount() - start < 250;)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
        Sleep( 15 );
    }
    DestroyWindow( hwnd );
}

enum screenshot { SCREEN, ACTIVE_WINDOW, SCREEN_TO_FILE };

static DWORD WINAPI screenshot_thread( void *arg )
{
    enum screenshot what = (enum screenshot)(UINT_PTR)arg;
    WCHAR path[MAX_PATH];
    UINT32 *pixels;
    HWND owner;
    RECT rect;

    SetThreadDescription( GetCurrentThread(), L"Screenshot" );
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );

    if (what == ACTIVE_WINDOW)
    {
        HWND active = GetForegroundWindow();
        if (!active || !GetWindowRect( GetAncestor( active, GA_ROOT ), &rect )) what = SCREEN;
    }
    if (what != ACTIVE_WINDOW)
        SetRect( &rect, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
                 GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ),
                 GetSystemMetrics( SM_YVIRTUALSCREEN ) + GetSystemMetrics( SM_CYVIRTUALSCREEN ) );

    if (!(pixels = capture_pixels( &rect )))
    {
        ERR( "no screenshot of %s\n", wine_dbgstr_rect( &rect ) );
        goto done;
    }
    owner = CreateWindowExW( 0, L"Message", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, NULL );
    set_clipboard( owner, pixels, rect.right - rect.left, rect.bottom - rect.top );
    if (owner) DestroyWindow( owner );

    if (what == SCREEN_TO_FILE && screenshot_name( path, ARRAY_SIZE(path) ))
    {
        HRESULT hr = save_png( path, pixels, rect.right - rect.left, rect.bottom - rect.top );
        if (FAILED(hr)) ERR( "%s: %#lx\n", debugstr_w(path), hr );
        else MESSAGE( "csrss: screenshot %s\n", debugstr_w(path) );
        dim_screen( &rect );
    }
    free( pixels );

done:
    CoUninitialize();
    InterlockedExchange( &busy, 0 );
    return 0;
}

/* Win+Shift+S: the Snipping Tool's overlay, as the shell starts it in
 * Windows 11; on a thread of its own, the keys must not wait for it */
static DWORD WINAPI snip_thread( void *arg )
{
    WCHAR cmdline[MAX_PATH + 16];
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    UINT len = GetSystemDirectoryW( cmdline + 1, MAX_PATH );

    cmdline[0] = '"';
    if (!len || len >= MAX_PATH) return 0;
    wcscat( cmdline, L"\\SnippingTool.exe\" /clip" );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        ERR( "%s: %lu\n", debugstr_w(cmdline), GetLastError() );
        return 0;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return 0;
}

void screen_snip(void)
{
    HANDLE thread;

    if ((thread = CreateThread( NULL, 0, snip_thread, NULL, 0, NULL ))) CloseHandle( thread );
}

/* the raw input thread saw PrintScreen go down */
void print_screen(void)
{
    enum screenshot what = SCREEN;
    HANDLE thread;

    if (GetAsyncKeyState( VK_LWIN ) < 0 || GetAsyncKeyState( VK_RWIN ) < 0) what = SCREEN_TO_FILE;
    else if (GetAsyncKeyState( VK_MENU ) < 0) what = ACTIVE_WINDOW;
    if (InterlockedCompareExchange( &busy, 1, 0 )) return;
    if ((thread = CreateThread( NULL, 0, screenshot_thread, (void *)(UINT_PTR)what, 0, NULL ))) CloseHandle( thread );
    else InterlockedExchange( &busy, 0 );
}
