/* Prints what uxtheme does with the window frame parts of the active visual
 * style: whether it opens them, draws them and gives them a region. Output
 * goes to stdout, which arctic-init passes to the serial log. */
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>

static void try_part( HTHEME theme, const char *name, int part, int state, int w, int h )
{
    BITMAPINFO bmi = { { sizeof(bmi.bmiHeader), w, -h, 1, 32 } };
    RECT rect = { 0, 0, w, h };
    HDC dc = CreateCompatibleDC( 0 );
    void *bits;
    HBITMAP bmp = CreateDIBSection( dc, &bmi, DIB_RGB_COLORS, &bits, 0, 0 );
    HBRUSH magenta = CreateSolidBrush( RGB(255, 0, 255) );
    HRGN rgn = 0;
    HRESULT hr, hr_rgn;
    int bgtype = -1, x, y, changed = 0;
    RGNDATAHEADER rdh = { 0 };
    DWORD *px;

    SelectObject( dc, bmp );
    FillRect( dc, &rect, magenta );
    GetThemeEnumValue( theme, part, state, TMT_BGTYPE, &bgtype );
    hr = DrawThemeBackground( theme, dc, part, state, &rect, NULL );
    GdiFlush();
    for (px = bits, y = 0; y < h; y++)
        for (x = 0; x < w; x++, px++)
            if ((*px & 0xffffff) != 0xff00ff) changed++;
    hr_rgn = GetThemeBackgroundRegion( theme, 0, part, state, &rect, &rgn );
    if (rgn) GetRegionData( rgn, sizeof(rdh), (RGNDATA *)&rdh );
    printf( "themetest: %-12s bgtype %d draw %#lx changed %d/%d px, first %08lx; region %#lx rects %lu\n",
            name, bgtype, hr, changed, w * h, *(DWORD *)bits, hr_rgn, rdh.nCount );
    DeleteObject( magenta );
    DeleteObject( bmp );
    DeleteDC( dc );
}

int main( void )
{
    WCHAR file[MAX_PATH], color[64], size[64];
    HTHEME theme;
    HWND hwnd;

    printf( "themetest: before a window: IsThemeActive %d IsAppThemed %d\n", IsThemeActive(), IsAppThemed() );
    /* the theme joins a process with its first window, as the user api hook is set up then */
    hwnd = CreateWindowW( L"STATIC", L"themetest", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, 0, 0 );
    printf( "themetest: window %p, GetWindowTheme %p\n", hwnd, GetWindowTheme( hwnd ) );

    printf( "themetest: IsThemeActive %d IsAppThemed %d props %#lx\n",
            IsThemeActive(), IsAppThemed(), GetThemeAppProperties() );
    if (SUCCEEDED( GetCurrentThemeName( file, MAX_PATH, color, 64, size, 64 ) ))
        printf( "themetest: theme %ls color %ls size %ls\n", file, color, size );

    theme = OpenThemeData( NULL, L"WINDOW" );
    printf( "themetest: OpenThemeData(WINDOW) %p\n", theme );
    if (theme)
    {
        try_part( theme, "CAPTION", WP_CAPTION, CS_ACTIVE, 400, 30 );
        try_part( theme, "FRAMELEFT", WP_FRAMELEFT, FS_ACTIVE, 8, 200 );
        try_part( theme, "CLOSEBUTTON", WP_CLOSEBUTTON, CBS_NORMAL, 21, 21 );
        CloseThemeData( theme );
    }
    {
        /* a real frame: who paints its caption, and in which state */
        WINDOWINFO info = { sizeof(info) };
        HWND frame = CreateWindowW( L"STATIC", L"themetest frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    300, 200, 400, 200, 0, 0, 0, 0 );
        DWORD end = GetTickCount() + 1500;
        MSG msg;
        HDC dc;

        SetForegroundWindow( frame );
        while (GetTickCount() < end)
        {
            while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
            Sleep( 20 );
        }
        GetWindowInfo( frame, &info );
        dc = GetWindowDC( frame );
        printf( "themetest: frame fg %d status %#lx theme %p caption px %06lx %06lx %06lx border px %06lx\n",
                GetForegroundWindow() == frame, info.dwWindowStatus, GetWindowTheme( frame ),
                GetPixel( dc, 60, 12 ), GetPixel( dc, 200, 12 ), GetPixel( dc, 390, 12 ), GetPixel( dc, 2, 100 ) );
        ReleaseDC( frame, dc );
        DestroyWindow( frame );
    }

    theme = OpenThemeData( NULL, L"TASKBAR" );
    if (theme)
    {
        try_part( theme, "TASKBAR", TBP_BACKGROUNDBOTTOM, 0, 400, 30 );
        CloseThemeData( theme );
    }
    theme = OpenThemeData( NULL, L"BUTTON" );
    if (theme)
    {
        try_part( theme, "PUSHBUTTON", BP_PUSHBUTTON, PBS_NORMAL, 80, 24 );
        CloseThemeData( theme );
    }
    return 0;
}
