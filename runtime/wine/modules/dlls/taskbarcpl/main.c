/*
 * Taskbar and Start Menu of the Control Panel (taskbarcpl.dll)
 *
 * Windows' item {0DF44EAA-FF21-4412-828E-260A8728E7F1}: not the dialog with
 * its tabs but pages of the Control Panel inside Explorer drawn as Windows
 * 11's Settings, one for each tab the dialog had, each change at once:
 *
 *  - the taskbar: locked, hidden by itself, on top, small buttons, how the
 *    buttons combine (explorer's tray says them and sets them,
 *    ArcticTraySettings, as its page's Apply did);
 *  - its look: the preset, the effect, the colour, the opacity, the blur,
 *    the text (startui.dll, ArcticLook*);
 *  - the buttons: those of Windows 10 or 11, in the middle or not, the
 *    Start button's picture, the widgets (ArcticTaskbarButtons*);
 *  - the Start menu: its style, what it shows (each style's own dialog,
 *    the classic menu's explorer's), the look of the taskbar or its own;
 *  - the notification area: the clock, its seconds, Show desktop.
 *
 * The taskbar's menu and Options_RunDLL 1 open this item (ArcticTaskbarOpen).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdio.h>

#include "cpanel.h"
#include "commctrl.h"
#include "commdlg.h"
#include "taskbarcpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0x0df44eaa, 0xff21, 0x4412, { 0x82, 0x8e, 0x26, 0x0a, 0x87, 0x28, 0xe7, 0xf1 } };
const WCHAR cp_class_name[] = L"Taskbar";

/* the pages, in the order of the dialog's tabs */
enum { PAGE_TASKBAR, PAGE_LOOK, PAGE_BUTTONS, PAGE_START, PAGE_NOTIFY, PAGE_COUNT };

static const UINT page_names[PAGE_COUNT] = { IDS_TASKBAR, IDS_LOOK, IDS_BUTTONS, IDS_START, IDS_NOTIFY };
static const UINT page_hints[PAGE_COUNT] = { IDS_TASKBAR_HINT, IDS_LOOK_HINT, IDS_BUTTONS_HINT, IDS_START_HINT, IDS_NOTIFY_HINT };
static const UINT page_icons[PAGE_COUNT] = { ICON_TASKBAR, ICON_LOOK, ICON_BUTTONS, ICON_START, ICON_NOTIFY };

/* the controls */
#define ID_LOCK         100
#define ID_AUTOHIDE     101
#define ID_ONTOP        102
#define ID_SMALL        103
#define ID_COMBINE      104
#define ID_PRESET       110
#define ID_EFFECT       111
#define ID_COLOR        112
#define ID_OPACITY      113
#define ID_BLUR         114
#define ID_TEXT         115
#define ID_BUTTON_STYLE 120
#define ID_ALIGNMENT    121
#define ID_START_KIND   122
#define ID_START_BROWSE 123
#define ID_WIDGETS      124
#define ID_START_STYLE  130
#define ID_CUSTOMIZE    131
#define ID_INHERIT      132
#define ID_CLOCK        140
#define ID_SECONDS      141
#define ID_DESKTOP      142

/* done after the control's notification is over */
#define ID_DO_REBUILD   200
#define ID_DO_COMBO     201     /* + the combo's id - 100 */

#define COMBO_WIDTH     px( 260 )

/* the look's values (startui.dll's): preset, effect, colour, opacity, blur, text, inherit */
enum { V_PRESET, V_EFFECT, V_COLOR, V_OPACITY, V_BLUR, V_TEXT, V_INHERIT, V_COUNT };
#define PRESET_CUSTOM   6
#define EFFECT_SOLID    1       /* ARCTIC_ACCENT_ENABLE_GRADIENT */
#define EFFECT_BLUR     3       /* ARCTIC_ACCENT_ENABLE_BLURBEHIND */
#define EFFECT_ACRYLIC  4       /* ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND */

/* the styles of Start (startui.dll's enum start_style), as listed */
enum { START_STANDARD, START_CLASSIC, START_WIN10, START_WIN11, START_COUNT };
static const int start_order[START_COUNT] = { START_STANDARD, START_WIN10, START_WIN11, START_CLASSIC };
static const UINT start_names[START_COUNT] =
{
    [START_STANDARD] = IDS_STYLE_STANDARD, [START_CLASSIC] = IDS_STYLE_CLASSIC,
    [START_WIN10] = IDS_STYLE_WIN10, [START_WIN11] = IDS_STYLE_WIN11,
};

/* the tray's bits (ArcticTraySettings) */
#define TRAY_LOCK       0x001
#define TRAY_AUTOHIDE   0x002
#define TRAY_ONTOP      0x004
#define TRAY_SMALL      0x008
#define TRAY_GLOM_SHIFT 4
#define TRAY_GLOM_MASK  0x030
#define TRAY_NO_CLOCK   0x040
#define TRAY_SECONDS    0x080
#define TRAY_DESKTOP    0x200
#define TRAY_CLOCK_POLICY 0x400

/**********************************************************************
 *          What the others keep
 */

static HWND tray_window(void)
{
    return FindWindowW( L"Shell_TrayWnd", NULL );
}

static BOOL tray_call( WPARAM what, LPARAM param, DWORD_PTR *result )
{
    HWND tray = tray_window();
    DWORD_PTR ret = 0;

    if (!tray || !SendMessageTimeoutW( tray, RegisterWindowMessageW( L"ArcticTraySettings" ), what, param,
                                       SMTO_ABORTIFHUNG, 5000, &ret ))
        return FALSE;
    if (result) *result = ret;
    return TRUE;
}

static BOOL tray_get( DWORD *bits )
{
    DWORD_PTR ret;

    if (!tray_call( 0, 0, &ret )) return FALSE;
    *bits = (DWORD)ret;
    return TRUE;
}

static void tray_set( DWORD bits )
{
    tray_call( 1, bits & ~TRAY_CLOCK_POLICY, NULL );
}

static void *startui( const char *name )
{
    HMODULE module = LoadLibraryW( L"startui.dll" );

    return module ? (void *)GetProcAddress( module, name ) : NULL;
}

static BOOL look_get( int which, DWORD *values )
{
    void (WINAPI *get)( int, DWORD * ) = startui( "ArcticLookGet" );

    memset( values, 0, V_COUNT * sizeof(*values) );
    if (!get) return FALSE;
    get( which, values );
    return TRUE;
}

static void look_set( int which, const DWORD *values )
{
    void (WINAPI *set)( HWND, int, const DWORD * ) = startui( "ArcticLookSet" );

    if (set) set( tray_window(), which, values );
}

/* the names of a kind of choice (0 presets, 1 effects, 2 the text) in a list */
static void fill_choices( HWND combo, UINT kind, DWORD current )
{
    BOOL (WINAPI *choice)( UINT, UINT, DWORD *, WCHAR *, UINT ) = startui( "ArcticLookChoice" );
    WCHAR name[128];
    DWORD value;
    LRESULT index;

    for (UINT i = 0; choice && choice( kind, i, &value, name, ARRAY_SIZE(name) ); i++)
    {
        index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)name );
        SendMessageW( combo, CB_SETITEMDATA, index, value );
        if (value == current) SendMessageW( combo, CB_SETCURSEL, index, 0 );
    }
}

static LPARAM combo_data( HWND combo )
{
    LRESULT index = SendMessageW( combo, CB_GETCURSEL, 0, 0 );

    return index == CB_ERR ? -1 : SendMessageW( combo, CB_GETITEMDATA, index, 0 );
}

static void combo_add( HWND combo, const WCHAR *text, LPARAM data, BOOL current )
{
    LRESULT index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );

    SendMessageW( combo, CB_SETITEMDATA, index, data );
    if (current) SendMessageW( combo, CB_SETCURSEL, index, 0 );
}

/**********************************************************************
 *          The cards
 */

static void defer( struct view *view, UINT id )
{
    HWND content = view_window( view );

    PostMessageW( content, WM_COMMAND, MAKEWPARAM( id, 0 ), (LPARAM)content );
}

static HICON icon_of( UINT icon )
{
    return icon ? view_icon( icon ) : NULL;
}

static HWND card_combo( struct view *view, UINT icon, UINT title, UINT hint, UINT id )
{
    WCHAR name[256];

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    return view_card( view, icon_of( icon ), name, hint ? load_string( hint ) : NULL, WC_COMBOBOXW, L"",
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, COMBO_WIDTH, px( 300 ), id );
}

static HWND card_toggle( struct view *view, UINT icon, UINT title, UINT hint, BOOL on, UINT id )
{
    WCHAR name[256];

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    return view_card_toggle( view, icon_of( icon ), name, hint ? load_string( hint ) : NULL, on, id );
}

static HWND card_button( struct view *view, UINT icon, UINT title, const WCHAR *hint, UINT button, UINT id )
{
    WCHAR name[256];

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    return view_card( view, icon_of( icon ), name, hint, L"BUTTON", load_string( button ), BS_PUSHBUTTON | WS_TABSTOP,
                      px( 130 ), px( 28 ), id );
}

/* a slider; the value under the card's name, in the format given */
static HWND card_slider( struct view *view, UINT title, UINT format, DWORD value, DWORD low, DWORD high, UINT id )
{
    WCHAR name[256];
    HWND slider;

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    slider = view_card( view, NULL, name, format_string( format, value ), TRACKBAR_CLASSW, L"",
                        TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, COMBO_WIDTH, px( 26 ), id );
    SendMessageW( slider, TBM_SETRANGE, TRUE, MAKELPARAM( low, high ) );
    SendMessageW( slider, TBM_SETPAGESIZE, 0, 10 );
    SendMessageW( slider, TBM_SETPOS, TRUE, value );
    return slider;
}

static void go_page( struct view *view, UINT_PTR page )
{
    view_navigate( view, (UINT)page, 0 );
}

static void go_item( struct view *view, UINT_PTR item )
{
    view_browse_item( view, (const WCHAR *)item );
}

static void nav_links( struct view *view, UINT current )
{
    view_nav_home( view );
    for (UINT i = 0; i < PAGE_COUNT; i++)
        if (i != current) view_nav_link( view, load_string( page_names[i] ), go_page, i );
    view_nav_see_also( view, load_string( IDS_PERSONALIZE ), go_item,
                       (UINT_PTR)L"::{C555438B-3C23-4769-A71F-B6D3D9B6053A}\\pagePersonalize" );
    view_nav_see_also( view, load_string( IDS_DISPLAY ), go_item, (UINT_PTR)L"::{C555438B-3C23-4769-A71F-B6D3D9B6053A}" );
}

/* the other pages, as Windows 11 lists the rest */
static void related( struct view *view, UINT current )
{
    view_section( view, load_string( IDS_RELATED ) );
    for (UINT i = 0; i < PAGE_COUNT; i++)
    {
        WCHAR name[128];

        if (i == current) continue;
        lstrcpynW( name, load_string( page_names[i] ), ARRAY_SIZE(name) );
        view_card_link( view, view_icon( page_icons[i] ), name, load_string( page_hints[i] ), go_page, i );
    }
}

static void begin_page( struct view *view, UINT page )
{
    nav_links( view, page );
    view_heading( view, load_string( page_names[page] ) );
}

static BOOL no_taskbar( struct view *view )
{
    WCHAR name[256];

    lstrcpynW( name, load_string( IDS_NO_TASKBAR ), ARRAY_SIZE(name) );
    view_card( view, view_icon( ICON_TASKBAR ), name, NULL, NULL, NULL, 0, 0, 0, 0 );
    return FALSE;
}

/**********************************************************************
 *          The taskbar
 */

static void taskbar_build( struct view *view )
{
    static const UINT glom[] = { IDS_GLOM_ALWAYS, IDS_GLOM_FULL, IDS_GLOM_NEVER };
    DWORD bits;
    HWND combo;

    begin_page( view, PAGE_TASKBAR );
    if (!tray_get( &bits ))
    {
        no_taskbar( view );
        return;
    }
    view_section( view, load_string( IDS_BEHAVIOR ) );
    card_toggle( view, ICON_LOCK, IDS_LOCK, 0, (bits & TRAY_LOCK) != 0, ID_LOCK );
    card_toggle( view, ICON_AUTOHIDE, IDS_AUTOHIDE, 0, (bits & TRAY_AUTOHIDE) != 0, ID_AUTOHIDE );
    card_toggle( view, ICON_ONTOP, IDS_ONTOP, 0, (bits & TRAY_ONTOP) != 0, ID_ONTOP );
    card_toggle( view, ICON_SMALL_BUTTONS, IDS_SMALL, 0, (bits & TRAY_SMALL) != 0, ID_SMALL );
    combo = card_combo( view, ICON_COMBINE, IDS_COMBINE, 0, ID_COMBINE );
    for (UINT i = 0; i < ARRAY_SIZE(glom); i++)
        combo_add( combo, load_string( glom[i] ), i, i == (bits & TRAY_GLOM_MASK) >> TRAY_GLOM_SHIFT );
    related( view, PAGE_TASKBAR );
}

static void taskbar_toggle( UINT id, BOOL on )
{
    static const struct { UINT id; DWORD bit; } bits[] =
    {
        { ID_LOCK, TRAY_LOCK }, { ID_AUTOHIDE, TRAY_AUTOHIDE }, { ID_ONTOP, TRAY_ONTOP }, { ID_SMALL, TRAY_SMALL },
        { ID_CLOCK, TRAY_NO_CLOCK }, { ID_SECONDS, TRAY_SECONDS }, { ID_DESKTOP, TRAY_DESKTOP },
    };
    DWORD now;

    if (!tray_get( &now )) return;
    for (UINT i = 0; i < ARRAY_SIZE(bits); i++)
    {
        if (bits[i].id != id) continue;
        /* the clock's bit says it is hidden */
        if (id == ID_CLOCK) on = !on;
        if (on) now |= bits[i].bit;
        else now &= ~bits[i].bit;
        tray_set( now );
    }
}

/**********************************************************************
 *          The look: the taskbar's (-1), or a style of Start's own
 */

/* effect, colour, opacity, blur and text; the opacity has no meaning to a
 * solid colour, the blur only to the effects that blur */
static void look_cards( struct view *view, const DWORD *v, BOOL enabled )
{
    WCHAR name[128];
    HWND control;

    control = card_combo( view, ICON_EFFECT, IDS_EFFECT, 0, ID_EFFECT );
    fill_choices( control, 1, v[V_EFFECT] );
    EnableWindow( control, enabled );

    lstrcpynW( name, load_string( IDS_COLOR ), ARRAY_SIZE(name) );
    view_card( view, view_icon( ICON_LOOK ), name, NULL, NULL, NULL, 0, 0, 0, 0 );
    control = view_card_extra( view, L"ArcticTile", L"", WS_TABSTOP, px( 46 ), px( 36 ), ID_COLOR );
    tile_set_color( control, v[V_COLOR] );
    EnableWindow( control, enabled );

    control = card_slider( view, IDS_OPACITY, IDS_PERCENT, v[V_OPACITY], 0, 100, ID_OPACITY );
    EnableWindow( control, enabled && v[V_EFFECT] != EFFECT_SOLID );
    control = card_slider( view, IDS_BLUR, IDS_NUMBER, v[V_BLUR], 1, 100, ID_BLUR );
    EnableWindow( control, enabled && (v[V_EFFECT] == EFFECT_ACRYLIC || v[V_EFFECT] == EFFECT_BLUR) );
    control = card_combo( view, 0, IDS_TEXT, 0, ID_TEXT );
    fill_choices( control, 2, v[V_TEXT] );
    EnableWindow( control, enabled );
}

static void look_build( struct view *view )
{
    DWORD v[V_COUNT];
    HWND control;

    begin_page( view, PAGE_LOOK );
    if (!look_get( -1, v ))
    {
        no_taskbar( view );
        return;
    }
    control = card_combo( view, ICON_PRESET, IDS_PRESET, IDS_PRESET_HINT, ID_PRESET );
    fill_choices( control, 0, v[V_PRESET] );
    view_section( view, load_string( IDS_LOOK_DETAILS ) );
    look_cards( view, v, TRUE );
    related( view, PAGE_LOOK );
}

/* which look the page changes: the taskbar's, or the style of Start's */
static int look_which( struct view *view )
{
    int (WINAPI *style)(void);

    if (view_state( view )->page != PAGE_START) return -1;
    return (style = startui( "ArcticStartStyleGet" )) ? style() : -1;
}

/* a value of the look changed: a preset, or Custom from then on */
static void look_changed( struct view *view, UINT id, DWORD value )
{
    int which = look_which( view );
    void (WINAPI *preset)( DWORD * ) = startui( "ArcticLookPreset" );
    DWORD v[V_COUNT];

    if (!look_get( which, v )) return;
    switch (id)
    {
    case ID_PRESET:
        v[V_PRESET] = value;
        if (preset) preset( v );
        break;
    case ID_EFFECT:  v[V_EFFECT] = value; v[V_PRESET] = PRESET_CUSTOM; break;
    case ID_COLOR:   v[V_COLOR] = value; v[V_PRESET] = PRESET_CUSTOM; break;
    case ID_OPACITY: v[V_OPACITY] = value; v[V_PRESET] = PRESET_CUSTOM; break;
    case ID_BLUR:    v[V_BLUR] = value; v[V_PRESET] = PRESET_CUSTOM; break;
    /* the text goes with any preset */
    case ID_TEXT:    v[V_TEXT] = value; break;
    case ID_INHERIT: v[V_INHERIT] = value; break;
    }
    look_set( which, v );
}

static void choose_color( struct view *view )
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };
    DWORD v[V_COUNT];

    if (!look_get( look_which( view ), v )) return;
    cc.hwndOwner = GetAncestor( view_window( view ), GA_ROOT );
    cc.rgbResult = v[V_COLOR];
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW( &cc )) look_changed( view, ID_COLOR, cc.rgbResult );
}

/**********************************************************************
 *          The buttons
 */

static void buttons_build( struct view *view )
{
    void (WINAPI *get)( DWORD *, WCHAR *, UINT ) = startui( "ArcticTaskbarButtonsGet" );
    DWORD v[3] = { 0 };
    WCHAR image[MAX_PATH] = L"";
    HWND combo;

    begin_page( view, PAGE_BUTTONS );
    if (!get || !tray_window())
    {
        no_taskbar( view );
        return;
    }
    get( v, image, ARRAY_SIZE(image) );
    combo = card_combo( view, ICON_BUTTONS, IDS_BUTTON_STYLE, 0, ID_BUTTON_STYLE );
    combo_add( combo, L"Windows 10", 0, !v[0] );
    combo_add( combo, L"Windows 11", 1, v[0] != 0 );
    combo = card_combo( view, ICON_TASKBAR, IDS_ALIGNMENT, 0, ID_ALIGNMENT );
    combo_add( combo, load_string( IDS_ALIGN_LEFT ), 0, !v[1] );
    combo_add( combo, load_string( IDS_ALIGN_CENTER ), 1, v[1] != 0 );
    combo = card_combo( view, ICON_START, IDS_START_BUTTON, 0, ID_START_KIND );
    combo_add( combo, load_string( IDS_START_STANDARD ), 0, !image[0] );
    combo_add( combo, load_string( IDS_START_IMAGE ), 1, image[0] != 0 );
    if (image[0])
    {
        const WCHAR *file = wcsrchr( image, '\\' );

        card_button( view, 0, IDS_START_PICTURE, file ? file + 1 : image, IDS_BROWSE, ID_START_BROWSE );
    }
    combo = card_combo( view, ICON_WIDGETS, IDS_WIDGETS, 0, ID_WIDGETS );
    combo_add( combo, load_string( IDS_WIDGETS_BOTH ), 2, v[2] == 2 );
    combo_add( combo, load_string( IDS_WIDGETS_ICON ), 1, v[2] == 1 );
    combo_add( combo, load_string( IDS_WIDGETS_OFF ), 0, v[2] == 0 );
    related( view, PAGE_BUTTONS );
}

/* one of the buttons' values; image NULL keeps the picture */
static void buttons_change( UINT index, DWORD value, const WCHAR *image )
{
    void (WINAPI *get)( DWORD *, WCHAR *, UINT ) = startui( "ArcticTaskbarButtonsGet" );
    void (WINAPI *set)( HWND, const DWORD *, const WCHAR * ) = startui( "ArcticTaskbarButtonsSet" );
    WCHAR now[MAX_PATH] = L"";
    DWORD v[3];

    if (!get || !set) return;
    get( v, now, ARRAY_SIZE(now) );
    if (index < ARRAY_SIZE(v)) v[index] = value;
    set( tray_window(), v, image ? image : now );
}

/* a picture for the Start button */
static BOOL browse_image( struct view *view, WCHAR *path, UINT count )
{
    static const WCHAR masks[] = L"*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.ico";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR filter[256];
    int len;

    lstrcpynW( filter, load_string( IDS_IMAGES ), 64 );
    len = lstrlenW( filter ) + 1;
    lstrcpyW( filter + len, masks );
    filter[len + lstrlenW( masks ) + 1] = 0;
    ofn.hwndOwner = GetAncestor( view_window( view ), GA_ROOT );
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = count;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW( &ofn );
}

/**********************************************************************
 *          Start
 */

static void start_build( struct view *view )
{
    int (WINAPI *get_style)(void) = startui( "ArcticStartStyleGet" );
    DWORD v[V_COUNT];
    int style;
    HWND control;

    begin_page( view, PAGE_START );
    if (!get_style || !tray_window())
    {
        no_taskbar( view );
        return;
    }
    style = get_style();
    control = card_combo( view, ICON_START, IDS_START_STYLE, 0, ID_START_STYLE );
    for (UINT i = 0; i < START_COUNT; i++)
        combo_add( control, load_string( start_names[start_order[i]] ), start_order[i], start_order[i] == style );
    card_button( view, ICON_ONTOP, IDS_START_CONTENT, load_string( IDS_START_CONTENT_HINT ), IDS_CUSTOMIZE, ID_CUSTOMIZE );

    view_section( view, load_string( IDS_MENU_LOOK ) );
    look_get( style, v );
    card_toggle( view, ICON_PRESET, IDS_INHERIT, IDS_INHERIT_HINT, v[V_INHERIT] != 0, ID_INHERIT );
    /* the menus of Windows 10 and 11 have a look of their own to choose */
    if (!v[V_INHERIT] && (style == START_WIN10 || style == START_WIN11)) look_cards( view, v, TRUE );
    related( view, PAGE_START );
}

static void start_customize( struct view *view )
{
    int (WINAPI *get_style)(void) = startui( "ArcticStartStyleGet" );
    void (WINAPI *customize)( HWND, int ) = startui( "ArcticStartCustomize" );
    HWND owner = GetAncestor( view_window( view ), GA_ROOT );
    int style = get_style ? get_style() : START_STANDARD;

    /* the classic menu's is explorer's */
    if (style == START_CLASSIC) tray_call( 2, (LPARAM)owner, NULL );
    else if (customize) customize( owner, style );
}

/**********************************************************************
 *          The notification area
 */

static void notify_build( struct view *view )
{
    DWORD bits;
    HWND control;

    begin_page( view, PAGE_NOTIFY );
    if (!tray_get( &bits ))
    {
        no_taskbar( view );
        return;
    }
    control = card_toggle( view, ICON_CLOCK, IDS_CLOCK, 0, !(bits & TRAY_NO_CLOCK), ID_CLOCK );
    EnableWindow( control, !(bits & TRAY_CLOCK_POLICY) );
    control = card_toggle( view, 0, IDS_SECONDS, 0, (bits & TRAY_SECONDS) != 0, ID_SECONDS );
    EnableWindow( control, !(bits & TRAY_NO_CLOCK) );
    card_toggle( view, ICON_DESKTOP, IDS_DESKTOP_BUTTON, IDS_DESKTOP_HINT, (bits & TRAY_DESKTOP) != 0, ID_DESKTOP );
    related( view, PAGE_NOTIFY );
}

/**********************************************************************
 *          What the controls change
 */

static BOOL is_slider_end( UINT code )
{
    /* while the thumb is dragged it is told on its release */
    return code != TB_THUMBTRACK && code != TB_ENDTRACK;
}

static BOOL page_command( struct view *view, UINT id, UINT code, HWND control )
{
    HWND content = view_window( view );

    switch (id)
    {
    case ID_LOCK:
    case ID_AUTOHIDE:
    case ID_ONTOP:
    case ID_SMALL:
    case ID_CLOCK:
    case ID_SECONDS:
    case ID_DESKTOP:
        if (code != BN_CLICKED) return TRUE;
        taskbar_toggle( id, SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
        /* the seconds go with the clock */
        if (id == ID_CLOCK) defer( view, ID_DO_REBUILD );
        return TRUE;
    case ID_COMBINE:
    case ID_PRESET:
    case ID_EFFECT:
    case ID_TEXT:
    case ID_BUTTON_STYLE:
    case ID_ALIGNMENT:
    case ID_START_KIND:
    case ID_WIDGETS:
    case ID_START_STYLE:
        if (code == CBN_SELCHANGE) defer( view, ID_DO_COMBO + id - 100 );
        return TRUE;
    case ID_COLOR:
        if (code == BN_CLICKED) defer( view, ID_DO_COMBO + id - 100 );
        return TRUE;
    case ID_OPACITY:
    case ID_BLUR:
        if (is_slider_end( code ))
            look_changed( view, id, (DWORD)SendMessageW( control, TBM_GETPOS, 0, 0 ) );
        if (code == TB_THUMBPOSITION || code == TB_ENDTRACK) defer( view, ID_DO_REBUILD );
        return TRUE;
    case ID_INHERIT:
        if (code != BN_CLICKED) return TRUE;
        look_changed( view, id, SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
        defer( view, ID_DO_REBUILD );
        return TRUE;
    case ID_CUSTOMIZE:
        if (code == BN_CLICKED) start_customize( view );
        return TRUE;
    case ID_START_BROWSE:
        if (code == BN_CLICKED) defer( view, ID_DO_COMBO + id - 100 );
        return TRUE;

    case ID_DO_REBUILD:
        view_rebuild( view );
        return TRUE;
    }
    if (id < ID_DO_COMBO || id >= ID_DO_COMBO + 100) return FALSE;

    /* posted: the control's notification is over */
    id = id - ID_DO_COMBO + 100;
    {
        LPARAM value = id == ID_COLOR || id == ID_START_BROWSE ? 0 : combo_data( GetDlgItem( content, id ) );

        switch (id)
        {
        case ID_COMBINE:
        {
            DWORD bits;

            if (value != -1 && tray_get( &bits ))
                tray_set( (bits & ~TRAY_GLOM_MASK) | ((DWORD)value << TRAY_GLOM_SHIFT & TRAY_GLOM_MASK) );
            break;
        }
        case ID_PRESET:
        case ID_EFFECT:
        case ID_TEXT:
            if (value != -1) look_changed( view, id, (DWORD)value );
            break;
        case ID_COLOR:
            choose_color( view );
            break;
        case ID_BUTTON_STYLE:
            if (value != -1) buttons_change( 0, (DWORD)value, NULL );
            break;
        case ID_ALIGNMENT:
            if (value != -1) buttons_change( 1, (DWORD)value, NULL );
            break;
        case ID_WIDGETS:
            if (value != -1) buttons_change( 2, (DWORD)value, NULL );
            break;
        case ID_START_KIND:
        case ID_START_BROWSE:
        {
            WCHAR path[MAX_PATH] = L"";

            /* the standard button, or a picture chosen now */
            if (id == ID_START_KIND && value == 0) buttons_change( ~0u, 0, L"" );
            else if (value != -1 && browse_image( view, path, ARRAY_SIZE(path) )) buttons_change( ~0u, 0, path );
            break;
        }
        case ID_START_STYLE:
        {
            void (WINAPI *set_style)( int ) = startui( "ArcticStartStyleSet" );

            if (value != -1 && set_style) set_style( (int)value );
            break;
        }
        }
    }
    view_rebuild( view );
    return TRUE;
}

static BOOL taskbar_command( struct view *view, UINT id, UINT code, HWND control )
{
    return page_command( view, id, code, control );
}

const struct cp_page cp_pages[] =
{
    { IDS_NAME, L"pageTaskbar", taskbar_build, taskbar_command },
    { IDS_LOOK, L"pageAppearance", look_build, taskbar_command },
    { IDS_BUTTONS, L"pageButtons", buttons_build, taskbar_command },
    { IDS_START, L"pageStartMenu", start_build, taskbar_command },
    { IDS_NOTIFY, L"pageNotificationArea", notify_build, taskbar_command },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* the item in a new Explorer window, on one of its pages (the tray's menu) */
BOOL WINAPI ArcticTaskbarOpen( UINT page )
{
    return cp_open( page < PAGE_COUNT ? page : 0, 0 );
}

/* rundll32 taskbarcpl.dll,ShowTaskbarProperties */
void WINAPI ShowTaskbarProperties( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    cp_open( 0, 0 );
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    return cp_class_object( clsid, riid, out );
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return cp_can_unload();
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        cp_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
