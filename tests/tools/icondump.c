/* The icons of the topmost window of a class, as the taskbar takes them
 * (WM_GETICON, then the class icons), written to D:\icondump-*.bmp: the
 * colour bitmap, the mask, and DrawIconEx at 16, 24, 30, 36 and 48 pixels
 * over white. "icondump CabinetWClass" — default: the foreground window. */
#include <windows.h>
#include <stdio.h>

static void save_bitmap( HBITMAP bitmap, const char *name )
{
    BITMAP bm;
    BITMAPINFOHEADER info = { sizeof(info) };
    BITMAPFILEHEADER file = { 0x4d42 };
    HDC hdc = GetDC( NULL );
    char path[MAX_PATH];
    DWORD size, written;
    void *bits;
    HANDLE out;

    if (!bitmap || !GetObjectW( bitmap, sizeof(bm), &bm )) return;
    info.biWidth = bm.bmWidth;
    info.biHeight = bm.bmHeight;
    info.biPlanes = 1;
    info.biBitCount = 32;
    size = bm.bmWidth * bm.bmHeight * 4;
    bits = malloc( size );
    GetDIBits( hdc, bitmap, 0, bm.bmHeight, bits, (BITMAPINFO *)&info, DIB_RGB_COLORS );
    ReleaseDC( NULL, hdc );
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + size;
    snprintf( path, sizeof(path), "D:\\icondump-%s.bmp", name );
    out = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    WriteFile( out, &file, sizeof(file), &written, NULL );
    WriteFile( out, &info, sizeof(info), &written, NULL );
    WriteFile( out, bits, size, &written, NULL );
    CloseHandle( out );
    free( bits );
}

static void dump( HICON icon, const char *name )
{
    static const int sizes[] = { 16, 24, 30, 36, 48 };
    ICONINFO ii;
    char sub[64];
    HDC screen = GetDC( NULL ), hdc = CreateCompatibleDC( screen );
    HBITMAP canvas = CreateCompatibleBitmap( screen, 48 * 5 + 40, 48 );
    RECT rc = { 0, 0, 48 * 5 + 40, 48 };
    int x = 0;

    if (!icon) return;
    if (GetIconInfo( icon, &ii ))
    {
        snprintf( sub, sizeof(sub), "%s-color", name );
        save_bitmap( ii.hbmColor, sub );
        snprintf( sub, sizeof(sub), "%s-mask", name );
        save_bitmap( ii.hbmMask, sub );
    }
    SelectObject( hdc, canvas );
    FillRect( hdc, &rc, GetStockObject( WHITE_BRUSH ) );
    for (int i = 0; i < 5; i++)
    {
        DrawIconEx( hdc, x, 0, icon, sizes[i], sizes[i], 0, NULL, DI_NORMAL );
        x += sizes[i] + 8;
    }
    SelectObject( hdc, GetStockObject( DEFAULT_GUI_FONT ) );
    snprintf( sub, sizeof(sub), "%s-drawn", name );
    DeleteDC( hdc );
    save_bitmap( canvas, sub );
    ReleaseDC( NULL, screen );
}

int WINAPI WinMain( HINSTANCE instance, HINSTANCE prev, char *cmdline, int show )
{
    HWND hwnd = *cmdline ? FindWindowA( cmdline, NULL ) : GetForegroundWindow();
    DWORD_PTR icon = 0;

    if (!hwnd) return 1;
    SendMessageTimeoutW( hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 1000, &icon );
    dump( (HICON)icon, "big" );
    icon = 0;
    SendMessageTimeoutW( hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 1000, &icon );
    dump( (HICON)icon, "small" );
    dump( (HICON)GetClassLongPtrW( hwnd, GCLP_HICON ), "class" );
    dump( (HICON)GetClassLongPtrW( hwnd, GCLP_HICONSM ), "classsm" );
    return 0;
}
