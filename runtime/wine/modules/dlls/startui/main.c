/*
 * The Start menu of Arctic (startui.dll)
 *
 * explorer (ReactOS, patch 0037) shows these menus instead of its classic one
 * unless "Classic Start menu" is chosen in the taskbar's properties: the
 * standard one of two columns, or those of Windows 10 and 11, as the page
 * "Меню «Пуск»" chooses (startpage.c). It calls in on the taskbar's thread:
 * ArcticStartMenuToggle for the Start button, the Windows key and Ctrl+Esc,
 * ArcticStartMenuHide when it has to go.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "startui.h"

HINSTANCE startui_instance;

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][256];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( startui_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        startui_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}

/* the Start button (its screen rectangle) was clicked, or the Windows key or
 * Ctrl+Esc pressed (click FALSE): open the menu, or close it if it is open */
void WINAPI ArcticStartMenuToggle( HWND tray, const RECT *button, BOOL click )
{
    int style = start_style_load();

    /* the one open closes, whichever it is */
    if (menu_visible() || menu10_visible() || menu11_visible())
    {
        BOOL same = (style == STYLE_WIN10 && menu10_visible()) || (style == STYLE_WIN11 && menu11_visible()) ||
                    (style != STYLE_WIN10 && style != STYLE_WIN11 && menu_visible());
        menu_hide();
        menu10_hide();
        menu11_hide();
        if (same) return;
    }
    switch (style)
    {
    case STYLE_WIN10: menu10_toggle( tray, button, click ); break;
    case STYLE_WIN11: menu11_toggle( tray, button, click ); break;
    default:          menu_toggle( tray, button, click ); break;
    }
}

void WINAPI ArcticStartMenuHide(void)
{
    menu_hide();
    menu10_hide();
    menu11_hide();
}

BOOL WINAPI ArcticStartMenuVisible(void)
{
    return menu_visible() || menu10_visible() || menu11_visible();
}

/* "Customize..." of the Start menu page in the taskbar's properties */
void WINAPI ArcticStartMenuCustomize( HWND owner )
{
    customize_dialog( owner );
}
