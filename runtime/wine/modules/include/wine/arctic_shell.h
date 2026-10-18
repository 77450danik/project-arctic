/*
 * The shell's look for its flyouts and the icons of the notification area
 *
 * startui.dll keeps the look chosen on the taskbar's page "Оформлення".
 * A flyout takes it when it shows (arctic_shell_look) and learns whether
 * it draws white on black, which dwm.exe keys by brightness, or, over a
 * light look, dark on white, keyed by darkness (DWMWA_ARCTIC_LIGHT). The
 * icons of the notification area follow the taskbar itself. When the look
 * changes, "ArcticShellLookChanged" is posted to every top-level window.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_SHELL_H
#define __WINE_ARCTIC_SHELL_H

static inline FARPROC arctic_startui_proc( const char *name )
{
    HMODULE module = GetModuleHandleW( L"startui.dll" );

    if (!module) module = LoadLibraryW( L"startui.dll" );
    return module ? GetProcAddress( module, name ) : NULL;
}

/* the look on a flyout: TRUE when it has to draw dark on white */
static inline BOOL arctic_shell_look( HWND hwnd )
{
    BOOL (WINAPI *apply_ex)( HWND ) = (void *)arctic_startui_proc( "ArcticApplyShellLookEx" );
    void (WINAPI *apply)( HWND );

    if (apply_ex) return apply_ex( hwnd );
    /* an older startui: white on black */
    if ((apply = (void *)arctic_startui_proc( "ArcticApplyShellLook" ))) apply( hwnd );
    return FALSE;
}

/* whether the taskbar draws dark on white now, for its icons */
static inline BOOL arctic_taskbar_is_light(void)
{
    BOOL (WINAPI *is_light)(void) = (void *)arctic_startui_proc( "ArcticTaskbarIsLight" );

    return is_light && is_light();
}

static inline UINT arctic_look_changed_message(void)
{
    return RegisterWindowMessageW( L"ArcticShellLookChanged" );
}

/* the colour of text and glyphs */
#define ARCTIC_INK( light ) ((light) ? RGB( 0, 0, 0 ) : RGB( 255, 255, 255 ))

/* GDI text has no alpha: dwm.exe takes its brightness (or, light, its
 * darkness) for it, so a grey says how opaque white (or black) text is */
static inline COLORREF arctic_keyed_grey( BOOL light, BYTE alpha )
{
    return light ? RGB( 255 - alpha, 255 - alpha, 255 - alpha ) : RGB( alpha, alpha, alpha );
}

/* what shows the backdrop through */
static inline void arctic_fill_back( HDC hdc, const RECT *rect, BOOL light )
{
    FillRect( hdc, rect, GetStockObject( light ? WHITE_BRUSH : BLACK_BRUSH ) );
}

#endif  /* __WINE_ARCTIC_SHELL_H */
