/*
 * Arctic web engine: the interface a host gives a page to show
 *
 * The engine owns its window: a host creates a view in its own window and
 * the engine makes the child, named "Internet Explorer_Server" as in
 * Windows, and from then on draws, scrolls and takes the input itself. The
 * hosts are iexplore.exe and, later, our mshtml.dll for programs that
 * embed the WebBrowser control (docs/browser.md).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_HTML_H
#define __WINE_ARCTIC_HTML_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct arctic_html_view *ARCTIC_HTML_VIEW;

/* sent to the host window when the title of the document changed */
#define ARCTIC_HTML_WM_TITLE    (WM_APP + 0x100)
/* sent when the document asks to go somewhere else; LPARAM is a WCHAR *
 * that stays valid until the message returns */
#define ARCTIC_HTML_WM_NAVIGATE (WM_APP + 0x101)

HRESULT WINAPI ArcticHtmlCreateView( HWND parent, const RECT *rect, ARCTIC_HTML_VIEW *view );
void WINAPI ArcticHtmlDestroyView( ARCTIC_HTML_VIEW view );
void WINAPI ArcticHtmlSetRect( ARCTIC_HTML_VIEW view, const RECT *rect );
HWND WINAPI ArcticHtmlGetWindow( ARCTIC_HTML_VIEW view );
HRESULT WINAPI ArcticHtmlNavigate( ARCTIC_HTML_VIEW view, const WCHAR *url );
HRESULT WINAPI ArcticHtmlGetTitle( ARCTIC_HTML_VIEW view, WCHAR *buffer, UINT count );
/* TRUE while the engine presents through Vulkan, FALSE on the software path */
BOOL WINAPI ArcticHtmlOnGpu( ARCTIC_HTML_VIEW view );

#ifdef __cplusplus
}
#endif

#endif /* __WINE_ARCTIC_HTML_H */
