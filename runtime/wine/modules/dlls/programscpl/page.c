/*
 * Programs and Features of the Control Panel: the pages
 *
 * Windows 7's "Видалення або змінення програми": the programs in a list
 * (the name, the publisher, when it was installed, its size, its version,
 * each column sorts), Uninstall, Change and Repair over it for the chosen
 * one, its details under it; installed updates on a page of their own; a
 * program installed from a file, as appwiz.cpl's "Install..." did. The list
 * comes anew once an uninstaller has finished.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "programscpl.h"
#include "commdlg.h"
#include "shellapi.h"
#include "shlwapi.h"
#include "uxtheme.h"

#define ID_LIST         100
#define ID_UNINSTALL    101
#define ID_CHANGE       102
#define ID_REPAIR       103
#define ID_DETAILS      104
#define ID_INSTALL      105

#define ID_DO_REFRESH   200
#define ID_DO_UNINSTALL 201
#define ID_DO_CHANGE    202
#define ID_DO_REPAIR    203

enum { COLUMN_NAME, COLUMN_PUBLISHER, COLUMN_DATE, COLUMN_SIZE, COLUMN_VERSION };

struct programs_state
{
    struct apps apps;
    HIMAGELIST images;
    WCHAR selected[256];       /* the chosen program's key, across a new list */
    int sort_column;
    BOOL sort_down;
};

static struct programs_state *programs_state( struct view *view )
{
    struct page_state *state = view_state( view );

    if (!state->data) state->data = calloc( 1, sizeof(struct programs_state) );
    return state->data;
}

static struct app *chosen( struct programs_state *s )
{
    for (UINT i = 0; s->selected[0] && i < s->apps.count; i++)
        if (!wcscmp( s->apps.list[i].key, s->selected )) return &s->apps.list[i];
    return NULL;
}

static void go_page( struct view *view, UINT_PTR page )
{
    view_navigate( view, (UINT)page, 0 );
}

/* appwiz.cpl's "Install...": a setup program chosen, started */
static void install_link( struct view *view, UINT_PTR param )
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[MAX_PATH], file[MAX_PATH] = L"", title[128];
    SHELLEXECUTEINFOW info = { sizeof(info) };
    int len;

    lstrcpynW( title, load_string( IDS_INSTALL_TITLE ), ARRAY_SIZE(title) );
    len = swprintf( filter, ARRAY_SIZE(filter), L"%s%c*instal*.exe;*setup*.exe;*.msi%c", load_string( IDS_FILTER_INSTALLS ), 0, 0 );
    len += swprintf( filter + len, ARRAY_SIZE(filter) - len, L"%s%c*.exe%c", load_string( IDS_FILTER_PROGRAMS ), 0, 0 );
    len += swprintf( filter + len, ARRAY_SIZE(filter) - len, L"%s%c*.*%c", load_string( IDS_FILTER_ALL ), 0, 0 );
    filter[len] = 0;
    ofn.hwndOwner = GetAncestor( view_window( view ), GA_ROOT );
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_SIZE(file);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_HIDEREADONLY | OFN_ENABLESIZING | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW( &ofn )) return;
    info.lpVerb = L"open";
    info.lpFile = file;
    info.nShow = SW_SHOWNORMAL;
    info.hwnd = ofn.hwndOwner;
    ShellExecuteExW( &info );
}

static void update_buttons( struct view *view, struct programs_state *s )
{
    HWND content = view_window( view ), details = GetDlgItem( content, ID_DETAILS );
    struct app *app = chosen( s );

    EnableWindow( GetDlgItem( content, ID_UNINSTALL ), app && app->uninstall );
    EnableWindow( GetDlgItem( content, ID_CHANGE ), app && app->modify );
    EnableWindow( GetDlgItem( content, ID_REPAIR ), app && app->repair );
    if (details) SendMessageW( details, DM_SETAPP, 0, (LPARAM)app );
}

static int CALLBACK compare_apps( LPARAM a, LPARAM b, LPARAM param )
{
    struct programs_state *s = (struct programs_state *)param;
    const struct app *x = &s->apps.list[a], *y = &s->apps.list[b];
    int ret = 0;

    switch (s->sort_column)
    {
    case COLUMN_PUBLISHER:
        ret = StrCmpLogicalW( x->publisher ? x->publisher : L"", y->publisher ? y->publisher : L"" );
        break;
    case COLUMN_DATE:
    {
        DWORD dx = x->installed.wYear * 10000 + x->installed.wMonth * 100 + x->installed.wDay;
        DWORD dy = y->installed.wYear * 10000 + y->installed.wMonth * 100 + y->installed.wDay;
        ret = dx < dy ? -1 : dx > dy;
        break;
    }
    case COLUMN_SIZE:
        ret = x->size_kb < y->size_kb ? -1 : x->size_kb > y->size_kb;
        break;
    case COLUMN_VERSION:
        ret = StrCmpLogicalW( x->version ? x->version : L"", y->version ? y->version : L"" );
        break;
    }
    if (!ret) ret = StrCmpLogicalW( x->name, y->name );
    return s->sort_down ? -ret : ret;
}

static void build_page( struct view *view, BOOL updates )
{
    static const UINT columns[] = { IDS_COL_NAME, IDS_COL_PUBLISHER, IDS_COL_DATE, IDS_COL_SIZE, IDS_COL_VERSION };
    static const int widths[] = { 280, 170, 110, 80, 110 };
    struct programs_state *s = programs_state( view );
    ULONGLONG total = 0;
    UINT shown = 0;
    HWND list, details;
    HICON generic = NULL;
    int cx = GetSystemMetrics( SM_CXSMICON );

    if (!s) return;
    apps_load( &s->apps );

    view_nav_home( view );
    if (updates) view_nav_link( view, load_string( IDS_PROGRAMS ), go_page, PAGE_PROGRAMS );
    else view_nav_link( view, load_string( IDS_UPDATES ), go_page, PAGE_UPDATES );
    view_nav_link( view, load_string( IDS_INSTALL ), install_link, 0 );

    /* Windows 11's Installed apps: what to do above, the list on a card */
    view_heading( view, load_string( updates ? IDS_UPDATES_TITLE : IDS_TITLE ) );
    {
        WCHAR name[256];

        if (!updates)
        {
            lstrcpynW( name, load_string( IDS_INSTALL ), ARRAY_SIZE(name) );
            view_card( view, view_icon( ICON_INSTALL ), name, load_string( IDS_INSTALL_HINT ), L"BUTTON",
                       load_string( IDS_BROWSE_INSTALL ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_INSTALL );
            lstrcpynW( name, load_string( IDS_UPDATES ), ARRAY_SIZE(name) );
            view_card_link( view, view_icon( ICON_UPDATES ), name, NULL, go_page, PAGE_UPDATES );
        }
        else
        {
            lstrcpynW( name, load_string( IDS_PROGRAMS ), ARRAY_SIZE(name) );
            view_card_link( view, view_icon( ICON_PROGRAMS ), name, NULL, go_page, PAGE_PROGRAMS );
        }
    }
    /* how many, and their size, over them */
    for (UINT i = 0; i < s->apps.count; i++)
        if (s->apps.list[i].update == updates)
        {
            total += s->apps.list[i].size_kb;
            shown++;
        }
    if (shown)
    {
        WCHAR size[64] = L"";

        StrFormatByteSizeW( (LONGLONG)total * 1024, size, ARRAY_SIZE(size) );
        view_section( view, format_string( IDS_TOTAL, shown, size ) );
    }
    else view_section( view, load_string( IDS_NONE ) );
    shown = 0;
    total = 0;

    view_card_area( view, px( 28 + 12 + 330 + 12 + 110 ), NULL, 0 );
    view_card_control( view, L"BUTTON", load_string( IDS_UNINSTALL ), BS_PUSHBUTTON | WS_TABSTOP, 0, 0, px( 100 ), px( 28 ),
                       ID_UNINSTALL );
    if (!updates)
    {
        view_card_control( view, L"BUTTON", load_string( IDS_CHANGE ), BS_PUSHBUTTON | WS_TABSTOP, px( 108 ), 0, px( 100 ),
                           px( 28 ), ID_CHANGE );
        view_card_control( view, L"BUTTON", load_string( IDS_REPAIR ), BS_PUSHBUTTON | WS_TABSTOP, px( 216 ), 0, px( 100 ),
                           px( 28 ), ID_REPAIR );
    }
    list = view_card_control( view, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                              0, px( 40 ), 0, px( 330 ), ID_LIST );
    SetWindowTheme( list, L"Explorer", NULL );
    SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                  LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
    /* on the card's colour */
    SendMessageW( list, LVM_SETBKCOLOR, 0, COLOR_CARD );
    SendMessageW( list, LVM_SETTEXTBKCOLOR, 0, COLOR_CARD );
    SendMessageW( list, LVM_SETTEXTCOLOR, 0, COLOR_TEXT );
    for (UINT i = 0; i < ARRAY_SIZE(columns); i++)
    {
        LVCOLUMNW column = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };

        column.pszText = load_string( columns[i] );
        column.cx = px( widths[i] );
        column.fmt = i == COLUMN_SIZE ? LVCFMT_RIGHT : LVCFMT_LEFT;
        SendMessageW( list, LVM_INSERTCOLUMNW, i, (LPARAM)&column );
    }

    /* the icons: the program's own, else Windows' picture of a program */
    if (s->images) ImageList_Destroy( s->images );
    s->images = ImageList_Create( cx, cx, ILC_COLOR32 | ILC_MASK, 16, 16 );
    {
        SHSTOCKICONINFO stock = { sizeof(stock) };

        if (SUCCEEDED(SHGetStockIconInfo( SIID_APPLICATION, SHGSI_ICON | SHGSI_SMALLICON, &stock ))) generic = stock.hIcon;
        ImageList_AddIcon( s->images, generic );
        if (generic) DestroyIcon( generic );
    }
    SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)s->images );

    for (UINT i = 0; i < s->apps.count; i++)
    {
        struct app *app = &s->apps.list[i];
        LVITEMW item = { LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE };
        WCHAR text[64];
        HICON icon = NULL;
        int index;

        if (app->update != updates) continue;
        item.iItem = shown++;
        item.pszText = app->name;
        item.lParam = i;
        item.iImage = 0;
        if (app->icon && ExtractIconExW( app->icon, app->icon_index, NULL, &icon, 1 ) == 1 && icon)
        {
            item.iImage = ImageList_AddIcon( s->images, icon );
            DestroyIcon( icon );
        }
        index = SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.iSubItem = COLUMN_PUBLISHER;
        item.pszText = app->publisher ? app->publisher : (WCHAR *)L"";
        SendMessageW( list, LVM_SETITEMTEXTW, index, (LPARAM)&item );
        if (app->installed.wYear &&
            GetDateFormatW( LOCALE_USER_DEFAULT, DATE_SHORTDATE, &app->installed, NULL, text, ARRAY_SIZE(text) ))
        {
            item.iSubItem = COLUMN_DATE;
            item.pszText = text;
            SendMessageW( list, LVM_SETITEMTEXTW, index, (LPARAM)&item );
        }
        if (app->size_kb && StrFormatByteSizeW( (LONGLONG)app->size_kb * 1024, text, ARRAY_SIZE(text) ))
        {
            item.iSubItem = COLUMN_SIZE;
            item.pszText = text;
            SendMessageW( list, LVM_SETITEMTEXTW, index, (LPARAM)&item );
        }
        item.iSubItem = COLUMN_VERSION;
        item.pszText = app->version ? app->version : (WCHAR *)L"";
        SendMessageW( list, LVM_SETITEMTEXTW, index, (LPARAM)&item );
        total += app->size_kb;
    }
    SendMessageW( list, LVM_SORTITEMS, (WPARAM)s, (LPARAM)compare_apps );
    /* the program chosen before stays chosen */
    if (chosen( s ))
    {
        LVFINDINFOW find = { LVFI_PARAM };
        LVITEMW item = { 0 };
        int index;

        find.lParam = chosen( s ) - s->apps.list;
        if ((index = SendMessageW( list, LVM_FINDITEMW, -1, (LPARAM)&find )) >= 0)
        {
            item.stateMask = item.state = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW( list, LVM_SETITEMSTATE, index, (LPARAM)&item );
            SendMessageW( list, LVM_ENSUREVISIBLE, index, FALSE );
        }
        else s->selected[0] = 0;
    }

    details_register();
    details = view_card_control( view, DETAILS_CLASS, L"", 0, 0, px( 28 + 12 + 330 + 12 ), 0, px( 110 ), ID_DETAILS );
    (void)details;
    (void)total;
    update_buttons( view, s );
}

void programs_build( struct view *view )
{
    build_page( view, FALSE );
}

void updates_build( struct view *view )
{
    build_page( view, TRUE );
}

/* waits for the uninstaller, then the list comes anew */
struct waiter
{
    HANDLE process;
    HWND content;
};

static DWORD WINAPI wait_thread( void *arg )
{
    struct waiter *w = arg;

    WaitForSingleObject( w->process, INFINITE );
    CloseHandle( w->process );
    PostMessageW( w->content, WM_COMMAND, MAKEWPARAM( ID_DO_REFRESH, 0 ), (LPARAM)w->content );
    free( w );
    return 0;
}

static void run( struct view *view, struct programs_state *s, UINT which )
{
    HWND content = view_window( view );
    struct app *app = chosen( s );
    const WCHAR *line;
    HANDLE process;

    if (!app) return;
    line = which == ID_DO_UNINSTALL ? app->uninstall : which == ID_DO_CHANGE ? app->modify : app->repair;
    if (!(process = app_run( content, app, line ))) return;
    if (process == INVALID_HANDLE_VALUE)
    {
        /* its key went: the list without it */
        s->selected[0] = 0;
        view_rebuild( view );
        return;
    }
    {
        struct waiter *w = malloc( sizeof(*w) );
        HANDLE thread;

        if (w)
        {
            w->process = process;
            w->content = content;
            if ((thread = CreateThread( NULL, 0, wait_thread, w, 0, NULL ))) CloseHandle( thread );
            else
            {
                CloseHandle( process );
                free( w );
            }
        }
        else CloseHandle( process );
    }
}

static void post( struct view *view, UINT id )
{
    HWND content = view_window( view );

    PostMessageW( content, WM_COMMAND, MAKEWPARAM( id, 0 ), (LPARAM)content );
}

static void context_menu( struct view *view, struct programs_state *s, HWND list )
{
    struct app *app = chosen( s );
    HMENU menu;
    POINT pt;
    UINT cmd;

    if (!app || !(menu = CreatePopupMenu())) return;
    AppendMenuW( menu, MF_STRING | (app->uninstall ? 0 : MF_GRAYED), ID_DO_UNINSTALL, load_string( IDS_UNINSTALL ) );
    if (app->modify) AppendMenuW( menu, MF_STRING, ID_DO_CHANGE, load_string( IDS_CHANGE ) );
    if (app->repair) AppendMenuW( menu, MF_STRING, ID_DO_REPAIR, load_string( IDS_REPAIR ) );
    SetMenuDefaultItem( menu, app->uninstall ? ID_DO_UNINSTALL : ID_DO_CHANGE, FALSE );
    GetCursorPos( &pt );
    cmd = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, list, NULL );
    DestroyMenu( menu );
    if (cmd) post( view, cmd );
}

BOOL programs_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct programs_state *s = programs_state( view );

    if (!s) return FALSE;
    switch (id)
    {
    case ID_LIST:
        if (code == LVN_ITEMCHANGED)
        {
            const NMLISTVIEW *nm = (const NMLISTVIEW *)view_notify( view );

            if (nm && (nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && nm->lParam >= 0 &&
                (UINT)nm->lParam < s->apps.count)
                lstrcpynW( s->selected, s->apps.list[nm->lParam].key, ARRAY_SIZE(s->selected) );
            else if (nm && (nm->uChanged & LVIF_STATE) && (nm->uOldState & LVIS_SELECTED) &&
                     SendMessageW( control, LVM_GETSELECTEDCOUNT, 0, 0 ) == 0)
                s->selected[0] = 0;
            update_buttons( view, s );
        }
        else if (code == LVN_COLUMNCLICK)
        {
            const NMLISTVIEW *nm = (const NMLISTVIEW *)view_notify( view );

            if (nm)
            {
                if (nm->iSubItem == s->sort_column) s->sort_down = !s->sort_down;
                else
                {
                    s->sort_column = nm->iSubItem;
                    s->sort_down = FALSE;
                }
                SendMessageW( control, LVM_SORTITEMS, (WPARAM)s, (LPARAM)compare_apps );
            }
        }
        else if (code == LVN_ITEMACTIVATE)
        {
            struct app *app = chosen( s );

            if (app) post( view, app->uninstall ? ID_DO_UNINSTALL : app->modify ? ID_DO_CHANGE : ID_DO_REPAIR );
        }
        else if (code == NM_RCLICK) context_menu( view, s, control );
        return TRUE;
    case ID_UNINSTALL:
        if (code == BN_CLICKED) post( view, ID_DO_UNINSTALL );
        return TRUE;
    case ID_CHANGE:
        if (code == BN_CLICKED) post( view, ID_DO_CHANGE );
        return TRUE;
    case ID_REPAIR:
        if (code == BN_CLICKED) post( view, ID_DO_REPAIR );
        return TRUE;
    case ID_INSTALL:
        if (code == BN_CLICKED) install_link( view, 0 );
        return TRUE;
    case ID_DO_UNINSTALL:
    case ID_DO_CHANGE:
    case ID_DO_REPAIR:
        run( view, s, id );
        return TRUE;
    case ID_DO_REFRESH:
        view_rebuild( view );
        return TRUE;
    }
    return FALSE;
}
