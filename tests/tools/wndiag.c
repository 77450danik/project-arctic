/* The child windows of a top-level window whose title starts with argv[1],
 * and which window is under the centre of each button (a control that covers
 * another one eats its clicks).
 *   x86_64-w64-mingw32-gcc -O1 -o wndiag.exe tests/tools/wndiag.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HWND found;
static const char *prefix;

static BOOL CALLBACK top( HWND hwnd, LPARAM lp )
{
    char title[256];

    GetWindowTextA( hwnd, title, sizeof(title) );
    if (IsWindowVisible( hwnd ) && !strncmp( title, prefix, strlen( prefix ) )) { found = hwnd; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK child( HWND hwnd, LPARAM lp )
{
    char cls[64], text[64];
    RECT rc;

    GetClassNameA( hwnd, cls, sizeof(cls) );
    GetWindowTextA( hwnd, text, sizeof(text) );
    GetWindowRect( hwnd, &rc );
    if (!IsWindowVisible( hwnd )) return TRUE;
    printf( "%5d %-16s %4ld,%4ld %4ldx%-4ld %s", GetDlgCtrlID( hwnd ), cls, rc.left, rc.top, rc.right - rc.left,
            rc.bottom - rc.top, text );
    if (!strcmp( cls, "Button" ))
    {
        POINT pt = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
        HWND under = WindowFromPoint( pt );

        GetClassNameA( under, cls, sizeof(cls) );
        if (under != hwnd) printf( "   <- covered by %d %s", GetDlgCtrlID( under ), cls );
    }
    printf( "\n" );
    return TRUE;
}

int main( int argc, char **argv )
{
    prefix = argc > 1 ? argv[1] : "Rufus";
    EnumWindows( top, 0 );
    if (!found) { printf( "no window\n" ); return 1; }
    EnumChildWindows( found, child, 0 );
    return 0;
}
