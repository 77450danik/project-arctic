/*
 * Arctic WordPad: printing and the page's setup
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "wordpad.h"

static HGLOBAL devmode, devnames;

void print_document( HWND owner, BOOL ask )
{
    PRINTDLGW pd = { sizeof(pd) };
    DOCINFOW info = { sizeof(info) };
    int first = 1, last = 0, copies;

    pd.hwndOwner = owner;
    pd.hDevMode = devmode;
    pd.hDevNames = devnames;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION;
    pd.nMinPage = 1;
    pd.nMaxPage = (WORD)min( view_page_count(), 0xffff );
    pd.nFromPage = 1;
    pd.nToPage = pd.nMaxPage;
    pd.nCopies = 1;
    if (!ask) pd.Flags |= PD_RETURNDEFAULT;
    if (!PrintDlgW( &pd )) return;
    devmode = pd.hDevMode;
    devnames = pd.hDevNames;
    if (!pd.hDC) return;
    if (pd.Flags & PD_PAGENUMS)
    {
        first = pd.nFromPage;
        last = pd.nToPage;
    }
    copies = (pd.Flags & PD_USEDEVMODECOPIESANDCOLLATE) ? 1 : max( pd.nCopies, 1 );
    info.lpszDocName = document_name();
    if (StartDocW( pd.hDC, &info ) > 0)
    {
        SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_WAIT ) );
        for (int c = 0; c < copies; c++) view_print( pd.hDC, first, last, NULL );
        EndDoc( pd.hDC );
    }
    DeleteDC( pd.hDC );
}

/* Page Setup: the paper, its way round and the margins */
void page_setup_dialog( HWND owner )
{
    PAGESETUPDLGW psd = { sizeof(psd) };
    struct page_setup s = page;

    psd.hwndOwner = owner;
    psd.hDevMode = devmode;
    psd.hDevNames = devnames;
    psd.Flags = PSD_INHUNDREDTHSOFMILLIMETERS | PSD_MARGINS;
    psd.rtMargin.left = MulDiv( page.left, 2540, 1440 );
    psd.rtMargin.top = MulDiv( page.top, 2540, 1440 );
    psd.rtMargin.right = MulDiv( page.right, 2540, 1440 );
    psd.rtMargin.bottom = MulDiv( page.bottom, 2540, 1440 );
    if (!PageSetupDlgW( &psd )) return;
    devmode = psd.hDevMode;
    devnames = psd.hDevNames;
    s.left = MulDiv( psd.rtMargin.left, 1440, 2540 );
    s.top = MulDiv( psd.rtMargin.top, 1440, 2540 );
    s.right = MulDiv( psd.rtMargin.right, 1440, 2540 );
    s.bottom = MulDiv( psd.rtMargin.bottom, 1440, 2540 );
    if (psd.ptPaperSize.x > 0 && psd.ptPaperSize.y > 0)
    {
        s.width = MulDiv( psd.ptPaperSize.x, 1440, 2540 );
        s.height = MulDiv( psd.ptPaperSize.y, 1440, 2540 );
    }
    view_set_page( &s );
    set_modified( TRUE );
}
