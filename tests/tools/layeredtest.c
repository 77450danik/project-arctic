/* A layered window with per-pixel alpha (UpdateLayeredWindow): a red square
 * whose left half is opaque and right half half transparent, at 200,200 for
 * ten seconds, to see whether the desktop shows through. */
#include <windows.h>

int WINAPI WinMain( HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show )
{
    WNDCLASSW wc = { .lpfnWndProc = DefWindowProcW, .hInstance = inst, .lpszClassName = L"LayeredTest" };
    BITMAPINFO bi = {{ sizeof(bi.bmiHeader), 200, -100, 1, 32, BI_RGB }};
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 }, dst = { 200, 200 };
    SIZE size = { 200, 100 };
    DWORD *bits;
    HBITMAP bmp;
    HDC screen, mem;
    HWND hwnd;
    MSG msg;

    RegisterClassW( &wc );
    hwnd = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LayeredTest", L"",
                            WS_POPUP, 200, 200, 200, 100, NULL, NULL, inst, NULL );
    screen = GetDC( NULL );
    mem = CreateCompatibleDC( screen );
    bmp = CreateDIBSection( screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    SelectObject( mem, bmp );
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 200; x++)
            bits[y * 200 + x] = x < 100 ? 0xffff0000 : 0x80800000; /* premultiplied */
    UpdateLayeredWindow( hwnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA );
    ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    SetTimer( hwnd, 1, 60000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (msg.message == WM_TIMER) break;
        DispatchMessageW( &msg );
    }
    return 0;
}
