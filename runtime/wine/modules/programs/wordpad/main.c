/*
 * Arctic WordPad
 *
 * A word processor in the way of Word of Windows 11: its title bar with the
 * search in the middle, its ribbon, its pages on the grey. It reads and
 * writes Word's documents (DOCX) as well as RTF and plain text; the text is
 * riched20's (view.c), the files are read and written in docx.c.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <shlobj.h>
#include <dwmapi.h>
#include "wordpad.h"

HINSTANCE instance;
HWND frame;
UINT dpi = 96;

static HWND ribbon, view, status, search_edit;
static struct surface title_surf;
static HICON small_icon;
static WCHAR file_path[MAX_PATH];
static int file_format = FMT_DOCX;
static BOOL modified;
static struct docx_keep *keep;          /* what the opened DOCX has that is not shown */
static int untitled_number = 1;
static BOOL quiet;                      /* /convert: no recent list, no messages */

static const WCHAR settings_key[] = L"Software\\Arctic\\WordPad";

#define TITLE_H     px(44)
#define STATUS_H    px(26)

/**********************************************************************
 *          Strings and settings
 */

const WCHAR *string( UINT id )
{
    static struct { UINT id; WCHAR *text; } cache[512];
    static int count;
    const WCHAR *res;
    int len;

    for (int i = 0; i < count; i++) if (cache[i].id == id) return cache[i].text;
    len = LoadStringW( instance, id, (WCHAR *)&res, 0 );
    if (count < ARRAY_SIZE(cache) && len > 0)
    {
        WCHAR *text = malloc( (len + 1) * sizeof(WCHAR) );
        if (!text) return L"";
        memcpy( text, res, len * sizeof(WCHAR) );
        text[len] = 0;
        cache[count].id = id;
        cache[count++].text = text;
        return text;
    }
    return L"";
}

DWORD setting( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

void set_setting( const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, settings_key, name, REG_DWORD, &value, sizeof(value) );
}

int recent_files( WCHAR paths[RECENT_MAX][MAX_PATH] )
{
    int n = 0;

    for (int i = 0; i < RECENT_MAX; i++)
    {
        WCHAR name[16];
        DWORD size = MAX_PATH * sizeof(WCHAR);

        swprintf( name, ARRAY_SIZE(name), L"Recent%d", i );
        if (!RegGetValueW( HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_SZ, NULL, paths[n], &size ) &&
            paths[n][0])
            n++;
    }
    return n;
}

void recent_add( const WCHAR *path )
{
    WCHAR paths[RECENT_MAX][MAX_PATH], list[RECENT_MAX][MAX_PATH];
    int n, m = 0;

    if (quiet) return;
    n = recent_files( paths );

    lstrcpynW( list[m++], path, MAX_PATH );
    for (int i = 0; i < n && m < RECENT_MAX; i++)
        if (wcsicmp( paths[i], path )) lstrcpynW( list[m++], paths[i], MAX_PATH );
    for (int i = 0; i < RECENT_MAX; i++)
    {
        WCHAR name[16];

        swprintf( name, ARRAY_SIZE(name), L"Recent%d", i );
        if (i < m)
            RegSetKeyValueW( HKEY_CURRENT_USER, settings_key, name, REG_SZ, list[i],
                             (lstrlenW( list[i] ) + 1) * sizeof(WCHAR) );
        else RegDeleteKeyValueW( HKEY_CURRENT_USER, settings_key, name );
    }
    SHAddToRecentDocs( SHARD_PATHW, path );
}

/**********************************************************************
 *          The document's name and state
 */

const WCHAR *document_name(void)
{
    static WCHAR name[MAX_PATH];

    if (file_path[0])
    {
        const WCHAR *base = wcsrchr( file_path, '\\' );
        WCHAR *dot;

        lstrcpynW( name, base ? base + 1 : file_path, ARRAY_SIZE(name) );
        if ((dot = wcsrchr( name, '.' )) && dot != name) *dot = 0;
    }
    else swprintf( name, ARRAY_SIZE(name), string( IDS_DEFAULT_NAME ), untitled_number );
    return name;
}

void frame_title_changed(void)
{
    WCHAR title[MAX_PATH + 64];

    swprintf( title, ARRAY_SIZE(title), L"%s - %s", document_name(), string( IDS_APP_NAME ) );
    SetWindowTextW( frame, title );
    {
        RECT r = { 0, 0, 0, TITLE_H };
        GetClientRect( frame, &r );
        r.bottom = TITLE_H;
        InvalidateRect( frame, &r, FALSE );
    }
}

void set_modified( BOOL value )
{
    if (modified == value) return;
    modified = value;
    frame_title_changed();
}

BOOL is_modified(void) { return modified; }

/**********************************************************************
 *          Files
 */

static BYTE *read_file( const WCHAR *path, size_t *len )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, 0, NULL );
    LARGE_INTEGER size;
    BYTE *data = NULL;
    DWORD done;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx( file, &size ) && size.QuadPart < 0x40000000 &&
        (data = malloc( size.QuadPart + 2 )))
    {
        if (ReadFile( file, data, (DWORD)size.QuadPart, &done, NULL ) && done == size.QuadPart)
        {
            data[done] = data[done + 1] = 0;
            *len = done;
        }
        else
        {
            free( data );
            data = NULL;
        }
    }
    CloseHandle( file );
    return data;
}

/* written beside it first, then put in its place: a failure leaves the old */
static BOOL write_file( const WCHAR *path, const void *data, size_t len )
{
    WCHAR temp[MAX_PATH + 16];
    HANDLE file;
    DWORD done;
    BOOL ok;

    swprintf( temp, ARRAY_SIZE(temp), L"%s.~wp", path );
    file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        /* a folder we cannot add to: straight into the file */
        file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if (file == INVALID_HANDLE_VALUE) return FALSE;
        ok = WriteFile( file, data, (DWORD)len, &done, NULL ) && done == len;
        CloseHandle( file );
        return ok;
    }
    ok = WriteFile( file, data, (DWORD)len, &done, NULL ) && done == len;
    ok = FlushFileBuffers( file ) && ok;
    CloseHandle( file );
    if (ok) ok = MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED );
    if (!ok) DeleteFileW( temp );
    return ok;
}

static int format_of_path( const WCHAR *path )
{
    const WCHAR *ext = wcsrchr( path, '.' );

    if (!ext) return FMT_DOCX;
    if (!wcsicmp( ext, L".rtf" )) return FMT_RTF;
    if (!wcsicmp( ext, L".txt" ) || !wcsicmp( ext, L".text" ) || !wcsicmp( ext, L".log" ) || !wcsicmp( ext, L".md" ))
        return FMT_TXT;
    if (!wcsicmp( ext, L".odt" )) return FMT_ODT;
    return FMT_DOCX;
}

static void error_box( UINT id, const WCHAR *path )
{
    WCHAR text[MAX_PATH + 256];

    if (quiet) return;
    swprintf( text, ARRAY_SIZE(text), string( id ), path );
    MessageBoxW( frame, text, string( IDS_APP_NAME ), MB_ICONWARNING );
}

/* text in some encoding: UTF-16 or UTF-8 with their marks, UTF-8 when it
 * reads as such, the ANSI code page otherwise */
static WCHAR *decode_text( const BYTE *data, size_t len )
{
    WCHAR *text;
    int n;

    if (len >= 2 && data[0] == 0xff && data[1] == 0xfe)
    {
        if (!(text = malloc( len + 2 ))) return NULL;
        memcpy( text, data + 2, len - 2 );
        text[(len - 2) / 2] = 0;
        return text;
    }
    if (len >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
    {
        data += 3;
        len -= 3;
    }
    if ((n = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, (int)len, NULL, 0 )) || !len)
    {
        if (!(text = malloc( (n + 1) * sizeof(WCHAR) ))) return NULL;
        MultiByteToWideChar( CP_UTF8, 0, (const char *)data, (int)len, text, n );
    }
    else
    {
        n = MultiByteToWideChar( CP_ACP, 0, (const char *)data, (int)len, NULL, 0 );
        if (!(text = malloc( (n + 1) * sizeof(WCHAR) ))) return NULL;
        MultiByteToWideChar( CP_ACP, 0, (const char *)data, (int)len, text, n );
    }
    text[n] = 0;
    return text;
}

static void load_text( const WCHAR *text )
{
    /* the text in Normal, as Word opens a .txt; its own lines stay */
    size_t len = lstrlenW( text );
    struct buffer rtf = { 0 };

    buf_str( &rtf, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Consolas;}}\\f0\\fs22 " );
    (void)len;
    buf_str( &rtf, "}" );
    view_reset_text();
    {
        SETTEXTEX set = { ST_DEFAULT, 1200 };
        WCHAR *conv = malloc( (len + 1) * sizeof(WCHAR) );
        size_t j = 0;

        if (conv)
        {
            /* \r\n and \n are paragraphs */
            for (size_t i = 0; i < len; i++)
            {
                if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') continue;
                conv[j++] = text[i] == '\n' ? '\r' : text[i];
            }
            conv[j] = 0;
            ed( EM_SETTEXTEX, (WPARAM)&set, (LPARAM)conv );
            free( conv );
        }
    }
    buf_free( &rtf );
}

void open_file( const WCHAR *path )
{
    WCHAR full[MAX_PATH];
    struct page_setup setup = { 11906, 16838, 1440, 1440, 1440, 1440 };
    struct docx_keep *new_keep = NULL;
    size_t len, rtf_len;
    BYTE *data;
    char *rtf = NULL;
    int format;
    BOOL ok = TRUE;

    if (!GetFullPathNameW( path, ARRAY_SIZE(full), full, NULL )) lstrcpynW( full, path, MAX_PATH );
    if (!(data = read_file( full, &len )))
    {
        error_box( IDS_CANT_OPEN, full );
        return;
    }
    format = format_of_path( full );
    if (len >= 4 && !memcmp( data, "PK\3\4", 4 ))
    {
        rtf = docx_to_rtf( data, len, &rtf_len, &setup, &new_keep );
        format = FMT_DOCX;
        if (!rtf && (rtf = odt_to_rtf( data, len, &rtf_len ))) format = FMT_ODT;
        ok = rtf != NULL;
    }
    else if (len >= 5 && !memcmp( data, "{\\rtf", 5 )) format = FMT_RTF;
    else if (len >= 8 && !memcmp( data, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8 ))
    {
        free( data );
        error_box( IDS_DOC_UNSUPPORTED, full );
        return;
    }
    else format = FMT_TXT;

    if (!ok)
    {
        free( data );
        error_box( IDS_CANT_OPEN, full );
        return;
    }

    SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_WAIT ) );
    view_freeze( TRUE );
    page = setup;
    if (format == FMT_TXT)
    {
        WCHAR *text = decode_text( data, len );
        load_text( text ? text : L"" );
        free( text );
    }
    else
    {
        view_reset_text();
        stream_in( rtf ? rtf : (const char *)data, rtf ? rtf_len : len, SF_RTF );
    }
    ed( EM_EMPTYUNDOBUFFER, 0, 0 );
    ed( EM_SETSEL, 0, 0 );
    view_set_page( &setup );
    view_freeze( FALSE );
    free( rtf );
    free( data );

    docx_keep_free( keep );
    keep = new_keep;
    lstrcpynW( file_path, full, MAX_PATH );
    file_format = format;
    modified = FALSE;
    recent_add( full );
    frame_title_changed();
    show_backstage( FALSE, 0 );
    view_repaginate();
    status_update();
    ribbon_refresh();
    view_focus();
}

BOOL save_file( const WCHAR *path, int format )
{
    char *data;
    size_t len;
    BOOL ok = FALSE;

    SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_WAIT ) );
    if (format == FMT_TXT)
    {
        WCHAR *text = (WCHAR *)stream_out( &len, SF_TEXT | SF_UNICODE );
        int n = WideCharToMultiByte( CP_UTF8, 0, text, (int)(len / sizeof(WCHAR)), NULL, 0, NULL, NULL );
        char *utf8 = malloc( n + 1 );

        if (utf8)
        {
            WideCharToMultiByte( CP_UTF8, 0, text, (int)(len / sizeof(WCHAR)), utf8, n, NULL, NULL );
            ok = write_file( path, utf8, n );
            free( utf8 );
        }
        free( text );
    }
    else
    {
        data = stream_out( &len, SF_RTF );
        if (format == FMT_RTF) ok = write_file( path, data, len );
        else
        {
            BYTE *docx;
            size_t docx_len;

            if (rtf_to_docx( data, len, &page, keep, &docx, &docx_len ))
            {
                ok = write_file( path, docx, docx_len );
                free( docx );
            }
        }
        free( data );
    }
    if (!ok)
    {
        error_box( IDS_CANT_SAVE, path );
        return FALSE;
    }
    lstrcpynW( file_path, path, MAX_PATH );
    file_format = format;
    modified = FALSE;
    recent_add( path );
    frame_title_changed();
    return TRUE;
}

static BOOL ask_file_name( WCHAR *path, int *format )
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[512];

    lstrcpynW( filter, string( IDS_FILTER_SAVE ), ARRAY_SIZE(filter) );
    for (WCHAR *p = filter; *p; p++) if (*p == '|') *p = 0;
    if (file_path[0])
    {
        WCHAR *dot;
        lstrcpynW( path, file_path, MAX_PATH );
        if ((dot = wcsrchr( path, '.' )) && wcsrchr( path, '\\' ) < dot) *dot = 0;
    }
    else lstrcpynW( path, document_name(), MAX_PATH );
    ofn.hwndOwner = frame;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = *format == FMT_RTF ? 2 : *format == FMT_TXT ? 3 : 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    ofn.lpstrDefExt = *format == FMT_RTF ? L"rtf" : *format == FMT_TXT ? L"txt" : L"docx";
    if (!GetSaveFileNameW( &ofn )) return FALSE;
    *format = ofn.nFilterIndex == 2 ? FMT_RTF : ofn.nFilterIndex == 3 ? FMT_TXT : FMT_DOCX;
    {
        /* the type chosen, whatever the name says */
        WCHAR *dot = wcsrchr( path, '.' );
        const WCHAR *want = *format == FMT_RTF ? L".rtf" : *format == FMT_TXT ? L".txt" : L".docx";
        if (!dot || wcsrchr( path, '\\' ) > dot || (wcsicmp( dot, want ) && *format != FMT_TXT))
        {
            if (lstrlenW( path ) + 6 < MAX_PATH)
            {
                if (dot && wcsrchr( path, '\\' ) < dot && (!wcsicmp( dot, L".docx" ) || !wcsicmp( dot, L".rtf" ) ||
                                                             !wcsicmp( dot, L".txt" ) || !wcsicmp( dot, L".odt" )))
                    *dot = 0;
                lstrcatW( path, want );
            }
        }
    }
    return TRUE;
}

BOOL save_document( BOOL ask_name )
{
    WCHAR path[MAX_PATH];
    int format = file_format == FMT_ODT ? FMT_DOCX : file_format;

    if (!ask_name && file_path[0] && file_format != FMT_ODT) return save_file( file_path, file_format );
    if (!ask_file_name( path, &format )) return FALSE;
    return save_file( path, format );
}

/* TRUE: go on (saved, or not wanted) */
BOOL ask_save_changes(void)
{
    TASKDIALOGCONFIG config = { sizeof(config) };
    TASKDIALOG_BUTTON buttons[] = { { IDYES, string( IDS_SAVE ) }, { IDNO, string( IDS_DONT_SAVE ) } };
    WCHAR text[MAX_PATH + 128];
    HRESULT (WINAPI *task_dialog)( const TASKDIALOGCONFIG *, int *, int *, BOOL * );
    int button = 0;

    if (!modified) return TRUE;
    swprintf( text, ARRAY_SIZE(text), string( IDS_SAVE_CHANGES ), document_name() );
    task_dialog = (void *)GetProcAddress( GetModuleHandleW( L"comctl32.dll" ), "TaskDialogIndirect" );
    if (task_dialog)
    {
        config.hwndParent = frame;
        config.hInstance = instance;
        config.pszWindowTitle = string( IDS_APP_NAME );
        config.pszMainInstruction = text;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pButtons = buttons;
        config.cButtons = ARRAY_SIZE(buttons);
        config.nDefaultButton = IDYES;
        if (FAILED(task_dialog( &config, &button, NULL, NULL ))) button = 0;
    }
    if (!button) button = MessageBoxW( frame, text, string( IDS_APP_NAME ), MB_YESNOCANCEL | MB_ICONWARNING );
    if (button == IDCANCEL) return FALSE;
    if (button == IDYES) return save_document( FALSE );
    return TRUE;
}

void new_document(void)
{
    static const struct page_setup a4 = { 11906, 16838, 1440, 1440, 1440, 1440 };

    if (file_path[0] || modified) untitled_number++;
    file_path[0] = 0;
    file_format = FMT_DOCX;
    docx_keep_free( keep );
    keep = NULL;
    page = a4;
    view_reset_text();
    view_set_page( &a4 );
    modified = FALSE;
    frame_title_changed();
    show_backstage( FALSE, 0 );
    view_focus();
}

static void open_dialog(void)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[512], path[MAX_PATH] = L"";

    lstrcpynW( filter, string( IDS_FILTER_OPEN ), ARRAY_SIZE(filter) );
    for (WCHAR *p = filter; *p; p++) if (*p == '|') *p = 0;
    ofn.hwndOwner = frame;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW( &ofn ) && ask_save_changes()) open_file( path );
}

const WCHAR *current_path(void) { return file_path; }

/**********************************************************************
 *          Commands
 */

static CHARFORMAT2W painter_cf;
static PARAFORMAT2 painter_pf;
static BOOL painter_on;

static void set_char( DWORD mask, DWORD effects )
{
    CHARFORMAT2W cf = { sizeof(cf) };

    cf.dwMask = mask;
    cf.dwEffects = effects;
    ed( EM_SETCHARFORMAT, SCF_SELECTION | SCF_WORD, (LPARAM)&cf );
    ribbon_refresh();
}

static void toggle_char( DWORD mask, DWORD effect )
{
    CHARFORMAT2W cf = { sizeof(cf) };

    ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    set_char( mask, (cf.dwMask & mask) && (cf.dwEffects & effect) ? 0 : effect );
}

static void set_para( DWORD mask, PARAFORMAT2 *pf )
{
    pf->cbSize = sizeof(*pf);
    pf->dwMask = mask;
    ed( EM_SETPARAFORMAT, 0, (LPARAM)pf );
    ribbon_refresh();
    view_repaginate();
}

static void set_alignment( WORD align )
{
    PARAFORMAT2 pf = { sizeof(pf) };
    pf.wAlignment = align;
    set_para( PFM_ALIGNMENT, &pf );
}

static void set_numbering( WORD numbering, WORD style, WORD start )
{
    PARAFORMAT2 pf = { sizeof(pf) };

    ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
    if (numbering && pf.wNumbering == numbering && (numbering == PFN_BULLET || pf.wNumberingStyle == style) &&
        !(start & 0x8000))
        numbering = 0;   /* again: off */
    pf.wNumbering = numbering;
    pf.wNumberingStyle = style;
    pf.wNumberingStart = 1;
    pf.wNumberingTab = 360;
    if (numbering && pf.dxOffset == 0)
    {
        pf.dxStartIndent += 720;
        pf.dxOffset = 360;
    }
    else if (!numbering && pf.dxOffset == 360 && pf.dxStartIndent >= 720)
    {
        pf.dxStartIndent -= 720;
        pf.dxOffset = 0;
    }
    pf.dxStartIndent -= pf.dxOffset;   /* PFM_STARTINDENT sets the first line; left = start + offset */
    set_para( PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGSTART | PFM_NUMBERINGTAB | PFM_STARTINDENT | PFM_OFFSET, &pf );
}

static void set_line_spacing( int twentieths )
{
    PARAFORMAT2 pf = { sizeof(pf) };

    pf.bLineSpacingRule = twentieths == 20 ? 0 : 5;
    pf.dyLineSpacing = twentieths;
    set_para( PFM_LINESPACING, &pf );
}

static void indent( int dir )
{
    PARAFORMAT2 pf = { sizeof(pf) };

    ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
    pf.dxStartIndent = max( 0, (pf.dxStartIndent + dir * 720) / 720 * 720 );
    set_para( PFM_STARTINDENT, &pf );
}

static void font_size_step( BOOL grow )
{
    static const int sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    CHARFORMAT2W cf = { sizeof(cf) };
    int pt, next;

    ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    pt = (cf.dwMask & CFM_SIZE) ? cf.yHeight / 20 : 11;
    next = grow ? pt + 10 : max( 1, pt - 10 );
    for (int i = 0; i < ARRAY_SIZE(sizes); i++)
    {
        if (grow && sizes[i] > pt) { next = sizes[i]; break; }
        if (!grow && sizes[ARRAY_SIZE(sizes) - 1 - i] < pt) { next = sizes[ARRAY_SIZE(sizes) - 1 - i]; break; }
    }
    if (grow && pt >= 72) next = (pt / 10 + 1) * 10;
    cf.dwMask = CFM_SIZE;
    cf.yHeight = next * 20;
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    ribbon_refresh();
}

static void set_font_size( int half_points )
{
    CHARFORMAT2W cf = { sizeof(cf) };

    cf.dwMask = CFM_SIZE;
    cf.yHeight = half_points * 10;
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    ribbon_refresh();
}

static void set_face( const WCHAR *face )
{
    CHARFORMAT2W cf = { sizeof(cf) };

    cf.dwMask = CFM_FACE | CFM_CHARSET;
    cf.bCharSet = DEFAULT_CHARSET;
    lstrcpynW( cf.szFaceName, face, LF_FACESIZE );
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    ribbon_refresh();
}

static void clear_formatting(void)
{
    CHARFORMAT2W cf = { sizeof(cf) };

    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_COLOR | CFM_BACKCOLOR | CFM_SUBSCRIPT |
                CFM_SUPERSCRIPT | CFM_FACE | CFM_SIZE | CFM_SMALLCAPS | CFM_ALLCAPS | CFM_HIDDEN;
    cf.dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
    cf.yHeight = 220;
    lstrcpyW( cf.szFaceName, body_face );
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    apply_style( 0 );
    ribbon_refresh();
}

static void apply_color( int cmd, COLORREF color, BOOL automatic )
{
    CHARFORMAT2W cf = { sizeof(cf) };

    if (cmd == IDM_FONTCOLOR)
    {
        cf.dwMask = CFM_COLOR;
        cf.dwEffects = automatic ? CFE_AUTOCOLOR : 0;
        cf.crTextColor = color;
    }
    else
    {
        cf.dwMask = CFM_BACKCOLOR;
        cf.dwEffects = automatic ? CFE_AUTOBACKCOLOR : 0;
        cf.crBackColor = color;
    }
    ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
    if (!automatic) ribbon_set_color( cmd, color );
}

/* the format painter: the format of the selection, given to the next one */
void format_painter_mouse_up(void)
{
    CHARRANGE sel;

    if (!painter_on) return;
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (sel.cpMin != sel.cpMax)
    {
        ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&painter_cf );
        ed( EM_SETPARAFORMAT, 0, (LPARAM)&painter_pf );
    }
    painter_on = FALSE;
    ribbon_refresh();
}

static void sort_paragraphs(void)
{
    CHARRANGE sel;
    WCHAR *text, **lines;
    int count = 0, len;
    TEXTRANGEW range;

    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (sel.cpMin == sel.cpMax) return;
    len = sel.cpMax - sel.cpMin;
    if (!(text = malloc( (len + 2) * sizeof(WCHAR) ))) return;
    range.chrg = sel;
    range.lpstrText = text;
    len = ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
    if (len && text[len - 1] == '\r') len--;
    text[len] = 0;
    if (!(lines = malloc( (len + 1) * sizeof(WCHAR *) ))) { free( text ); return; }
    lines[count++] = text;
    for (int i = 0; i < len; i++)
        if (text[i] == '\r')
        {
            text[i] = 0;
            lines[count++] = text + i + 1;
        }
    if (count > 1)
    {
        struct buffer out = { 0 };
        for (int i = 1; i < count; i++)
            for (int j = i; j > 0 && CompareStringEx( LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, lines[j], -1, lines[j - 1], -1, NULL, NULL, 0 ) == CSTR_LESS_THAN; j--)
            {
                WCHAR *t = lines[j]; lines[j] = lines[j - 1]; lines[j - 1] = t;
            }
        for (int i = 0; i < count; i++)
        {
            buf_add( &out, lines[i], lstrlenW( lines[i] ) * sizeof(WCHAR) );
            if (i < count - 1) buf_add( &out, L"\r", sizeof(WCHAR) );
        }
        buf_add( &out, L"\0", sizeof(WCHAR) );
        sel.cpMax = sel.cpMin + len;
        ed( EM_EXSETSEL, 0, (LPARAM)&sel );
        ed( EM_REPLACESEL, TRUE, (LPARAM)out.data );
        buf_free( &out );
    }
    free( lines );
    free( text );
}

static void find_next_search(void)
{
    WCHAR text[256];
    FINDTEXTEXW ft;
    CHARRANGE sel;
    LONG found;

    GetWindowTextW( search_edit, text, ARRAY_SIZE(text) );
    if (!text[0]) return;
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    ft.chrg.cpMin = sel.cpMax;
    ft.chrg.cpMax = -1;
    ft.lpstrText = text;
    found = ed( EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft );
    if (found < 0)
    {
        ft.chrg.cpMin = 0;
        found = ed( EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft );
    }
    if (found < 0)
    {
        MessageBeep( MB_ICONASTERISK );
        return;
    }
    ed( EM_EXSETSEL, 0, (LPARAM)&ft.chrgText );
    view_scroll_caret();
}

static void set_page_size( int w, int h )
{
    struct page_setup s = page;
    BOOL landscape = s.width > s.height;

    s.width = landscape ? max( w, h ) : min( w, h );
    s.height = landscape ? min( w, h ) : max( w, h );
    view_set_page( &s );
    set_modified( TRUE );
}

static void set_margins( int l, int t, int r, int b )
{
    struct page_setup s = page;

    s.left = l; s.top = t; s.right = r; s.bottom = b;
    view_set_page( &s );
    set_modified( TRUE );
}

BOOL command_enabled( int id )
{
    CHARRANGE sel;

    switch (id)
    {
    case IDM_UNDO: return ed( EM_CANUNDO, 0, 0 ) != 0;
    case IDM_REDO: return ed( EM_CANREDO, 0, 0 ) != 0;
    case IDM_CUT:
    case IDM_COPY:
        ed( EM_EXGETSEL, 0, (LPARAM)&sel );
        return sel.cpMin != sel.cpMax;
    case IDM_SORT:
        ed( EM_EXGETSEL, 0, (LPARAM)&sel );
        return sel.cpMin != sel.cpMax;
    case IDM_PASTE:
        return IsClipboardFormatAvailable( CF_UNICODETEXT ) || IsClipboardFormatAvailable( RegisterClipboardFormatW( L"Rich Text Format" ) ) ||
               IsClipboardFormatAvailable( CF_BITMAP ) || IsClipboardFormatAvailable( CF_DIB ) || IsClipboardFormatAvailable( CF_ENHMETAFILE );
    case IDM_INDENT_LEFT:
    case IDM_INDENT_RIGHT:
    case IDM_SPACE_BEFORE:
    case IDM_SPACE_AFTER:
        return TRUE;
    case IDM_ONEPAGE:
    case IDM_MULTIPAGE:
    case IDM_PAGEWIDTH:
    case IDM_MARGINS:
    case IDM_ORIENTATION:
    case IDM_SIZE:
        return !web_layout;
    }
    return TRUE;
}

BOOL command_checked( int id )
{
    switch (id)
    {
    case IDM_FORMATPAINTER: return painter_on;
    case IDM_VIEW_PRINT: return !web_layout;
    case IDM_VIEW_WEB: return web_layout;
    case IDM_PORTRAIT: return page.width <= page.height;
    case IDM_LANDSCAPE: return page.width > page.height;
    }
    return FALSE;
}

void command( int id )
{
    command_arg( id, 0 );
}

static const RECT *anchor_rect( LPARAM arg, RECT *store )
{
    if (arg) return (const RECT *)arg;
    GetCursorPos( (POINT *)store );
    store->right = store->left;
    store->bottom = store->top;
    return store;
}

void command_arg( int id, LPARAM arg )
{
    RECT store;
    const RECT *anchor;

    if (id >= IDM_STYLE_FIRST && id < IDM_STYLE_FIRST + STYLE_COUNT)
    {
        apply_style( id - IDM_STYLE_FIRST );
        ribbon_refresh();
        view_repaginate();
        return;
    }
    if (id >= IDM_RECENT_FIRST && id < IDM_RECENT_FIRST + RECENT_MAX)
    {
        WCHAR paths[RECENT_MAX][MAX_PATH];
        int n = recent_files( paths );
        if (id - IDM_RECENT_FIRST < n && ask_save_changes()) open_file( paths[id - IDM_RECENT_FIRST] );
        return;
    }

    switch (id)
    {
    case IDM_FILE: show_backstage( TRUE, 0 ); break;
    case IDM_NEW: if (ask_save_changes()) new_document(); break;
    case IDM_OPEN: open_dialog(); break;
    case IDM_SAVE: save_document( FALSE ); break;
    case IDM_SAVEAS: save_document( TRUE ); break;
    case IDM_PRINT: print_document( frame, TRUE ); break;
    case IDM_QUICKPRINT: print_document( frame, FALSE ); break;
    case IDM_PAGESETUP: page_setup_dialog( frame ); break;
    case IDM_CLOSE: if (ask_save_changes()) new_document(); break;
    case IDM_EXIT: PostMessageW( frame, WM_CLOSE, 0, 0 ); break;
    case IDM_ABOUT: dialog_about( frame ); break;

    case IDM_UNDO: ed( EM_UNDO, 0, 0 ); view_scroll_caret(); break;
    case IDM_REDO: ed( EM_REDO, 0, 0 ); view_scroll_caret(); break;
    case IDM_CUT: ed( WM_CUT, 0, 0 ); break;
    case IDM_COPY: ed( WM_COPY, 0, 0 ); break;
    case IDM_PASTE: ed( WM_PASTE, 0, 0 ); view_scroll_caret(); break;
    case IDM_PASTE_TEXT: ed( EM_PASTESPECIAL, CF_UNICODETEXT, 0 ); view_scroll_caret(); break;
    case IDM_PASTE_MENU:
    {
        static const int ids[] = { IDM_PASTE, IDM_PASTE_TEXT };
        static const char *const icons[] = { "paste", "document" };
        int pick;
        anchor = anchor_rect( arg, &store );
        if ((pick = menu_popup( frame, anchor, ids, icons, 2, -1 ))) command( pick );
        break;
    }
    case IDM_SELECTALL: ed( EM_SETSEL, 0, -1 ); break;
    case IDM_SELECT_MENU:
    {
        static const int ids[] = { IDM_SELECTALL };
        static const char *const icons[] = { "select" };
        int pick;
        anchor = anchor_rect( arg, &store );
        if ((pick = menu_popup( frame, anchor, ids, icons, 1, -1 ))) command( pick );
        break;
    }
    case IDM_FORMATPAINTER:
        if (painter_on) painter_on = FALSE;
        else
        {
            painter_cf.cbSize = sizeof(painter_cf);
            ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&painter_cf );
            painter_pf.cbSize = sizeof(painter_pf);
            ed( EM_GETPARAFORMAT, 0, (LPARAM)&painter_pf );
            painter_on = TRUE;
        }
        ribbon_refresh();
        view_focus();
        break;
    case IDM_FIND: dialog_find( frame, FALSE ); break;
    case IDM_REPLACE: dialog_find( frame, TRUE ); break;
    case IDM_SEARCHBOX: find_next_search(); break;

    case IDM_FONTFACE:
    {
        CHARFORMAT2W cf = { sizeof(cf) };
        WCHAR face[LF_FACESIZE];
        ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
        lstrcpynW( face, cf.szFaceName, LF_FACESIZE );
        anchor = anchor_rect( arg, &store );
        if (pick_font( frame, anchor, face, LF_FACESIZE )) set_face( face );
        view_focus();
        break;
    }
    case IDM_FONTSIZE:
        if (arg > 0 && arg < 4000) set_font_size( (int)arg );
        else
        {
            /* the arrow of the size: Word's list of sizes */
            static const int sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
            HMENU menu = CreatePopupMenu();
            CHARFORMAT2W cf = { sizeof(cf) };
            TPMPARAMS params = { sizeof(params) };
            int pick;

            ed( EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
            anchor = anchor_rect( arg, &store );
            for (int i = 0; i < ARRAY_SIZE(sizes); i++)
            {
                WCHAR text[8];
                swprintf( text, ARRAY_SIZE(text), L"%d", sizes[i] );
                AppendMenuW( menu, MF_STRING | ((cf.dwMask & CFM_SIZE) && cf.yHeight == sizes[i] * 20 ? MF_CHECKED : 0),
                             i + 1, text );
            }
            params.rcExclude = *anchor;
            pick = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, anchor->left,
                                     anchor->bottom, frame, &params );
            DestroyMenu( menu );
            if (pick) set_font_size( sizes[pick - 1] * 2 );
            view_focus();
        }
        break;
    case IDM_CASE_SENTENCE:
    case IDM_CASE_LOWER:
    case IDM_CASE_UPPER:
    case IDM_CASE_TITLE:
    case IDM_CASE_TOGGLE:
        change_case( id );
        break;
    case IDM_LS_100: set_line_spacing( 20 ); break;
    case IDM_LS_115: set_line_spacing( 23 ); break;
    case IDM_LS_150: set_line_spacing( 30 ); break;
    case IDM_LS_200: set_line_spacing( 40 ); break;
    case IDM_LS_250: set_line_spacing( 50 ); break;
    case IDM_LS_300: set_line_spacing( 60 ); break;
    case IDM_FINDNEXT:
        if (GetWindowTextLengthW( search_edit )) find_next_search();
        else dialog_find( frame, FALSE );
        break;
    case IDM_GROW: font_size_step( TRUE ); break;
    case IDM_SHRINK: font_size_step( FALSE ); break;
    case IDM_CASE:
    {
        static const int ids[] = { IDM_CASE_SENTENCE, IDM_CASE_LOWER, IDM_CASE_UPPER, IDM_CASE_TITLE, IDM_CASE_TOGGLE };
        int pick;
        anchor = anchor_rect( arg, &store );
        if ((pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 ))) change_case( pick );
        break;
    }
    case IDM_CLEARFORMAT: clear_formatting(); break;
    case IDM_BOLD: toggle_char( CFM_BOLD, CFE_BOLD ); break;
    case IDM_ITALIC: toggle_char( CFM_ITALIC, CFE_ITALIC ); break;
    case IDM_UNDERLINE: toggle_char( CFM_UNDERLINE, CFE_UNDERLINE ); break;
    case IDM_UNDERLINE_MENU:
    {
        static const int ids[] = { IDM_UL_SINGLE, IDM_UL_DOUBLE, IDM_UL_DOTTED, IDM_UL_WORD };
        CHARFORMAT2W cf = { sizeof(cf) };
        int pick;
        anchor = anchor_rect( arg, &store );
        if ((pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 )))
        {
            cf.dwMask = CFM_UNDERLINETYPE | CFM_UNDERLINE;
            cf.dwEffects = CFE_UNDERLINE;
            cf.bUnderlineType = pick == IDM_UL_DOUBLE ? CFU_UNDERLINEDOUBLE : pick == IDM_UL_DOTTED ? CFU_UNDERLINEDOTTED :
                                pick == IDM_UL_WORD ? CFU_UNDERLINEWORD : CFU_UNDERLINE;
            ed( EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
            ribbon_refresh();
        }
        break;
    }
    case IDM_STRIKE: toggle_char( CFM_STRIKEOUT, CFE_STRIKEOUT ); break;
    case IDM_SUBSCRIPT: toggle_char( CFM_SUBSCRIPT, CFE_SUBSCRIPT ); break;
    case IDM_SUPERSCRIPT: toggle_char( CFM_SUPERSCRIPT, CFE_SUPERSCRIPT ); break;
    case IDM_TEXTEFFECTS:
    {
        static const int ids[] = { IDM_FX_SMALLCAPS, IDM_FX_ALLCAPS, IDM_FX_HIDDEN, 0, IDM_FONTDIALOG };
        int pick;
        anchor = anchor_rect( arg, &store );
        pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 );
        if (pick == IDM_FX_SMALLCAPS) toggle_char( CFM_SMALLCAPS, CFE_SMALLCAPS );
        else if (pick == IDM_FX_ALLCAPS) toggle_char( CFM_ALLCAPS, CFE_ALLCAPS );
        else if (pick == IDM_FX_HIDDEN) toggle_char( CFM_HIDDEN, CFE_HIDDEN );
        else if (pick) command( pick );
        break;
    }
    case IDM_FONTCOLOR:
    case IDM_HIGHLIGHT:
    case IDM_SHADING:
        if (id == IDM_SHADING && arg)
        {
            BOOL automatic;
            COLORREF c = pick_color( frame, (const RECT *)arg, ribbon_color( id ), TRUE, &automatic );
            if (c != CLR_INVALID) apply_color( IDM_HIGHLIGHT, c, automatic );
            break;
        }
        apply_color( id == IDM_FONTCOLOR ? IDM_FONTCOLOR : IDM_HIGHLIGHT, ribbon_color( id ), FALSE );
        break;
    case IDM_FONTCOLOR_MENU:
    case IDM_HIGHLIGHT_MENU:
    {
        BOOL automatic;
        int target = id == IDM_FONTCOLOR_MENU ? IDM_FONTCOLOR : IDM_HIGHLIGHT;
        COLORREF c;
        anchor = anchor_rect( arg, &store );
        c = pick_color( frame, anchor, ribbon_color( target ), target == IDM_HIGHLIGHT, &automatic );
        if (c != CLR_INVALID) apply_color( target, c, automatic );
        break;
    }
    case IDM_FONTDIALOG: dialog_font( frame ); break;

    case IDM_BULLETS: set_numbering( PFN_BULLET, 0, 0 ); break;
    case IDM_NUMBERING: set_numbering( PFN_ARABIC, PFNS_PERIOD, 0 ); break;
    case IDM_BULLETS_MENU:
    {
        static const int ids[] = { IDM_BULLET_DOT, IDM_BULLET_NONE };
        int pick;
        anchor = anchor_rect( arg, &store );
        pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 );
        if (pick == IDM_BULLET_DOT) set_numbering( PFN_BULLET, 0, 0x8000 );
        else if (pick == IDM_BULLET_NONE) set_numbering( 0, 0, 0 );
        break;
    }
    case IDM_NUMBERING_MENU:
    case IDM_MULTILEVEL:
    {
        static const int ids[] = { IDM_NUM_ARABIC, IDM_NUM_PAREN, IDM_NUM_UCLETTER, IDM_NUM_LCLETTER, IDM_NUM_UCROMAN,
                                   IDM_NUM_LCROMAN, 0, IDM_NUM_NONE };
        int pick;
        anchor = anchor_rect( arg, &store );
        switch ((pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 )))
        {
        case IDM_NUM_ARABIC: set_numbering( PFN_ARABIC, PFNS_PERIOD, 0x8000 ); break;
        case IDM_NUM_PAREN: set_numbering( PFN_ARABIC, PFNS_PAREN, 0x8000 ); break;
        case IDM_NUM_UCLETTER: set_numbering( PFN_UCLETTER, PFNS_PERIOD, 0x8000 ); break;
        case IDM_NUM_LCLETTER: set_numbering( PFN_LCLETTER, PFNS_PAREN, 0x8000 ); break;
        case IDM_NUM_UCROMAN: set_numbering( PFN_UCROMAN, PFNS_PERIOD, 0x8000 ); break;
        case IDM_NUM_LCROMAN: set_numbering( PFN_LCROMAN, PFNS_PERIOD, 0x8000 ); break;
        case IDM_NUM_NONE: set_numbering( 0, 0, 0 ); break;
        }
        break;
    }
    case IDM_OUTDENT: indent( -1 ); break;
    case IDM_INDENT: indent( 1 ); break;
    case IDM_SORT: sort_paragraphs(); break;
    case IDM_ALIGN_LEFT: set_alignment( PFA_LEFT ); break;
    case IDM_ALIGN_CENTER: set_alignment( PFA_CENTER ); break;
    case IDM_ALIGN_RIGHT: set_alignment( PFA_RIGHT ); break;
    case IDM_ALIGN_JUSTIFY: set_alignment( PFA_JUSTIFY ); break;
    case IDM_LINESPACING:
    {
        static const int ids[] = { IDM_LS_100, IDM_LS_115, IDM_LS_150, IDM_LS_200, IDM_LS_250, IDM_LS_300, 0,
                                   IDM_PARADIALOG, 0, IDM_LS_BEFORE, IDM_LS_AFTER };
        PARAFORMAT2 pf = { sizeof(pf) };
        int pick, current = -1;

        ed( EM_GETPARAFORMAT, 0, (LPARAM)&pf );
        if (pf.bLineSpacingRule == 0) current = IDM_LS_100;
        else if (pf.bLineSpacingRule == 5)
            current = pf.dyLineSpacing <= 20 ? IDM_LS_100 : pf.dyLineSpacing <= 23 ? IDM_LS_115 : pf.dyLineSpacing <= 30 ? IDM_LS_150 :
                      pf.dyLineSpacing <= 40 ? IDM_LS_200 : pf.dyLineSpacing <= 50 ? IDM_LS_250 : IDM_LS_300;
        anchor = anchor_rect( arg, &store );
        switch ((pick = menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), current )))
        {
        case IDM_LS_100: set_line_spacing( 20 ); break;
        case IDM_LS_115: set_line_spacing( 23 ); break;
        case IDM_LS_150: set_line_spacing( 30 ); break;
        case IDM_LS_200: set_line_spacing( 40 ); break;
        case IDM_LS_250: set_line_spacing( 50 ); break;
        case IDM_LS_300: set_line_spacing( 60 ); break;
        case IDM_LS_BEFORE: pf.dySpaceBefore = pf.dySpaceBefore ? 0 : 240; set_para( PFM_SPACEBEFORE, &pf ); break;
        case IDM_LS_AFTER: pf.dySpaceAfter = pf.dySpaceAfter ? 0 : 160; set_para( PFM_SPACEAFTER, &pf ); break;
        case IDM_PARADIALOG: command( pick ); break;
        }
        break;
    }
    case IDM_PARADIALOG: dialog_paragraph( frame ); break;

    case IDM_PAGEBREAK: insert_page_break(); break;
    case IDM_TABLE:
    {
        int size;
        anchor = anchor_rect( arg, &store );
        if ((size = pick_table( frame, anchor ))) insert_table( LOWORD( size ), HIWORD( size ) );
        view_focus();
        break;
    }
    case IDM_PICTURE: insert_picture( frame ); break;
    case IDM_LINK: dialog_link( frame ); break;
    case IDM_DATETIME: dialog_datetime( frame ); break;
    case IDM_SYMBOL:
    {
        WCHAR c, text[2] = { 0 };
        anchor = anchor_rect( arg, &store );
        if ((c = pick_symbol( frame, anchor )))
        {
            text[0] = c;
            insert_text( text );
        }
        view_focus();
        break;
    }

    case IDM_MARGINS:
    {
        static const int ids[] = { IDM_MARGIN_NORMAL, IDM_MARGIN_NARROW, IDM_MARGIN_MODERATE, IDM_MARGIN_WIDE, 0, IDM_PAGESETUP };
        static const char *const icons[] = { "margins", "margins", "margins", "margins", NULL, NULL };
        anchor = anchor_rect( arg, &store );
        switch (menu_popup( frame, anchor, ids, icons, ARRAY_SIZE(ids), -1 ))
        {
        case IDM_MARGIN_NORMAL: set_margins( 1701, 1134, 850, 1134 ); break;     /* 3 / 2 / 1.5 / 2 cm, as Word in Ukraine */
        case IDM_MARGIN_NARROW: set_margins( 720, 720, 720, 720 ); break;
        case IDM_MARGIN_MODERATE: set_margins( 1080, 1440, 1080, 1440 ); break;
        case IDM_MARGIN_WIDE: set_margins( 2880, 1440, 2880, 1440 ); break;
        case IDM_PAGESETUP: page_setup_dialog( frame ); break;
        }
        break;
    }
    case IDM_ORIENTATION:
    {
        static const int ids[] = { IDM_PORTRAIT, IDM_LANDSCAPE };
        static const char *const icons[] = { "printlayout", "orientation" };
        int pick;
        anchor = anchor_rect( arg, &store );
        pick = menu_popup( frame, anchor, ids, icons, ARRAY_SIZE(ids), -1 );
        if ((pick == IDM_PORTRAIT && page.width > page.height) || (pick == IDM_LANDSCAPE && page.width < page.height))
        {
            struct page_setup s = page;
            s.width = page.height; s.height = page.width;
            s.left = page.top; s.top = page.right; s.right = page.bottom; s.bottom = page.left;
            view_set_page( &s );
            set_modified( TRUE );
        }
        break;
    }
    case IDM_SIZE:
    {
        static const int ids[] = { IDM_SIZE_A4, IDM_SIZE_A5, IDM_SIZE_A3, IDM_SIZE_LETTER, IDM_SIZE_LEGAL };
        anchor = anchor_rect( arg, &store );
        switch (menu_popup( frame, anchor, ids, NULL, ARRAY_SIZE(ids), -1 ))
        {
        case IDM_SIZE_A4: set_page_size( 11906, 16838 ); break;
        case IDM_SIZE_A5: set_page_size( 8391, 11906 ); break;
        case IDM_SIZE_A3: set_page_size( 16838, 23811 ); break;
        case IDM_SIZE_LETTER: set_page_size( 12240, 15840 ); break;
        case IDM_SIZE_LEGAL: set_page_size( 12240, 20160 ); break;
        }
        break;
    }

    case IDM_VIEW_PRINT: view_set_web( FALSE ); set_setting( L"WebLayout", 0 ); break;
    case IDM_VIEW_WEB: view_set_web( TRUE ); set_setting( L"WebLayout", 1 ); break;
    case IDM_ZOOM: dialog_zoom( frame ); break;
    case IDM_ZOOM_100: view_set_zoom( 100 ); break;
    case IDM_ZOOM_IN: view_set_zoom( view_zoom() + 10 ); break;
    case IDM_ZOOM_OUT: view_set_zoom( view_zoom() - 10 ); break;
    case IDM_ONEPAGE: view_set_zoom( view_fit_zoom( TRUE ) ); break;
    case IDM_MULTIPAGE:
    {
        RECT client;
        GetClientRect( view, &client );
        view_set_zoom( MulDiv( (client.right - px( 48 )) / 2, 100 * 1440, dpi * page.width ) );
        break;
    }
    case IDM_PAGEWIDTH: view_set_zoom( view_fit_zoom( FALSE ) ); break;
    case IDM_WORDCOUNT: dialog_wordcount( frame ); break;
    }
}

/**********************************************************************
 *          The title bar
 */

enum { T_NONE, T_SEARCH, T_MIN, T_MAX, T_CLOSE, T_ICON };
static int title_hot, title_down;
static RECT title_zones[T_ICON + 1];

static void title_layout( int width )
{
    int w = min( px( 460 ), width - px( 460 ) ), x = (width - w) / 2;

    w = max( w, px( 140 ) );
    SetRect( &title_zones[T_SEARCH], x, (TITLE_H - px( 32 )) / 2, x + w, (TITLE_H + px( 32 )) / 2 );
    /* the window's buttons as File Explorer's (startui explorerui.c): 46 by 32 */
    SetRect( &title_zones[T_CLOSE], width - px( 46 ), 0, width, px( 32 ) );
    SetRect( &title_zones[T_MAX], width - px( 92 ), 0, width - px( 46 ), px( 32 ) );
    SetRect( &title_zones[T_MIN], width - px( 138 ), 0, width - px( 92 ), px( 32 ) );
    SetRect( &title_zones[T_ICON], px( 12 ), (TITLE_H - px( 20 )) / 2, px( 36 ), (TITLE_H + px( 20 )) / 2 );
    if (search_edit)
    {
        RECT *s = &title_zones[T_SEARCH];
        int h = px( 18 );
        SetWindowPos( search_edit, NULL, s->left + px( 34 ), (s->top + s->bottom - h) / 2, s->right - s->left - px( 44 ), h,
                      SWP_NOZORDER | SWP_NOACTIVATE );
    }
}

static void title_paint( HDC hdc )
{
    RECT client, r;
    int x;
    BOOL active = GetActiveWindow() == frame;
    COLORREF text = active ? th.text : th.dim;

    GetClientRect( frame, &client );
    client.bottom = TITLE_H;
    if (!surface_size( &title_surf, client.right, client.bottom )) return;
    fill( &title_surf, &client, th.chrome );

    /* the program's picture */
    if (small_icon)
    {
        GdiFlush();
        DrawIconEx( title_surf.dc, title_zones[T_ICON].left, title_zones[T_ICON].top + px( 2 ), small_icon, px( 16 ), px( 16 ), 0, NULL, DI_NORMAL );
    }
    /* the document's name and whether it is saved */
    x = title_zones[T_ICON].left + px( 28 );
    {
        const WCHAR *name = document_name();
        int w = text_width( get_font( F_TITLE ), name, -1 );
        int limit = title_zones[T_SEARCH].left - px( 16 );

        SetRect( &r, x, 0, min( x + w + px( 2 ), limit ), TITLE_H );
        draw_text( &title_surf, name, -1, &r, get_font( F_TITLE ), text, DT_LEFT );
        if (file_path[0] && !modified && r.right + px( 80 ) < limit)
        {
            SetRect( &r, r.right + px( 12 ), 0, limit, TITLE_H );
            draw_text( &title_surf, string( IDS_SAVED ), -1, &r, get_font( F_TITLE ), th.dim, DT_LEFT );
        }
    }
    /* the search */
    {
        RECT s = title_zones[T_SEARCH];
        BOOL focused = GetFocus() == search_edit;

        fill_round( &title_surf, &s, (float)px( 6 ), focused ? th.field : th.dark ? RGB( 0x2b, 0x2b, 0x2b ) : RGB( 0xff, 0xff, 0xff ), 255 );
        outline_round( &title_surf, &s, (float)px( 6 ),
                       focused ? th.accent : title_hot == T_SEARCH ? th.checked_line : (th.dark ? th.field_line : RGB( 0xdd, 0xe0, 0xe4 )), 255 );
        draw_glyph( &title_surf, G_SEARCH, s.left + px( 12 ), (s.top + s.bottom - px( 16 )) / 2, px( 16 ), th.dim );
        if (!focused && !GetWindowTextLengthW( search_edit ))
        {
            r = s;
            r.left += px( 36 );
            draw_text( &title_surf, string( IDS_SEARCH ), -1, &r, get_font( F_UI ), th.dim, DT_LEFT );
        }
    }
    /* the window's buttons */
    for (int i = T_MIN; i <= T_CLOSE; i++)
    {
        int glyph = i == T_MIN ? G_MIN : i == T_CLOSE ? G_CLOSE : IsZoomed( frame ) ? G_RESTORE : G_MAX;
        BOOL hot = title_hot == i;
        COLORREF color = text;

        r = title_zones[i];
        if (hot && i == T_CLOSE)
        {
            fill( &title_surf, &r, title_down == i ? RGB( 0xc8, 0x3c, 0x2e ) : RGB( 0xc4, 0x2b, 0x1c ) );
            color = RGB( 255, 255, 255 );
        }
        else if (hot) fill( &title_surf, &r, title_down == i ? th.pressed : (th.dark ? th.hover : RGB( 0xe3, 0xe6, 0xea )) );
        draw_glyph( &title_surf, glyph, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2, px( 16 ), color );
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, client.right, client.bottom, title_surf.dc, 0, 0, SRCCOPY );
}

static int title_hit( POINT pt )
{
    if (pt.y >= TITLE_H) return T_NONE;
    for (int i = T_SEARCH; i <= T_CLOSE; i++)
        if (PtInRect( &title_zones[i], pt )) return i;
    return T_NONE;
}

static void invalidate_title(void)
{
    RECT r;
    GetClientRect( frame, &r );
    r.bottom = TITLE_H;
    InvalidateRect( frame, &r, FALSE );
}

static LRESULT CALLBACK search_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wparam == VK_RETURN)
        {
            command( IDM_SEARCHBOX );
            return 0;
        }
        if (wparam == VK_ESCAPE)
        {
            SetWindowTextW( hwnd, L"" );
            view_focus();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == '\r' || wparam == 0x1b) return 0;
        break;
    case WM_SETFOCUS:
        invalidate_title();
        break;
    case WM_KILLFOCUS:
        /* the field is drawn by the title bar again, its text with it */
        if (!GetWindowTextLengthW( hwnd )) ShowWindow( hwnd, SW_HIDE );
        invalidate_title();
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

/**********************************************************************
 *          The window
 */

void layout_frame(void)
{
    RECT client;
    int top = TITLE_H, rib = ribbon_height(), stat = STATUS_H;

    GetClientRect( frame, &client );
    title_layout( client.right );
    backstage_place( 0, top, client.right, client.bottom - top );
    SetWindowPos( ribbon, NULL, 0, top, client.right, rib, SWP_NOZORDER | SWP_NOACTIVATE );
    SetWindowPos( view, NULL, 0, top + rib, client.right, max( client.bottom - top - rib - stat, 0 ), SWP_NOZORDER | SWP_NOACTIVATE );
    SetWindowPos( status, NULL, 0, client.bottom - stat, client.right, stat, SWP_NOZORDER | SWP_NOACTIVATE );
    invalidate_title();
}

static int client_top_offset(void)
{
    return IsZoomed( frame ) ? GetSystemMetrics( SM_CYSIZEFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER ) : 0;
}

static void paint_frame_edges( HWND hwnd )
{
    RECT window, client;
    POINT origin = { 0, 0 };
    HDC hdc = GetWindowDC( hwnd );
    HBRUSH brush;

    if (!hdc) return;
    GetWindowRect( hwnd, &window );
    GetClientRect( hwnd, &client );
    ClientToScreen( hwnd, &origin );
    OffsetRect( &client, origin.x - window.left, origin.y - window.top );
    OffsetRect( &window, -window.left, -window.top );
    ExcludeClipRect( hdc, client.left, client.top, client.right, client.bottom );
    brush = CreateSolidBrush( th.chrome );
    FillRect( hdc, &window, brush );
    DeleteObject( brush );
    ReleaseDC( hwnd, hdc );
}

/* menus of the system dark or light with the app mode, as Windows 11's own
 * programs have them (uxtheme's unnamed SetPreferredAppMode, 135, and
 * FlushMenuThemes, 136; a system without them keeps its menus) */
static void menus_follow_theme(void)
{
    HMODULE uxtheme = GetModuleHandleW( L"uxtheme.dll" );
    int (WINAPI *set_mode)( int );
    void (WINAPI *flush)(void);

    if (!uxtheme && !(uxtheme = LoadLibraryW( L"uxtheme.dll" ))) return;
    set_mode = (void *)GetProcAddress( uxtheme, MAKEINTRESOURCEA( 135 ) );
    flush = (void *)GetProcAddress( uxtheme, MAKEINTRESOURCEA( 136 ) );
    if (set_mode) set_mode( th.dark ? 2 : 3 );   /* force dark, force light */
    if (flush) flush();
}

static HBRUSH field_brush;

/* Arctic's dwm draws a backdrop under the title bars uxtheme draws; this
 * window draws its own (DWMWA_ARCTIC_CAPTION, wine/arctic_dwm.h, height 0:
 * none). Windows has no such attribute and says so. */
static void no_caption_backdrop( HWND hwnd )
{
    UINT32 caption[5] = { 0 };

    DwmSetWindowAttribute( hwnd, 0x1005, caption, sizeof(caption) );
}

static void apply_theme(void)
{
    BOOL dark;

    theme_load();
    menus_follow_theme();
    if (field_brush) DeleteObject( field_brush );
    field_brush = NULL;
    dark = th.dark;
    DwmSetWindowAttribute( frame, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark) );
    fonts_reset();
    ribbon_theme_changed();
    view_theme_changed();
    paint_frame_edges( frame );
    InvalidateRect( frame, NULL, FALSE );
    backstage_theme_changed();
}

static LRESULT CALLBACK frame_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_CREATE:
        frame = hwnd;
        ribbon = ribbon_create( hwnd );
        if (!(view = view_create( hwnd ))) return -1;
        status = status_create( hwnd );
        search_edit = CreateWindowExW( 0, L"EDIT", NULL, WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                       NULL, instance, NULL );
        SendMessageW( search_edit, WM_SETFONT, (WPARAM)get_font( F_UI ), 0 );
        SetWindowSubclass( search_edit, search_proc, 1, 0 );
        SendMessageW( search_edit, 0x1501 /* EM_SETCUEBANNER */, TRUE, (LPARAM)string( IDS_SEARCH ) );
        no_caption_backdrop( hwnd );
        return 0;

    case WM_NCCALCSIZE:
    {
        /* no caption: the client area reaches the window's top (Wine asks
         * with a plain rectangle when the window is made, later with more) */
        RECT *rect = wparam ? &((NCCALCSIZE_PARAMS *)lparam)->rgrc[0] : (RECT *)lparam;
        int top = rect->top;
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );

        rect->top = top;
        if (IsZoomed( hwnd ))
        {
            /* maximized, the frame reaches past the screen: the client area
             * begins where the work area does */
            MONITORINFO mi = { sizeof(mi) };

            if (GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &mi ))
                rect->top = max( top, mi.rcWork.top );
            else rect->top = top + client_top_offset();
        }
        return result;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        MONITORINFO monitor = { sizeof(monitor) };
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );
        int bx = max( 0, -info->ptMaxPosition.x ), by = max( 0, -info->ptMaxPosition.y );

        if (GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &monitor ))
        {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left - bx;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top - by;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left + 2 * bx;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top + 2 * by;
        }
        info->ptMinTrackSize.x = px( 520 );
        info->ptMinTrackSize.y = px( 360 );
        return result;
    }

    case WM_NCPAINT:
        paint_frame_edges( hwnd );
        return 0;

    case WM_NCACTIVATE:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, (LPARAM)-1 );
        no_caption_backdrop( hwnd );
        paint_frame_edges( hwnd );
        invalidate_title();
        return result;
    }

    case WM_SETTEXT:
    case WM_SETICON:
    {
        LRESULT result = DefWindowProcW( hwnd, msg, wparam, lparam );
        paint_frame_edges( hwnd );
        invalidate_title();
        return result;
    }

    case WM_NCHITTEST:
    {
        LRESULT hit;
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        RECT rect;
        int zone;

        /* the title bar's buttons are this window's: the system's are never
         * named, so it draws none of its own over them */
        ScreenToClient( hwnd, &pt );
        zone = title_hit( pt );
        if (zone >= T_SEARCH && zone <= T_CLOSE) return HTCLIENT;
        pt.x = GET_X( lparam );
        pt.y = GET_Y( lparam );
        hit = DefWindowProcW( hwnd, msg, wparam, lparam );
        if (hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE || hit == HTHELP) return HTCAPTION;
        if (hit != HTCLIENT) return hit;
        GetClientRect( hwnd, &rect );
        ScreenToClient( hwnd, &pt );
        if (!IsZoomed( hwnd ) && pt.y < px( 4 ))
        {
            if (pt.x < px( 8 )) return HTTOPLEFT;
            if (pt.x >= rect.right - px( 8 )) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y >= TITLE_H) return HTCLIENT;
        zone = title_hit( pt );
        if (zone == T_ICON) return HTSYSMENU;
        if (zone == T_NONE) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTSYSMENU)
        {
            PostMessageW( hwnd, WM_CLOSE, 0, 0 );
            return 0;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        title_paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        int hot = title_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (hot != title_hot)
        {
            title_hot = hot;
            invalidate_title();
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (title_hot)
        {
            title_hot = T_NONE;
            invalidate_title();
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        title_down = title_hit( pt );
        if (title_down == T_SEARCH)
        {
            ShowWindow( search_edit, SW_SHOW );
            SetFocus( search_edit );
            title_down = T_NONE;
        }
        else if (title_down) SetCapture( hwnd );
        invalidate_title();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        int was = title_down;

        title_down = T_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        invalidate_title();
        if (was && title_hit( pt ) == was)
        {
            if (was == T_MIN) PostMessageW( hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0 );
            else if (was == T_MAX) PostMessageW( hwnd, WM_SYSCOMMAND, IsZoomed( hwnd ) ? SC_RESTORE : SC_MAXIMIZE, 0 );
            else if (was == T_CLOSE) PostMessageW( hwnd, WM_CLOSE, 0, 0 );
        }
        return 0;
    }
    case WM_RBUTTONUP:
    {
        POINT pt = { GET_X( lparam ), GET_Y( lparam ) };
        if (pt.y < TITLE_H)
        {
            HMENU menu = GetSystemMenu( hwnd, FALSE );
            int cmd;
            ClientToScreen( hwnd, &pt );
            cmd = TrackPopupMenu( menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL );
            if (cmd) PostMessageW( hwnd, WM_SYSCOMMAND, cmd, 0 );
        }
        return 0;
    }

    case WM_SIZE:
        no_caption_backdrop( hwnd );
        layout_frame();
        return 0;

    case WM_ACTIVATE:
        invalidate_title();
        if (LOWORD( wparam ) != WA_INACTIVE && !backstage_shown()) view_focus();
        return 0;

    case WM_SETFOCUS:
        if (!backstage_shown()) view_focus();
        return 0;

    case WM_COMMAND:
        if (lparam && (HWND)lparam == search_edit) return 0;
        command( LOWORD( wparam ) );
        return 0;

    case WM_SETTINGCHANGE:
        if (lparam && !lstrcmpW( (const WCHAR *)lparam, L"ImmersiveColorSet" )) apply_theme();
        break;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        apply_theme();
        break;

    case WM_CTLCOLOREDIT:
        /* the search field in the colour of its box */
        if (!field_brush) field_brush = CreateSolidBrush( th.field );
        SetTextColor( (HDC)wparam, th.text );
        SetBkColor( (HDC)wparam, th.field );
        return (LRESULT)field_brush;

    case WM_DROPFILES:
    {
        WCHAR path[MAX_PATH];
        if (DragQueryFileW( (HDROP)wparam, 0, path, MAX_PATH ) && ask_save_changes()) open_file( path );
        DragFinish( (HDROP)wparam );
        return 0;
    }

    case WM_CLOSE:
        if (!ask_save_changes()) return 0;
        {
            WINDOWPLACEMENT place = { sizeof(place) };
            GetWindowPlacement( hwnd, &place );
            set_setting( L"Left", place.rcNormalPosition.left );
            set_setting( L"Top", place.rcNormalPosition.top );
            set_setting( L"Width", place.rcNormalPosition.right - place.rcNormalPosition.left );
            set_setting( L"Height", place.rcNormalPosition.bottom - place.rcNormalPosition.top );
            set_setting( L"Maximized", place.showCmd == SW_SHOWMAXIMIZED );
            set_setting( L"Zoom", view_zoom() );
        }
        DestroyWindow( hwnd );
        return 0;

    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSEXW cls = { sizeof(cls) };
    HACCEL accel;
    WCHAR **argv;
    int argc, x, y, w, h;
    BOOL print = FALSE;
    const WCHAR *open = NULL;
    MSG msg;
    HDC hdc;

    instance = inst;
    SetProcessDPIAware();
    hdc = GetDC( 0 );
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    ReleaseDC( 0, hdc );
    OleInitialize( NULL );
    InitCommonControlsEx( &controls );
    LoadLibraryW( L"riched20.dll" );
    theme_load();
    menus_follow_theme();

    argv = CommandLineToArgvW( GetCommandLineW(), &argc );

    /* checks of the converters without a window: /rtf in.docx out.rtf */
    if (argc == 4 && !wcsicmp( argv[1], L"/rtf" ))
    {
        size_t len, rtf_len;
        BYTE *data = read_file( argv[2], &len );
        struct page_setup setup = { 11906, 16838, 1440, 1440, 1440, 1440 };
        struct docx_keep *k = NULL;
        char *rtf = data ? docx_to_rtf( data, len, &rtf_len, &setup, &k ) : NULL;

        if (!rtf) return 2;
        write_file( argv[3], rtf, rtf_len );
        return 0;
    }
    for (int i = 1; i < argc; i++)
    {
        if (!wcsicmp( argv[i], L"/p" ) || !wcsicmp( argv[i], L"-p" )) print = TRUE;
        else if (argv[i][0] != '/' || GetFileAttributesW( argv[i] ) != INVALID_FILE_ATTRIBUTES) open = argv[i];
    }

    small_icon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_WORDPAD ), IMAGE_ICON, px( 16 ), px( 16 ), 0 );
    cls.lpfnWndProc = frame_proc;
    cls.hInstance = inst;
    cls.hIcon = LoadImageW( inst, MAKEINTRESOURCEW( IDI_WORDPAD ), IMAGE_ICON, GetSystemMetrics( SM_CXICON ),
                            GetSystemMetrics( SM_CYICON ), 0 );
    cls.hIconSm = small_icon;
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    cls.lpszClassName = L"ArcticWordPad";
    RegisterClassExW( &cls );

    w = setting( L"Width", px( 1200 ) );
    h = setting( L"Height", px( 820 ) );
    x = setting( L"Left", CW_USEDEFAULT );
    y = setting( L"Top", CW_USEDEFAULT );
    {
        RECT r = { x, y, x + w, y + h }, work;
        SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
        if (x == (int)CW_USEDEFAULT || !IntersectRect( &r, &r, &work ) || r.right - r.left < px( 200 ))
        {
            w = min( w, work.right - work.left );
            h = min( h, work.bottom - work.top );
            x = work.left + (work.right - work.left - w) / 2;
            y = work.top + (work.bottom - work.top - h) / 2;
        }
    }
    if (!CreateWindowExW( WS_EX_ACCEPTFILES, cls.lpszClassName, string( IDS_APP_NAME ),
                          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, inst, NULL ))
        return 1;
    {
        BOOL dark = th.dark;
        DwmSetWindowAttribute( frame, 20, &dark, sizeof(dark) );
    }
    web_layout = setting( L"WebLayout", 0 ) != 0;
    view_reset_text();
    view_set_zoom( setting( L"Zoom", 100 ) );
    frame_title_changed();
    layout_frame();
    /* the ribbon's commands one after the other, unseen, into a file:
     * /selftest out.docx (and out.rtf beside it) */
    if (argc == 3 && !wcsicmp( argv[1], L"/selftest" ))
    {
        static const int steps[] =
        {
            IDM_STYLE_FIRST + 5, IDM_BOLD, IDM_ITALIC, IDM_UNDERLINE, IDM_STRIKE, IDM_SUPERSCRIPT, IDM_SUBSCRIPT,
            IDM_GROW, IDM_SHRINK, IDM_ALIGN_CENTER, IDM_ALIGN_RIGHT, IDM_ALIGN_JUSTIFY, IDM_ALIGN_LEFT,
            IDM_BULLETS, IDM_NUMBERING, IDM_INDENT, IDM_OUTDENT, IDM_STYLE_FIRST + 2, IDM_CLEARFORMAT,
            IDM_FONTCOLOR, IDM_HIGHLIGHT, IDM_PAGEBREAK, IDM_UNDO, IDM_REDO, IDM_SELECTALL, IDM_COPY,
        };
        WCHAR rtf_path[MAX_PATH];
        BOOL ok;

        quiet = TRUE;
        for (int i = 0; i < ARRAY_SIZE(steps); i++)
        {
            WCHAR line[64];
            CHARRANGE end = { -1, -1 };

            ed( EM_EXSETSEL, 0, (LPARAM)&end );
            swprintf( line, ARRAY_SIZE(line), L"\rStep %d: some text here", i );
            insert_text( line );
            /* the line just typed is the selection the command acts on */
            {
                LONG len = ed( WM_GETTEXTLENGTH, 0, 0 );
                CHARRANGE sel = { len - lstrlenW( line ) + 1, len };
                ed( EM_EXSETSEL, 0, (LPARAM)&sel );
            }
            command( steps[i] );
        }
        {
            CHARRANGE end = { -1, -1 };
            ed( EM_EXSETSEL, 0, (LPARAM)&end );
            insert_table( 3, 2 );
            insert_text( L"after the table" );
            change_case( IDM_CASE_UPPER );
            sort_paragraphs();
        }
        ok = save_file( argv[2], FMT_DOCX );
        lstrcpynW( rtf_path, argv[2], MAX_PATH - 5 );
        lstrcatW( rtf_path, L".rtf" );
        ok = save_file( rtf_path, FMT_RTF ) && ok;
        DestroyWindow( frame );
        return ok ? 0 : 3;
    }
    /* a check of the whole way, unseen: /convert in out (.docx, .rtf, .txt) */
    if (argc == 4 && !wcsicmp( argv[1], L"/convert" ))
    {
        BOOL ok;

        quiet = TRUE;
        open_file( argv[2] );
        ok = save_file( argv[3], format_of_path( argv[3] ) );
        DestroyWindow( frame );
        return ok ? 0 : 3;
    }
    ShowWindow( frame, setting( L"Maximized", 0 ) && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show );
    UpdateWindow( frame );
    if (open) open_file( open );
    if (print && open)
    {
        print_document( frame, FALSE );
        DestroyWindow( frame );
    }
    LocalFree( argv );

    accel = LoadAcceleratorsW( inst, MAKEINTRESOURCEW( IDA_ACCEL ) );
    while (GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        if (is_find_dialog_message( &msg )) continue;
        if (msg.hwnd != search_edit && TranslateAcceleratorW( frame, accel, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    OleUninitialize();
    return (int)msg.wParam;
}
