/* Lists the visible top-level windows with what a visual style did to them:
 * rectangle, window region, activation. Output goes to stdout. */
#include <windows.h>
#include <stdio.h>

static BOOL CALLBACK show( HWND hwnd, LPARAM lparam )
{
    WCHAR title[128], cls[64];
    WINDOWINFO info = { sizeof(info) };
    HRGN rgn = CreateRectRgn( 0, 0, 0, 0 );
    RECT box = { 0 };
    int type;

    if (!IsWindowVisible( hwnd )) return TRUE;
    GetWindowTextW( hwnd, title, ARRAYSIZE(title) );
    GetClassNameW( hwnd, cls, ARRAYSIZE(cls) );
    GetWindowInfo( hwnd, &info );
    type = GetWindowRgn( hwnd, rgn );
    if (type != ERROR) GetRgnBox( rgn, &box );
    printf( "wininfo: %p %-20ls %-24ls rect %ld,%ld-%ld,%ld status %#lx region %d box %ld,%ld-%ld,%ld\n",
            hwnd, cls, title, info.rcWindow.left, info.rcWindow.top, info.rcWindow.right,
            info.rcWindow.bottom, info.dwWindowStatus, type, box.left, box.top, box.right, box.bottom );
    DeleteObject( rgn );
    return TRUE;
}

int main( void )
{
    printf( "wininfo: foreground %p\n", GetForegroundWindow() );
    EnumWindows( show, 0 );
    return 0;
}
