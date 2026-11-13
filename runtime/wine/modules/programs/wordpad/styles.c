/*
 * Arctic WordPad: the paragraph styles
 *
 * The styles of Word's gallery, with Word's own names in styles.xml, so a
 * heading stays a heading in Word and in a table of contents. riched20
 * keeps a paragraph's style as its number (PARAFORMAT2.sStyle, \s in RTF):
 * the number is the index here plus one.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "wordpad.h"

WCHAR body_face[LF_FACESIZE] = L"Calibri";
WCHAR head_face[LF_FACESIZE] = L"Calibri Light";

#define BLUE_DARK  RGB( 0x2f, 0x54, 0x96 )
#define BLUE_DEEP  RGB( 0x1f, 0x37, 0x63 )

const struct style styles[STYLE_COUNT] =
{
    { L"Normal",        IDS_STYLE_NORMAL,    NULL, 22, FALSE, FALSE, CLR_DEFAULT, 0, 160, 259, 0 },
    { L"NoSpacing",     IDS_STYLE_NOSPACING, NULL, 22, FALSE, FALSE, CLR_DEFAULT, 0, 0, 240, 0 },
    { L"Heading1",      IDS_STYLE_H1,        L"", 32, FALSE, FALSE, BLUE_DARK, 480, 0, 259, 1 },
    { L"Heading2",      IDS_STYLE_H2,        L"", 26, FALSE, FALSE, BLUE_DARK, 40, 0, 259, 2 },
    { L"Heading3",      IDS_STYLE_H3,        L"", 24, FALSE, FALSE, BLUE_DEEP, 40, 0, 259, 3 },
    { L"Title",         IDS_STYLE_TITLE,     L"", 56, FALSE, FALSE, CLR_DEFAULT, 0, 0, 240, 0 },
    { L"Subtitle",      IDS_STYLE_SUBTITLE,  NULL, 22, FALSE, FALSE, RGB( 0x5a, 0x5a, 0x5a ), 0, 160, 259, 0 },
    { L"Quote",         IDS_STYLE_QUOTE,     NULL, 22, FALSE, TRUE, RGB( 0x40, 0x40, 0x40 ), 200, 160, 259, 0, 864 },
    { L"ListParagraph", IDS_STYLE_LIST,      NULL, 22, FALSE, FALSE, CLR_DEFAULT, 0, 160, 259, 0, 720 },
};

static BOOL font_exists( const WCHAR *name )
{
    LOGFONTW lf = { 0 };
    HDC hdc;
    BOOL found = FALSE;
    WCHAR value[LF_FACESIZE];
    DWORD size = sizeof(value);

    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW( lf.lfFaceName, name, LF_FACESIZE );
    /* a face of its own, or one the system gives under that name */
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
                       name, RRF_RT_REG_SZ, NULL, value, &size ))
        return TRUE;
    {
        HFONT font = CreateFontIndirectW( &lf ), old;
        WCHAR got[LF_FACESIZE] = { 0 };

        hdc = GetDC( 0 );
        old = SelectObject( hdc, font );
        GetTextFaceW( hdc, LF_FACESIZE, got );
        SelectObject( hdc, old );
        DeleteObject( font );
        ReleaseDC( 0, hdc );
        found = !wcsicmp( got, name );
    }
    return found;
}

static void choose_faces(void)
{
    static BOOL done;

    if (done) return;
    done = TRUE;
    if (!font_exists( L"Calibri" ))
    {
        lstrcpyW( body_face, L"Arial" );
        lstrcpyW( head_face, L"Arial" );
    }
    else if (!font_exists( L"Calibri Light" )) lstrcpyW( head_face, L"Calibri" );
}

static void style_formats( int index, CHARFORMAT2W *cf, PARAFORMAT2 *pf )
{
    const struct style *s = &styles[index];

    choose_faces();
    memset( cf, 0, sizeof(*cf) );
    cf->cbSize = sizeof(*cf);
    cf->dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_COLOR | CFM_CHARSET;
    lstrcpyW( cf->szFaceName, s->face ? head_face : body_face );
    cf->bCharSet = DEFAULT_CHARSET;
    cf->yHeight = s->size * 10;
    cf->dwEffects = (s->bold ? CFE_BOLD : 0) | (s->italic ? CFE_ITALIC : 0);
    if (s->color == CLR_DEFAULT) cf->dwEffects |= CFE_AUTOCOLOR;
    else cf->crTextColor = s->color;

    memset( pf, 0, sizeof(*pf) );
    pf->cbSize = sizeof(*pf);
    pf->dwMask = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_LINESPACING | PFM_STYLE | PFM_STARTINDENT | PFM_OFFSET |
                 PFM_KEEPNEXT | PFM_OUTLINELEVEL | PFM_ALIGNMENT | PFM_RIGHTINDENT;
    pf->dySpaceBefore = s->space_before;
    pf->dySpaceAfter = s->space_after;
    if (s->line == 240) pf->bLineSpacingRule = 0;
    else
    {
        pf->bLineSpacingRule = 5;
        pf->dyLineSpacing = (s->line * 20 + 120) / 240;   /* in twentieths of a line */
    }
    pf->sStyle = index + 1;
    pf->dxStartIndent = s->left_indent;
    pf->dxRightIndent = index == 7 ? 864 : 0;
    pf->wAlignment = index == 7 ? PFA_CENTER : PFA_LEFT;
    pf->wEffects = s->outline ? PFE_KEEPNEXT : 0;
    pf->bOutlineLevel = s->outline ? s->outline - 1 : 0;
}

/* the paragraphs of the selection take the style; Normal also clears what
 * was set on their letters */
void apply_style( int index )
{
    CHARFORMAT2W cf;
    PARAFORMAT2 pf;
    CHARRANGE sel, para;
    LONG length = ed( WM_GETTEXTLENGTH, 0, 0 );

    if (index < 0 || index >= STYLE_COUNT) return;
    style_formats( index, &cf, &pf );
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    ed( EM_SETPARAFORMAT, 0, (LPARAM)&pf );

    /* the letters of the whole paragraphs, as Word does */
    para.cpMin = ed( EM_LINEINDEX, ed( EM_EXLINEFROMCHAR, 0, sel.cpMin ), 0 );
    para.cpMax = sel.cpMax;
    {
        WCHAR buf[2] = { 0 };
        TEXTRANGEW range;

        /* back to the paragraph's start, forward to its end */
        while (para.cpMin > 0)
        {
            range.chrg.cpMin = para.cpMin - 1;
            range.chrg.cpMax = para.cpMin;
            range.lpstrText = buf;
            ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
            if (buf[0] == '\r') break;
            para.cpMin--;
        }
        while (para.cpMax < length)
        {
            range.chrg.cpMin = para.cpMax;
            range.chrg.cpMax = para.cpMax + 1;
            range.lpstrText = buf;
            ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
            if (buf[0] == '\r') break;
            para.cpMax++;
        }
    }
    view_freeze( TRUE );
    ed( EM_EXSETSEL, 0, (LPARAM)&para );
    if (index == 0 || index == 1 || index == 8)
    {
        /* Normal keeps what the letters were given, but not the size and
         * colour of a heading */
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET;
    }
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    ed( EM_EXSETSEL, 0, (LPARAM)&sel );
    view_freeze( FALSE );
}

int current_style(void)
{
    PARAFORMAT2 pf = { sizeof(pf) };

    ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
    if (!(pf.dwMask & PFM_STYLE)) return -1;
    if (pf.sStyle >= 1 && pf.sStyle <= STYLE_COUNT) return pf.sStyle - 1;
    return pf.sStyle == 0 ? 0 : -1;
}

/* a new document: Normal everywhere */
void apply_normal_defaults(void)
{
    CHARFORMAT2W cf;
    PARAFORMAT2 pf;

    style_formats( 0, &cf, &pf );
    ed( EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf );
    ed( EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf );
    ed( EM_SETSEL, 0, -1 );
    ed( EM_SETPARAFORMAT, 0, (LPARAM)&pf );
    ed( EM_SETSEL, 0, 0 );
}
