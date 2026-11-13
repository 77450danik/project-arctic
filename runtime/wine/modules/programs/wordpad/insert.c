/*
 * Arctic WordPad: what the Insert tab puts in the text
 *
 * Tables, pictures and page breaks go in as RTF at the selection, as riched20
 * reads them; a picture is a metafile of its pixels, the form riched20 keeps.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include "wordpad.h"

static void insert_rtf( const char *rtf, size_t len )
{
    struct buffer b = { 0 };

    (void)b;
    stream_in( rtf, len, SF_RTF | SFF_SELECTION );
    set_modified( TRUE );
    view_repaginate();
    view_scroll_caret();
}

void insert_text( const WCHAR *text )
{
    ed( EM_REPLACESEL, TRUE, (LPARAM)text );
    view_scroll_caret();
}

void insert_table( int columns, int rows )
{
    struct buffer b = { 0 };
    int width = page.width - page.left - page.right, x = 0;

    if (columns < 1 || rows < 1) return;
    buf_str( &b, "{\\rtf1\\ansi " );
    for (int r = 0; r < rows; r++)
    {
        buf_str( &b, "\\trowd\\trgaph108\\trleft0" );
        x = 0;
        for (int c = 0; c < columns; c++)
        {
            x += width / columns;
            buf_printf( &b, "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10"
                            "\\clbrdrr\\brdrs\\brdrw10\\cellx%d", x );
        }
        for (int c = 0; c < columns; c++) buf_str( &b, "\\pard\\intbl\\sa0\\sl240\\slmult1 \\cell " );
        buf_str( &b, "\\row " );
    }
    buf_str( &b, "\\pard\\par}" );
    insert_rtf( b.data, b.len );
    buf_free( &b );
}

void insert_page_break(void)
{
    PARAFORMAT2 pf = { sizeof(pf) };

    ed( EM_REPLACESEL, TRUE, (LPARAM)L"\r" );
    pf.dwMask = PFM_PAGEBREAKBEFORE;
    pf.wEffects = PFE_PAGEBREAKBEFORE;
    ed( EM_SETPARAFORMAT, 0, (LPARAM)&pf );
    set_modified( TRUE );
    view_repaginate();
    view_scroll_caret();
}

/* a picture as riched20 keeps it: a metafile of its pixels, at most the
 * width of the page's text */
BOOL picture_rtf( struct buffer *out, const BYTE *data, size_t len, int goal_w, int goal_h )
{
    BITMAPINFOHEADER *bih;
    int width, height;
    size_t dib_len;
    /* the pixels of 144 dpi at the size it is shown: enough for screen and paper */
    BYTE *dib = image_to_dib( data, len, goal_w > 0 ? goal_w / 10 : 0, goal_h > 0 ? goal_h / 10 : 0,
                              &width, &height, &dib_len );
    HDC emf_dc;
    HENHMETAFILE emf;
    RECT frame_rect;
    UINT emf_len;
    BYTE *emf_bits;
    int text_w = page.width - page.left - page.right;

    if (!dib) return FALSE;
    bih = (BITMAPINFOHEADER *)dib;
    if (goal_w <= 0 || goal_h <= 0)
    {
        /* its own size at 96 dpi, smaller to fit the page */
        goal_w = width * 15;
        goal_h = height * 15;
        if (goal_w > text_w)
        {
            goal_h = MulDiv( goal_h, text_w, goal_w );
            goal_w = text_w;
        }
    }
    SetRect( &frame_rect, 0, 0, MulDiv( goal_w, 2540, 1440 ), MulDiv( goal_h, 2540, 1440 ) );
    emf_dc = CreateEnhMetaFileW( NULL, NULL, &frame_rect, NULL );
    SetStretchBltMode( emf_dc, HALFTONE );
    StretchDIBits( emf_dc, 0, 0, width, height, 0, 0, width, height, dib + bih->biSize, (BITMAPINFO *)bih,
                   DIB_RGB_COLORS, SRCCOPY );
    emf = CloseEnhMetaFile( emf_dc );
    free( dib );
    if (!emf) return FALSE;
    emf_len = GetEnhMetaFileBits( emf, 0, NULL );
    if ((emf_bits = malloc( emf_len )))
    {
        GetEnhMetaFileBits( emf, emf_len, emf_bits );
        buf_printf( out, "{\\pict\\emfblip\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\n", width, height, goal_w, goal_h );
        hex_bytes( out, emf_bits, emf_len );
        buf_str( out, "}" );
        free( emf_bits );
    }
    DeleteEnhMetaFile( emf );
    return emf_bits != NULL;
}

void insert_picture( HWND owner )
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[512], path[MAX_PATH] = L"";
    struct buffer rtf = { 0 };
    HANDLE file;
    BYTE *data;
    DWORD size, done;

    lstrcpynW( filter, string( IDS_PICTURE_FILTER ), ARRAY_SIZE(filter) );
    for (WCHAR *p = filter; *p; p++) if (*p == '|') *p = 0;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW( &ofn )) return;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize( file, NULL );
    if (size != INVALID_FILE_SIZE && (data = malloc( size )))
    {
        if (ReadFile( file, data, size, &done, NULL ) && done == size)
        {
            buf_str( &rtf, "{\\rtf1\\ansi " );
            if (picture_rtf( &rtf, data, size, 0, 0 ))
            {
                buf_str( &rtf, "}" );
                insert_rtf( rtf.data, rtf.len );
            }
            else MessageBoxW( owner, path, string( IDS_APP_NAME ), MB_ICONWARNING );
        }
        free( data );
    }
    CloseHandle( file );
    buf_free( &rtf );
}

/* Change Case: each stretch of letters changed is replaced alone, so the
 * letters keep their formats */
void change_case( int mode )
{
    CHARRANGE sel, keep;
    WCHAR *text, *changed;
    TEXTRANGEW range;
    int len;
    BOOL start_of_sentence = TRUE, start_of_word = TRUE;

    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (sel.cpMin == sel.cpMax)
    {
        sel.cpMin = ed( EM_FINDWORDBREAK, WB_MOVEWORDLEFT, sel.cpMin + 1 );
        sel.cpMax = ed( EM_FINDWORDBREAK, WB_RIGHTBREAK, sel.cpMin );
    }
    keep = sel;
    len = sel.cpMax - sel.cpMin;
    if (len <= 0 || !(text = malloc( (len + 1) * sizeof(WCHAR) ))) return;
    if (!(changed = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        free( text );
        return;
    }
    range.chrg = sel;
    range.lpstrText = text;
    len = ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
    for (int i = 0; i < len; i++)
    {
        WCHAR c = text[i], up = (WCHAR)(ULONG_PTR)CharUpperW( (WCHAR *)(ULONG_PTR)c ), low = (WCHAR)(ULONG_PTR)CharLowerW( (WCHAR *)(ULONG_PTR)c );
        BOOL letter = IsCharAlphaW( c );

        switch (mode)
        {
        case IDM_CASE_UPPER: changed[i] = up; break;
        case IDM_CASE_LOWER: changed[i] = low; break;
        case IDM_CASE_TOGGLE: changed[i] = c == up ? low : up; break;
        case IDM_CASE_TITLE: changed[i] = letter ? (start_of_word ? up : low) : c; break;
        default: changed[i] = letter ? (start_of_sentence ? up : low) : c; break;
        }
        if (letter) start_of_sentence = start_of_word = FALSE;
        else
        {
            start_of_word = c == ' ' || c == '\t' || c == '\r' || c == '-' || c == '(' || c == '"';
            if (c == '.' || c == '!' || c == '?' || c == '\r') start_of_sentence = TRUE;
        }
    }
    view_freeze( TRUE );
    for (int i = 0; i < len; )
    {
        int j = i;
        if (changed[i] == text[i])
        {
            i++;
            continue;
        }
        while (j < len && changed[j] != text[j]) j++;
        {
            CHARRANGE part = { sel.cpMin + i, sel.cpMin + j };
            WCHAR save = changed[j];

            changed[j] = 0;
            ed( EM_EXSETSEL, 0, (LPARAM)&part );
            ed( EM_REPLACESEL, TRUE, (LPARAM)(changed + i) );
            changed[j] = save;
        }
        i = j;
    }
    ed( EM_EXSETSEL, 0, (LPARAM)&keep );
    view_freeze( FALSE );
    free( text );
    free( changed );
}
