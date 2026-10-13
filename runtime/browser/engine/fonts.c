/*
 * Arctic web engine: fonts
 *
 * One cache for the layout, which measures, and the rasterizer, which
 * draws, so a run is drawn with the very font it was measured with. A font
 * stays until the document goes: the display list points at them.
 *
 * Until the text of stage 3 (DirectWrite, ICU) this is GDI, which every
 * Windows has.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "arctichtml_private.h"

static struct font **fonts;
static unsigned int font_count, font_capacity;
static HDC measure_dc;

HDC font_dc(void)
{
    if (!measure_dc) measure_dc = CreateCompatibleDC( NULL );
    return measure_dc;
}

const struct font *font_get( const struct font_key *key )
{
    LOGFONTW desc = { 0 };
    TEXTMETRICW metrics;
    struct font *font;
    HFONT old;
    SIZE size;
    HDC dc = font_dc();

    for (unsigned int i = 0; i < font_count; i++)
        if (!memcmp( &fonts[i]->key, key, sizeof(*key) )) return fonts[i];

    if (font_count == font_capacity)
    {
        unsigned int capacity = font_capacity ? font_capacity * 2 : 32;
        struct font **grown = realloc( fonts, capacity * sizeof(*grown) );

        if (!grown) return font_count ? fonts[0] : NULL;
        fonts = grown;
        font_capacity = capacity;
    }
    if (!(font = calloc( 1, sizeof(*font) ))) return font_count ? fonts[0] : NULL;
    font->key = *key;

    desc.lfHeight = -max( key->px, 1 );
    desc.lfWeight = key->weight;
    desc.lfItalic = (BYTE)key->italic;
    desc.lfUnderline = (BYTE)key->underline;
    desc.lfStrikeOut = (BYTE)key->strike;
    desc.lfCharSet = DEFAULT_CHARSET;
    desc.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW( desc.lfFaceName, key->face, LF_FACESIZE );
    font->handle = CreateFontIndirectW( &desc );

    old = SelectObject( dc, font->handle );
    GetTextMetricsW( dc, &metrics );
    font->ascent = metrics.tmAscent;
    font->descent = metrics.tmDescent;
    GetTextExtentPoint32W( dc, L" ", 1, &size );
    font->space = size.cx;
    SelectObject( dc, old );

    fonts[font_count++] = font;
    return font;
}

int font_measure( const struct font *font, const WCHAR *text, int len )
{
    HDC dc = font_dc();
    HFONT old;
    SIZE size = { 0 };

    if (len <= 0) return 0;
    old = SelectObject( dc, font->handle );
    GetTextExtentPoint32W( dc, text, len, &size );
    SelectObject( dc, old );
    return size.cx;
}

void font_cache_clear(void)
{
    for (unsigned int i = 0; i < font_count; i++)
    {
        DeleteObject( fonts[i]->handle );
        free( fonts[i] );
    }
    font_count = 0;
}

/* --- which families this machine has --- */

struct family
{
    WCHAR name[LF_FACESIZE];
    BOOL  exists;
};

static struct family *families;
static unsigned int family_count, family_capacity;

static int CALLBACK family_found( const LOGFONTW *font, const TEXTMETRICW *metrics, DWORD type, LPARAM param )
{
    *(BOOL *)param = TRUE;
    return 0;
}

BOOL font_family_exists( const WCHAR *name )
{
    LOGFONTW desc = { .lfCharSet = DEFAULT_CHARSET };
    BOOL found = FALSE;

    if (!*name || wcslen( name ) >= LF_FACESIZE) return FALSE;
    for (unsigned int i = 0; i < family_count; i++)
        if (!_wcsicmp( families[i].name, name )) return families[i].exists;

    lstrcpynW( desc.lfFaceName, name, LF_FACESIZE );
    EnumFontFamiliesExW( font_dc(), &desc, family_found, (LPARAM)&found, 0 );

    if (family_count == family_capacity)
    {
        unsigned int capacity = family_capacity ? family_capacity * 2 : 32;
        struct family *grown = realloc( families, capacity * sizeof(*grown) );

        if (!grown) return found;
        families = grown;
        family_capacity = capacity;
    }
    lstrcpynW( families[family_count].name, name, LF_FACESIZE );
    families[family_count++].exists = found;
    return found;
}

/* The faces a page names that Windows calls otherwise: what a page made on
 * a Mac or on Linux asks for, and what Arctic has in their place. */
static const WCHAR *const aliases[][2] =
{
    { L"Helvetica",        L"Arial" },
    { L"Helvetica Neue",   L"Arial" },
    { L"Liberation Sans",  L"Arial" },
    { L"Times",            L"Times New Roman" },
    { L"Liberation Serif", L"Times New Roman" },
    { L"Courier",          L"Courier New" },
    { L"Liberation Mono",  L"Courier New" },
    { L"Menlo",            L"Courier New" },
    { L"Monaco",           L"Courier New" },
    { L"Consolas",         L"Courier New" },
};

BOOL font_face_for( const WCHAR *name, WCHAR face[LF_FACESIZE] )
{
    if (font_family_exists( name ))
    {
        lstrcpynW( face, name, LF_FACESIZE );
        return TRUE;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(aliases); i++)
    {
        if (_wcsicmp( aliases[i][0], name ) || !font_family_exists( aliases[i][1] )) continue;
        lstrcpynW( face, aliases[i][1], LF_FACESIZE );
        return TRUE;
    }
    return FALSE;
}

/* the generic families: what Windows calls them, then what Arctic has */
void font_generic_face( enum generic_family generic, WCHAR face[LF_FACESIZE] )
{
    static const WCHAR *const choices[][3] =
    {
        [GENERIC_SERIF]     = { L"Times New Roman", L"Georgia", NULL },
        [GENERIC_SANS]      = { L"Arial", L"Segoe UI", NULL },
        [GENERIC_MONO]      = { L"Courier New", L"Consolas", NULL },
        [GENERIC_CURSIVE]   = { L"Comic Sans MS", L"Times New Roman", NULL },
        [GENERIC_FANTASY]   = { L"Impact", L"Arial", NULL },
        [GENERIC_SYSTEM_UI] = { L"Segoe UI", L"Source Sans Pro", L"Arial" },
    };

    for (unsigned int i = 0; i < 3 && choices[generic][i]; i++)
    {
        if (!font_family_exists( choices[generic][i] )) continue;
        lstrcpynW( face, choices[generic][i], LF_FACESIZE );
        return;
    }
    lstrcpynW( face, choices[generic][0], LF_FACESIZE );
}
