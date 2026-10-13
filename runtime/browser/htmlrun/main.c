/*
 * htmlrun: the engine with no window (docs/browser.md, component 7)
 *
 *   htmlrun URL OUT.png [WIDTH [HEIGHT]] [--full]
 *
 * Lays the page out at WIDTH x HEIGHT (1024 x 768 by default), draws it and
 * writes the picture; --full makes it as tall as the page. The reference
 * screenshots and the web-platform-tests run on it.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <windows.h>

#include "arctic_html.h"

int wmain( int argc, WCHAR *argv[] )
{
    const WCHAR *url = NULL, *out = NULL;
    UINT width = 1024, height = 768, numbers = 0;
    BOOL full = FALSE;
    DWORD start;
    HRESULT hr;

    for (int i = 1; i < argc; i++)
    {
        if (!wcscmp( argv[i], L"--full" )) full = TRUE;
        else if (!url) url = argv[i];
        else if (!out) out = argv[i];
        else if (numbers++ == 0) width = (UINT)_wtoi( argv[i] );
        else height = (UINT)_wtoi( argv[i] );
    }
    if (!url || !out || !width || !height)
    {
        fwprintf( stderr, L"usage: htmlrun URL OUT.png [WIDTH [HEIGHT]] [--full]\n" );
        return 2;
    }
    start = GetTickCount();
    hr = ArcticHtmlRenderToFile( url, width, height, full, out );
    if (FAILED(hr))
    {
        fwprintf( stderr, L"htmlrun: %ls: error %#lx\n", url, hr );
        return 1;
    }
    wprintf( L"%ls -> %ls in %lu ms\n", url, out, GetTickCount() - start );
    return 0;
}
