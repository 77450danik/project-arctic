/*
 * The pictures of Windows 11's context menus, for the taskbar's menus
 *
 * File Explorer's context menus show the pictures of shell32 (its resource
 * MENUICONS, drawn by tools/menu-icons.py, painted by menuicons.cpp in the
 * colours of USER's menus). The taskbar's menus draw over the taskbar's own
 * look, light or dark whatever USER's menus are, so the same pictures are
 * painted here in the colours of that look.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "startui.h"

/* the roles of a picture's parts, as tools/menu-icons.py numbers them */
enum { ROLE_LINES, ROLE_ACCENT, ROLE_INSIDE, ROLE_WHITE, ROLE_DARK, ROLE_GREY, ROLE_COUNT };

#define NO_COLOUR  0xffffffff

/* light, dark: menuicons.cpp's */
static const COLORREF colours[2][ROLE_COUNT] =
{
    { RGB( 0x42, 0x42, 0x42 ), RGB( 0x00, 0x78, 0xd4 ), RGB( 0xff, 0xff, 0xff ),
      RGB( 0xff, 0xff, 0xff ), RGB( 0x2b, 0x2b, 0x2b ), RGB( 0x6e, 0x6e, 0x6e ) },
    { RGB( 0xde, 0xde, 0xde ), RGB( 0x4c, 0xc2, 0xff ), NO_COLOUR,
      RGB( 0xff, 0xff, 0xff ), RGB( 0x2b, 0x2b, 0x2b ), RGB( 0x6e, 0x6e, 0x6e ) },
};

#define MAX_SIZES     8
#define MAX_PICTURES  128
#define MAX_CACHED    32

struct picture
{
    char name[32];
    BYTE roles;
    const BYTE *data[MAX_SIZES];
};

static BOOL parsed;
static UINT size_count, picture_count;
static BYTE sizes[MAX_SIZES];
static struct picture pictures[MAX_PICTURES];

static struct
{
    char name[32];
    int size;
    BOOL light;
    HBITMAP bitmap;
} cache[MAX_CACHED];
static UINT cached;

static const BYTE *skip_stream( const BYTE *p, const BYTE *end, UINT pixels )
{
    UINT n = 0;

    while (n < pixels && p + 2 <= end)
    {
        UINT literal = p[1];
        n += p[0] + literal;
        p += 2 + literal;
    }
    return n == pixels && p <= end ? p : NULL;
}

static void decode_stream( const BYTE *p, BYTE *out, UINT pixels )
{
    UINT n = 0;

    while (n < pixels)
    {
        UINT zeros = p[0], literal = p[1];

        p += 2;
        for (UINT i = 0; i < zeros && n < pixels; i++) out[n++] = 0;
        for (UINT i = 0; i < literal && n < pixels; i++) out[n++] = *p++;
    }
}

static BOOL parse(void)
{
    HMODULE shell32;
    HRSRC res;
    HGLOBAL data;
    const BYTE *p, *end;
    DWORD length;
    UINT icons;

    if (parsed) return picture_count != 0;
    parsed = TRUE;
    if (!(shell32 = GetModuleHandleW( L"shell32.dll" )) && !(shell32 = LoadLibraryW( L"shell32.dll" ))) return FALSE;
    if (!(res = FindResourceW( shell32, L"MENUICONS", (const WCHAR *)RT_RCDATA ))) return FALSE;
    if (!(data = LoadResource( shell32, res )) || !(p = LockResource( data ))) return FALSE;
    length = SizeofResource( shell32, res );
    if (length < 8 || memcmp( p, "AMI1", 4 )) return FALSE;
    end = p + length;
    icons = p[4] | (p[5] << 8);
    size_count = min( p[6], MAX_SIZES );
    p += 7;
    for (UINT i = 0; i < size_count; i++) sizes[i] = p[i];
    p += p[-1];
    for (UINT i = 0; i < icons && picture_count < MAX_PICTURES && p < end; i++)
    {
        struct picture *pic = &pictures[picture_count];
        UINT name_len = *p++;

        if (p + name_len + 1 > end) break;
        memcpy( pic->name, p, min( name_len, sizeof(pic->name) - 1 ) );
        pic->name[min( name_len, sizeof(pic->name) - 1 )] = 0;
        p += name_len;
        pic->roles = *p++;
        for (UINT s = 0; s < size_count && p; s++)
        {
            pic->data[s] = p;
            for (UINT r = 0; r < ROLE_COUNT && p; r++)
                if (pic->roles & (1 << r)) p = skip_stream( p, end, sizes[s] * sizes[s] );
        }
        if (!p) break;
        picture_count++;
    }
    return picture_count != 0;
}

/* a picture size by size, premultiplied, centred in a square of that size */
static HBITMAP paint_picture( const struct picture *pic, UINT index, int size, BOOL light )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    UINT side = sizes[index], pixels = side * side;
    int offset = (size - (int)side) / 2;
    const BYTE *p = pic->data[index];
    UINT *sum;
    BYTE *cover;
    DWORD *bits;
    HBITMAP bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );

    if (!bitmap) return NULL;
    memset( bits, 0, size * size * 4 );
    sum = calloc( pixels * 4, sizeof(*sum) );
    cover = malloc( pixels );
    if (!sum || !cover)
    {
        free( sum );
        free( cover );
        DeleteObject( bitmap );
        return NULL;
    }
    for (UINT r = 0; r < ROLE_COUNT; r++)
    {
        const BYTE *next;
        COLORREF c;

        if (!(pic->roles & (1 << r))) continue;
        next = skip_stream( p, p + 65536, pixels );
        c = colours[!light][r];
        if (c != NO_COLOUR)
        {
            decode_stream( p, cover, pixels );
            for (UINT i = 0; i < pixels; i++)
            {
                sum[i * 4 + 0] += cover[i] * GetBValue( c );
                sum[i * 4 + 1] += cover[i] * GetGValue( c );
                sum[i * 4 + 2] += cover[i] * GetRValue( c );
                sum[i * 4 + 3] += cover[i] * 255;
            }
        }
        p = next;
    }
    for (UINT y = 0; y < side; y++)
    {
        for (UINT x = 0; x < side; x++)
        {
            UINT i = y * side + x;
            int dx = offset + x, dy = offset + y;
            UINT b = min( sum[i * 4] / 255, 255u ), g = min( sum[i * 4 + 1] / 255, 255u );
            UINT rr = min( sum[i * 4 + 2] / 255, 255u ), a = min( sum[i * 4 + 3] / 255, 255u );

            if (dx < 0 || dy < 0 || dx >= size || dy >= size) continue;
            bits[dy * size + dx] = (a << 24) | (rr << 16) | (g << 8) | b;
        }
    }
    free( sum );
    free( cover );
    return bitmap;
}

/* one of the pictures, for a menu over a light or a dark look; kept, never freed */
HBITMAP menu_picture( const char *name, BOOL light, int size )
{
    UINT index = 0;
    HBITMAP bitmap = NULL;

    if (!name || !parse()) return NULL;
    for (UINT i = 0; i < cached; i++)
        if (cache[i].size == size && cache[i].light == light && !strcmp( cache[i].name, name )) return cache[i].bitmap;
    for (UINT i = 0; i < size_count; i++) if (sizes[i] <= size) index = i;
    for (UINT i = 0; i < picture_count; i++)
    {
        if (strcmp( pictures[i].name, name )) continue;
        bitmap = paint_picture( &pictures[i], index, size, light );
        break;
    }
    if (bitmap && cached < MAX_CACHED)
    {
        lstrcpynA( cache[cached].name, name, sizeof(cache[cached].name) );
        cache[cached].size = size;
        cache[cached].light = light;
        cache[cached].bitmap = bitmap;
        cached++;
    }
    return bitmap;
}
