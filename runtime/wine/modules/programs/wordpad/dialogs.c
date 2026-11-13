/*
 * Arctic WordPad: dialogs
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include "wordpad.h"

/**********************************************************************
 *          Paragraph
 */

static void set_cm( HWND dlg, int id, int twips )
{
    WCHAR text[32];
    int hundredths = MulDiv( twips, 254, 144 );

    swprintf( text, ARRAY_SIZE(text), L"%d,%02d", hundredths / 100, abs( hundredths % 100 ) );
    SetDlgItemTextW( dlg, id, text );
}

static int get_cm( HWND dlg, int id )
{
    WCHAR text[32];

    GetDlgItemTextW( dlg, id, text, ARRAY_SIZE(text) );
    for (WCHAR *p = text; *p; p++) if (*p == ',') *p = '.';
    return (int)(wcstod( text, NULL ) * 1440 / 2.54 + (text[0] == '-' ? -0.5 : 0.5));
}

static void set_pt( HWND dlg, int id, int twips )
{
    WCHAR text[32];

    if (twips % 20) swprintf( text, ARRAY_SIZE(text), L"%d,%d", twips / 20, (twips % 20) * 10 / 20 );
    else swprintf( text, ARRAY_SIZE(text), L"%d", twips / 20 );
    SetDlgItemTextW( dlg, id, text );
}

static int get_pt( HWND dlg, int id )
{
    WCHAR text[32];

    GetDlgItemTextW( dlg, id, text, ARRAY_SIZE(text) );
    for (WCHAR *p = text; *p; p++) if (*p == ',') *p = '.';
    return (int)(wcstod( text, NULL ) * 20 + 0.5);
}

static INT_PTR CALLBACK paragraph_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    static PARAFORMAT2 pf;

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND align = GetDlgItem( dlg, IDC_PARA_ALIGN ), special = GetDlgItem( dlg, IDC_PARA_SPECIAL );
        HWND line = GetDlgItem( dlg, IDC_PARA_LINE );
        int left, first, rule = 0;

        pf.cbSize = sizeof(pf);
        ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
        for (int i = IDM_ALIGN_LEFT; i <= IDM_ALIGN_JUSTIFY; i++)
            SendMessageW( align, CB_ADDSTRING, 0, (LPARAM)string( IDS_CMD( i ) ) );
        SendMessageW( align, CB_SETCURSEL, pf.wAlignment == PFA_CENTER ? 1 : pf.wAlignment == PFA_RIGHT ? 2 :
                      pf.wAlignment == PFA_JUSTIFY ? 3 : 0, 0 );
        /* left = start + offset; the first line is start */
        left = pf.dxStartIndent + pf.dxOffset;
        first = -pf.dxOffset;
        set_cm( dlg, IDC_PARA_LEFT, left );
        set_cm( dlg, IDC_PARA_RIGHT, pf.dxRightIndent );
        for (int i = 0; i < 3; i++) SendMessageW( special, CB_ADDSTRING, 0, (LPARAM)string( 3000 + i ) );
        SendMessageW( special, CB_SETCURSEL, first > 0 ? 1 : first < 0 ? 2 : 0, 0 );
        set_cm( dlg, IDC_PARA_BY, abs( first ) );
        set_pt( dlg, IDC_PARA_BEFORE, pf.dySpaceBefore );
        set_pt( dlg, IDC_PARA_AFTER, pf.dySpaceAfter );
        for (int i = 0; i < 6; i++) SendMessageW( line, CB_ADDSTRING, 0, (LPARAM)string( 3010 + i ) );
        switch (pf.bLineSpacingRule)
        {
        case 0: rule = 0; break;
        case 1: rule = 1; break;
        case 2: rule = 2; break;
        case 3: rule = 3; break;
        case 4: rule = 4; break;
        case 5: rule = pf.dyLineSpacing == 20 ? 0 : pf.dyLineSpacing == 30 ? 1 : pf.dyLineSpacing == 40 ? 2 : 5; break;
        }
        SendMessageW( line, CB_SETCURSEL, rule, 0 );
        if (rule == 3 || rule == 4) set_pt( dlg, IDC_PARA_AT, pf.dyLineSpacing );
        else if (rule == 5)
        {
            WCHAR text[16];
            swprintf( text, ARRAY_SIZE(text), L"%d,%02d", pf.dyLineSpacing / 20, (pf.dyLineSpacing % 20) * 5 );
            SetDlgItemTextW( dlg, IDC_PARA_AT, text );
        }
        CheckDlgButton( dlg, IDC_PARA_PAGEBREAK, pf.wEffects & PFE_PAGEBREAKBEFORE ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( dlg, IDC_PARA_KEEPNEXT, pf.wEffects & PFE_KEEPNEXT ? BST_CHECKED : BST_UNCHECKED );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD( wparam ) == IDOK)
        {
            static const WORD aligns[] = { PFA_LEFT, PFA_CENTER, PFA_RIGHT, PFA_JUSTIFY };
            int sel = (int)SendDlgItemMessageW( dlg, IDC_PARA_ALIGN, CB_GETCURSEL, 0, 0 );
            int special = (int)SendDlgItemMessageW( dlg, IDC_PARA_SPECIAL, CB_GETCURSEL, 0, 0 );
            int line = (int)SendDlgItemMessageW( dlg, IDC_PARA_LINE, CB_GETCURSEL, 0, 0 );
            int left = get_cm( dlg, IDC_PARA_LEFT ), by = get_cm( dlg, IDC_PARA_BY ), first;

            first = special == 1 ? by : special == 2 ? -by : 0;
            pf.cbSize = sizeof(pf);
            pf.dwMask = PFM_ALIGNMENT | PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT | PFM_SPACEBEFORE |
                        PFM_SPACEAFTER | PFM_LINESPACING | PFM_PAGEBREAKBEFORE | PFM_KEEPNEXT;
            pf.wAlignment = aligns[max( sel, 0 )];
            pf.dxOffset = -first;
            pf.dxStartIndent = left + first;
            pf.dxRightIndent = get_cm( dlg, IDC_PARA_RIGHT );
            pf.dySpaceBefore = get_pt( dlg, IDC_PARA_BEFORE );
            pf.dySpaceAfter = get_pt( dlg, IDC_PARA_AFTER );
            switch (line)
            {
            case 1: pf.bLineSpacingRule = 5; pf.dyLineSpacing = 30; break;
            case 2: pf.bLineSpacingRule = 5; pf.dyLineSpacing = 40; break;
            case 3: pf.bLineSpacingRule = 3; pf.dyLineSpacing = get_pt( dlg, IDC_PARA_AT ); break;
            case 4: pf.bLineSpacingRule = 4; pf.dyLineSpacing = get_pt( dlg, IDC_PARA_AT ); break;
            case 5: pf.bLineSpacingRule = 5; pf.dyLineSpacing = max( 1, get_pt( dlg, IDC_PARA_AT ) ); break;
            default: pf.bLineSpacingRule = 0; pf.dyLineSpacing = 0; break;
            }
            pf.wEffects = (IsDlgButtonChecked( dlg, IDC_PARA_PAGEBREAK ) ? PFE_PAGEBREAKBEFORE : 0) |
                          (IsDlgButtonChecked( dlg, IDC_PARA_KEEPNEXT ) ? PFE_KEEPNEXT : 0);
            ed( EM_SETPARAFORMAT, 0, (LPARAM)&pf );
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        if (LOWORD( wparam ) == IDCANCEL)
        {
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void dialog_paragraph( HWND owner )
{
    if (DialogBoxW( instance, MAKEINTRESOURCEW( IDD_PARAGRAPH ), owner, paragraph_proc ) == IDOK)
    {
        set_modified( TRUE );
        ribbon_refresh();
        view_repaginate();
    }
    view_focus();
}

/**********************************************************************
 *          Font
 */

void dialog_font( HWND owner )
{
    CHOOSEFONTW cf = { sizeof(cf) };
    LOGFONTW lf = { 0 };
    CHARFORMAT2W fmt = { sizeof(fmt) };
    HDC hdc = GetDC( owner );

    ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&fmt );
    lstrcpynW( lf.lfFaceName, fmt.szFaceName, LF_FACESIZE );
    lf.lfHeight = -MulDiv( fmt.yHeight, GetDeviceCaps( hdc, LOGPIXELSY ), 1440 );
    lf.lfWeight = fmt.dwEffects & CFE_BOLD ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (fmt.dwEffects & CFE_ITALIC) != 0;
    lf.lfUnderline = (fmt.dwEffects & CFE_UNDERLINE) != 0;
    lf.lfStrikeOut = (fmt.dwEffects & CFE_STRIKEOUT) != 0;
    lf.lfCharSet = fmt.bCharSet;
    ReleaseDC( owner, hdc );
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
    cf.rgbColors = fmt.dwEffects & CFE_AUTOCOLOR ? RGB( 0, 0, 0 ) : fmt.crTextColor;
    if (ChooseFontW( &cf ))
    {
        fmt.cbSize = sizeof(fmt);
        fmt.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_COLOR | CFM_CHARSET;
        lstrcpynW( fmt.szFaceName, lf.lfFaceName, LF_FACESIZE );
        fmt.yHeight = cf.iPointSize * 2;
        fmt.dwEffects = (lf.lfWeight >= FW_SEMIBOLD ? CFE_BOLD : 0) | (lf.lfItalic ? CFE_ITALIC : 0) |
                        (lf.lfUnderline ? CFE_UNDERLINE : 0) | (lf.lfStrikeOut ? CFE_STRIKEOUT : 0);
        fmt.crTextColor = cf.rgbColors;
        fmt.bCharSet = lf.lfCharSet;
        ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&fmt );
        ribbon_refresh();
    }
    view_focus();
}

/**********************************************************************
 *          Link
 */

static INT_PTR CALLBACK link_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        CHARRANGE sel;
        ed( EM_EXGETSEL, 0, (LPARAM)&sel );
        if (sel.cpMax > sel.cpMin && sel.cpMax - sel.cpMin < 1024)
        {
            WCHAR text[1025];
            TEXTRANGEW range = { sel, text };
            ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
            SetDlgItemTextW( dlg, IDC_LINK_TEXT, text );
        }
        SetDlgItemTextW( dlg, IDC_LINK_URL, L"https://" );
        SetFocus( GetDlgItem( dlg, IDC_LINK_URL ) );
        SendDlgItemMessageW( dlg, IDC_LINK_URL, EM_SETSEL, 0, -1 );
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD( wparam ) == IDOK)
        {
            WCHAR text[1025], url[2048];
            struct buffer rtf = { 0 };

            GetDlgItemTextW( dlg, IDC_LINK_TEXT, text, ARRAY_SIZE(text) );
            GetDlgItemTextW( dlg, IDC_LINK_URL, url, ARRAY_SIZE(url) );
            if (!text[0]) lstrcpynW( text, url, ARRAY_SIZE(text) );
            /* riched20 keeps no fields: the text in the colour of links, its
             * address with the program (docx.c writes the link back) */
            buf_str( &rtf, "{\\rtf1\\ansi{\\colortbl;\\red5\\green99\\blue193;}{\\ul\\cf1 " );
            for (WCHAR *p = text; *p; p++)
            {
                if (*p == '\\' || *p == '{' || *p == '}') buf_printf( &rtf, "\\%c", *p );
                else if (*p < 128) buf_printf( &rtf, "%c", *p );
                else buf_printf( &rtf, "\\u%d?", (short)*p );
            }
            buf_str( &rtf, "}}" );
            stream_in( rtf.data, rtf.len, SF_RTF | SFF_SELECTION );
            buf_free( &rtf );
            {
                char utf8[2048 * 3];
                WideCharToMultiByte( CP_UTF8, 0, url, -1, utf8, sizeof(utf8), NULL, NULL );
                link_register( text, utf8 );
            }
            set_modified( TRUE );
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        if (LOWORD( wparam ) == IDCANCEL) EndDialog( dlg, IDCANCEL );
        break;
    }
    return FALSE;
}

void dialog_link( HWND owner )
{
    DialogBoxW( instance, MAKEINTRESOURCEW( IDD_LINK ), owner, link_proc );
    view_focus();
}

/**********************************************************************
 *          Date and time
 */

static INT_PTR CALLBACK datetime_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        static const DWORD date_flags[] = { DATE_SHORTDATE, DATE_LONGDATE };
        HWND list = GetDlgItem( dlg, IDC_DATETIME_LIST );
        SYSTEMTIME st;
        WCHAR date[128], time[64], both[200];

        GetLocalTime( &st );
        for (int i = 0; i < ARRAY_SIZE(date_flags); i++)
        {
            GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, date_flags[i], &st, NULL, date, ARRAY_SIZE(date), NULL );
            SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)date );
        }
        GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, 0, &st, L"dd.MM.yy", date, ARRAY_SIZE(date), NULL );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)date );
        GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, 0, &st, L"yyyy-MM-dd", date, ARRAY_SIZE(date), NULL );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)date );
        GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMMM yyyy", date, ARRAY_SIZE(date), NULL );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)date );
        GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, ARRAY_SIZE(date), NULL );
        GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, ARRAY_SIZE(time) );
        swprintf( both, ARRAY_SIZE(both), L"%s %s", date, time );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)both );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)time );
        GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, 0, &st, NULL, time, ARRAY_SIZE(time) );
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)time );
        SendMessageW( list, LB_SETCURSEL, 0, 0 );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD( wparam ) == IDOK || (LOWORD( wparam ) == IDC_DATETIME_LIST && HIWORD( wparam ) == LBN_DBLCLK))
        {
            WCHAR text[200];
            int sel = (int)SendDlgItemMessageW( dlg, IDC_DATETIME_LIST, LB_GETCURSEL, 0, 0 );

            if (sel >= 0 && SendDlgItemMessageW( dlg, IDC_DATETIME_LIST, LB_GETTEXTLEN, sel, 0 ) < ARRAY_SIZE(text))
            {
                SendDlgItemMessageW( dlg, IDC_DATETIME_LIST, LB_GETTEXT, sel, (LPARAM)text );
                insert_text( text );
            }
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        if (LOWORD( wparam ) == IDCANCEL) EndDialog( dlg, IDCANCEL );
        break;
    }
    return FALSE;
}

void dialog_datetime( HWND owner )
{
    DialogBoxW( instance, MAKEINTRESOURCEW( IDD_DATETIME ), owner, datetime_proc );
    view_focus();
}

/**********************************************************************
 *          Find and Replace
 */

static HWND find_dialog, find_owner;
static FINDREPLACEW find_data;
static WCHAR find_what[256], replace_with[256];
static UINT find_message;

static BOOL find_next( DWORD flags, BOOL quiet )
{
    FINDTEXTEXW ft;
    CHARRANGE sel;
    DWORD f = flags & (FR_MATCHCASE | FR_WHOLEWORD);
    LONG found;

    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (flags & FR_DOWN)
    {
        ft.chrg.cpMin = sel.cpMax;
        ft.chrg.cpMax = -1;
        f |= FR_DOWN;
    }
    else
    {
        ft.chrg.cpMin = sel.cpMin;
        ft.chrg.cpMax = 0;
    }
    ft.lpstrText = find_what;
    found = ed( EM_FINDTEXTEXW, f, (LPARAM)&ft );
    if (found < 0)
    {
        /* round from the other end, as Word asks to */
        ft.chrg.cpMin = flags & FR_DOWN ? 0 : ed( WM_GETTEXTLENGTH, 0, 0 );
        ft.chrg.cpMax = flags & FR_DOWN ? -1 : 0;
        found = ed( EM_FINDTEXTEXW, f, (LPARAM)&ft );
    }
    if (found < 0)
    {
        if (!quiet) MessageBoxW( find_dialog, string( IDS_NOT_FOUND ), string( IDS_APP_NAME ), MB_ICONINFORMATION );
        return FALSE;
    }
    ed( EM_EXSETSEL, 0, (LPARAM)&ft.chrgText );
    view_scroll_caret();
    return TRUE;
}

static BOOL selection_is_match( DWORD flags )
{
    CHARRANGE sel;
    WCHAR text[257];
    TEXTRANGEW range;

    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (sel.cpMax - sel.cpMin != lstrlenW( find_what ) || sel.cpMax == sel.cpMin) return FALSE;
    range.chrg = sel;
    range.lpstrText = text;
    ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
    return flags & FR_MATCHCASE ? !wcscmp( text, find_what ) : !wcsicmp( text, find_what );
}

static LRESULT CALLBACK find_owner_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == find_message)
    {
        FINDREPLACEW *fr = (FINDREPLACEW *)lparam;

        if (fr->Flags & FR_DIALOGTERM)
        {
            find_dialog = NULL;
            view_focus();
            return 0;
        }
        if (fr->Flags & FR_FINDNEXT) find_next( fr->Flags, FALSE );
        else if (fr->Flags & FR_REPLACE)
        {
            if (selection_is_match( fr->Flags )) ed( EM_REPLACESEL, TRUE, (LPARAM)replace_with );
            find_next( fr->Flags | FR_DOWN, FALSE );
        }
        else if (fr->Flags & FR_REPLACEALL)
        {
            FINDTEXTEXW ft = { { 0, -1 }, find_what };
            int count = 0;
            WCHAR text[128];

            view_freeze( TRUE );
            while (ed( EM_FINDTEXTEXW, FR_DOWN | (fr->Flags & (FR_MATCHCASE | FR_WHOLEWORD)), (LPARAM)&ft ) >= 0)
            {
                ed( EM_EXSETSEL, 0, (LPARAM)&ft.chrgText );
                ed( EM_REPLACESEL, TRUE, (LPARAM)replace_with );
                ft.chrg.cpMin = ft.chrgText.cpMin + lstrlenW( replace_with );
                count++;
            }
            view_freeze( FALSE );
            swprintf( text, ARRAY_SIZE(text), string( IDS_REPLACED ), count );
            MessageBoxW( find_dialog, text, string( IDS_APP_NAME ), MB_ICONINFORMATION );
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void dialog_find( HWND owner, BOOL replace )
{
    CHARRANGE sel;

    if (find_dialog) DestroyWindow( find_dialog );
    if (!find_owner)
    {
        WNDCLASSW cls = { 0, find_owner_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadFind" };
        RegisterClassW( &cls );
        find_owner = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, owner, NULL, instance, NULL );
        find_message = RegisterWindowMessageW( FINDMSGSTRINGW );
    }
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (sel.cpMax > sel.cpMin && sel.cpMax - sel.cpMin < ARRAY_SIZE(find_what))
    {
        TEXTRANGEW range = { sel, find_what };
        ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
        if (wcschr( find_what, '\r' )) find_what[0] = 0;
    }
    memset( &find_data, 0, sizeof(find_data) );
    find_data.lStructSize = sizeof(find_data);
    find_data.hwndOwner = find_owner;
    find_data.Flags = FR_DOWN;
    find_data.lpstrFindWhat = find_what;
    find_data.wFindWhatLen = ARRAY_SIZE(find_what);
    find_data.lpstrReplaceWith = replace_with;
    find_data.wReplaceWithLen = ARRAY_SIZE(replace_with);
    find_dialog = replace ? ReplaceTextW( &find_data ) : FindTextW( &find_data );
}

BOOL is_find_dialog_message( MSG *msg )
{
    return find_dialog && IsDialogMessageW( find_dialog, msg );
}

/**********************************************************************
 *          Zoom
 */

static INT_PTR CALLBACK zoom_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        int z = view_zoom();
        SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, z, FALSE );
        SendDlgItemMessageW( dlg, IDC_ZOOM_SPIN, UDM_SETRANGE32, 10, 500 );
        SendDlgItemMessageW( dlg, IDC_ZOOM_SPIN, UDM_SETPOS32, 0, z );
        CheckRadioButton( dlg, IDC_ZOOM_200, IDC_ZOOM_WHOLE, z == 200 ? IDC_ZOOM_200 : z == 75 ? IDC_ZOOM_75 : IDC_ZOOM_100 );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case IDC_ZOOM_200: SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, 200, FALSE ); break;
        case IDC_ZOOM_100: SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, 100, FALSE ); break;
        case IDC_ZOOM_75: SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, 75, FALSE ); break;
        case IDC_ZOOM_WIDTH: SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, view_fit_zoom( FALSE ), FALSE ); break;
        case IDC_ZOOM_WHOLE: SetDlgItemInt( dlg, IDC_ZOOM_PERCENT, view_fit_zoom( TRUE ), FALSE ); break;
        case IDOK:
        {
            BOOL ok;
            int z = GetDlgItemInt( dlg, IDC_ZOOM_PERCENT, &ok, FALSE );
            if (ok) view_set_zoom( z );
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void dialog_zoom( HWND owner )
{
    DialogBoxW( instance, MAKEINTRESOURCEW( IDD_ZOOM ), owner, zoom_proc );
    view_focus();
}

/**********************************************************************
 *          Word count, About
 */

void dialog_wordcount( HWND owner )
{
    WCHAR text[512];
    LONG chars = ed( WM_GETTEXTLENGTH, 0, 0 ), paras = ed( EM_GETLINECOUNT, 0, 0 );
    int words = word_count(), pars = 1;
    LONG length = chars;
    WCHAR *all = malloc( (length + 1) * sizeof(WCHAR) );
    LONG no_space = 0;

    (void)paras;
    if (all)
    {
        GETTEXTEX get = { (DWORD)(length + 1) * sizeof(WCHAR), GT_DEFAULT, 1200 };
        length = ed( EM_GETTEXTEX, (WPARAM)&get, (LPARAM)all );
        pars = 0;
        for (LONG i = 0; i < length; i++)
        {
            if (all[i] == '\r') pars++;
            else if (all[i] != ' ' && all[i] != '\t') no_space++;
        }
        if (length && all[length - 1] != '\r') pars++;
        chars = 0;
        for (LONG i = 0; i < length; i++) if (all[i] != '\r') chars++;
        free( all );
    }
    swprintf( text, ARRAY_SIZE(text), string( IDS_WORDCOUNT_TITLE ), view_page_count(), words, chars, no_space, pars );
    MessageBoxW( owner, text, string( IDS_CMD( IDM_WORDCOUNT ) ), MB_ICONINFORMATION );
}

void dialog_about( HWND owner )
{
    ShellAboutW( owner, string( IDS_APP_NAME ), string( IDS_ABOUT_TEXT ),
                 LoadIconW( instance, MAKEINTRESOURCEW( IDI_WORDPAD ) ) );
}
