/* The keyboard as a game reads it: DirectInput 8 in foreground exclusive
 * mode (Wine serves it from WM_INPUT), next to the window's own WM_KEYDOWN
 * and whether the window is the foreground one. Everything is drawn into the
 * window, twenty times a second. "dinputtest fullscreen" covers the primary
 * monitor without a frame, as a game does; Esc closes it. */
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

static IDirectInputDevice8W *keyboard;
static HRESULT acquire_hr = E_FAIL, state_hr = E_FAIL;
static unsigned int keydowns, dinput_presses;
static BYTE last_keys[256];
static WCHAR held[256];

static void poll_keyboard( HWND hwnd )
{
    BYTE keys[256];
    int i, len = 0;

    if (!keyboard) return;
    state_hr = IDirectInputDevice8_GetDeviceState( keyboard, sizeof(keys), keys );
    if (state_hr == DIERR_INPUTLOST || state_hr == DIERR_NOTACQUIRED)
    {
        acquire_hr = IDirectInputDevice8_Acquire( keyboard );
        return;
    }
    if (FAILED(state_hr)) return;
    held[0] = 0;
    for (i = 0; i < 256; i++)
    {
        if ((keys[i] & 0x80) && !(last_keys[i] & 0x80)) dinput_presses++;
        if ((keys[i] & 0x80) && len < 240) len += swprintf( held + len, 256 - len, L"%02x ", i );
    }
    memcpy( last_keys, keys, sizeof(keys) );
}

static LRESULT WINAPI proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_KEYDOWN:
        keydowns++;
        if (wp == VK_ESCAPE) DestroyWindow( hwnd );
        return 0;
    case WM_TIMER:
        poll_keyboard( hwnd );
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        WCHAR text[512];
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT rc;

        GetClientRect( hwnd, &rc );
        swprintf( text, ARRAYSIZE(text),
                  L"foreground: %s\nfocus: %s\nWM_KEYDOWN: %u\nDirectInput acquire: 0x%08lx\n"
                  L"DirectInput state: 0x%08lx\nDirectInput presses: %u\nheld: %s",
                  GetForegroundWindow() == hwnd ? L"yes" : L"no", GetFocus() == hwnd ? L"yes" : L"no",
                  keydowns, acquire_hr, state_hr, dinput_presses, held );
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
                      .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1), .lpszClassName = L"dinputtest" };
    BOOL fullscreen = strstr( cmdline, "fullscreen" ) != NULL;
    IDirectInput8W *dinput;
    HWND hwnd;
    MSG msg;
    HRESULT hr;

    RegisterClassW( &cls );
    if (fullscreen)
        hwnd = CreateWindowExW( WS_EX_TOPMOST, cls.lpszClassName, L"dinputtest", WS_POPUP | WS_VISIBLE, 0, 0,
                                GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ), NULL, NULL, instance, NULL );
    else
        hwnd = CreateWindowExW( 0, cls.lpszClassName, L"dinputtest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 400, NULL, NULL, instance, NULL );

    hr = DirectInput8Create( instance, DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&dinput, NULL );
    if (SUCCEEDED(hr)) hr = IDirectInput8_CreateDevice( dinput, &GUID_SysKeyboard, &keyboard, NULL );
    if (SUCCEEDED(hr)) hr = IDirectInputDevice8_SetDataFormat( keyboard, &c_dfDIKeyboard );
    if (SUCCEEDED(hr)) hr = IDirectInputDevice8_SetCooperativeLevel( keyboard, hwnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE );
    if (SUCCEEDED(hr)) acquire_hr = IDirectInputDevice8_Acquire( keyboard );
    printf( "dinput 0x%08lx, acquire 0x%08lx\n", hr, acquire_hr );

    SetTimer( hwnd, 1, 50, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
