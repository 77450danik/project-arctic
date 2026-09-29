/* The mouse as games aim with it: raw input (WM_INPUT) from the mouse,
 * summed up, next to where the cursor went. A game turns its camera by the
 * raw sums; if they stay at zero while the cursor moves, aiming is dead.
 * Everything is drawn into the window, twenty times a second. */
#include <windows.h>
#include <stdio.h>

static LONG raw_x, raw_y, raw_count;

static LRESULT WINAPI proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INPUT:
    {
        RAWINPUT input;
        UINT size = sizeof(input);

        if (GetRawInputData( (HRAWINPUT)lp, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER) ) != (UINT)-1 &&
            input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            raw_x += input.data.mouse.lLastX;
            raw_y += input.data.mouse.lLastY;
            raw_count++;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow( hwnd );
        return 0;
    case WM_TIMER:
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        WCHAR text[256];
        POINT pt;
        RECT rc;
        HDC hdc = BeginPaint( hwnd, &ps );

        GetCursorPos( &pt );
        GetClientRect( hwnd, &rc );
        swprintf( text, ARRAYSIZE(text), L"raw events: %ld\nraw x: %ld\nraw y: %ld\ncursor: %ld, %ld",
                  raw_count, raw_x, raw_y, pt.x, pt.y );
        InflateRect( &rc, -20, -20 );
        DrawTextW( hdc, text, -1, &rc, DT_LEFT | DT_TOP );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI WinMain( HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show )
{
    WNDCLASSW cls = { .lpfnWndProc = proc, .hInstance = instance, .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                      .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1), .lpszClassName = L"rawmousetest" };
    RAWINPUTDEVICE device = { .usUsagePage = 1, .usUsage = 2 };
    HWND hwnd;
    MSG msg;

    RegisterClassW( &cls );
    hwnd = CreateWindowExW( 0, cls.lpszClassName, L"rawmousetest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 500, 300, NULL, NULL, instance, NULL );
    device.hwndTarget = hwnd;
    RegisterRawInputDevices( &device, 1, sizeof(device) );
    SetTimer( hwnd, 1, 50, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
