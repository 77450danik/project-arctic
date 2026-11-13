/*
 * Display of the Control Panel: the page Display
 *
 * Windows 11's Settings, System, Display, as a page of the Control Panel:
 * the arrangement of the monitors with Identify; the brightness of a panel;
 * Scale and layout (the scale, the resolution, the orientation, and what
 * desk.cpl's Settings page had besides: how smaller modes show on the
 * monitor); Multiple displays (extend, one only, the main display, detect);
 * Advanced display and Graphics. A mode applies at once and is taken back
 * unless kept within 15 seconds, as there. The size of the desktop's icons
 * is the desktop's (page_desktop.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "displaycpl.h"
#include "wine/arctic_power.h"

/* powrprof.h wants NTSTATUS: the one call of it here */
DWORD WINAPI PowerWriteACValueIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD index );

#define ID_LAYOUT         100
#define ID_IDENTIFY       101
#define ID_APPLY_LAYOUT   102
#define ID_CANCEL_LAYOUT  103
#define ID_BRIGHTNESS     110
#define ID_SCALE          111
#define ID_RESOLUTION     112
#define ID_ORIENTATION    113
#define ID_SCALING        114
#define ID_MULTI          116
#define ID_MAIN           117
#define ID_DETECT         118

/* done after the control's notification is over (defer_command) */
#define ID_DO_REBUILD     200
#define ID_DO_SCALE       201
#define ID_DO_RESOLUTION  202
#define ID_DO_ORIENTATION 203
#define ID_DO_MULTI       204
#define ID_DO_MAIN        205
#define ID_DO_LAYOUT      206

#define MULTI_EXTEND      0xffff

/* the controls at a card's right */
#define COMBO_WIDTH       px( 260 )
#define LAYOUT_HEIGHT     px( 190 )

static const GUID sub_video = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
static const GUID set_brightness = { 0xaded5e82, 0xb909, 0x4619, { 0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb } };

struct display_state *display_state( struct view *view )
{
    struct page_state *state = view_state( view );

    if (!state->data) state->data = calloc( 1, sizeof(struct display_state) );
    return state->data;
}

void defer_command( struct view *view, UINT id )
{
    HWND content = view_window( view );

    PostMessageW( content, WM_COMMAND, MAKEWPARAM( id, 0 ), (LPARAM)content );
}

void go_page( struct view *view, UINT_PTR page )
{
    view_navigate( view, (UINT)page, 0 );
}

void go_item( struct view *view, UINT_PTR item )
{
    view_browse_item( view, (const WCHAR *)item );
}

void nav_display( struct view *view, UINT current )
{
    static const UINT pages[] = { PAGE_DISPLAY, PAGE_PERSONALIZE, PAGE_BACKGROUND, PAGE_DESKTOP, PAGE_SCREENSAVER };
    static const UINT names[] = { IDS_DISPLAY, IDS_PERSONALIZE, IDS_BACKGROUND, IDS_DESKTOP, IDS_SCREENSAVER };

    view_nav_home( view );
    for (UINT i = 0; i < ARRAY_SIZE(pages); i++)
        if (pages[i] != current) view_nav_link( view, load_string( names[i] ), go_page, pages[i] );
    view_nav_see_also( view, load_string( IDS_TASKBAR ), go_item, (UINT_PTR)ITEM_TASKBAR );
    view_nav_see_also( view, load_string( IDS_POWER ), go_item, (UINT_PTR)ITEM_POWER );
}

void combo_add( HWND combo, const WCHAR *text, LPARAM data, BOOL current )
{
    LRESULT index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );

    SendMessageW( combo, CB_SETITEMDATA, index, data );
    if (current) SendMessageW( combo, CB_SETCURSEL, index, 0 );
}

LPARAM combo_data( HWND combo )
{
    LRESULT index = SendMessageW( combo, CB_GETCURSEL, 0, 0 );

    return index == CB_ERR ? -1 : SendMessageW( combo, CB_GETITEMDATA, index, 0 );
}

/* a card with its icon, its name, a line under it, and a list to choose from */
HWND card_combo( struct view *view, UINT icon, UINT title, UINT hint, UINT id )
{
    WCHAR name[256];

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    return view_card( view, view_icon( icon ), name, hint ? load_string( hint ) : NULL, WC_COMBOBOXW, L"",
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, COMBO_WIDTH, px( 300 ), id );
}

/* the monitor whose settings the page shows: the one chosen, else the main one */
static struct monitor *chosen( struct display_state *s )
{
    struct monitor *m = s->selected[0] ? displays_find( &s->displays, s->selected ) : NULL;

    if (!m)
        for (UINT i = 0; i < s->displays.count && !m; i++)
            if (s->displays.list[i].attached && s->displays.list[i].primary) m = &s->displays.list[i];
    if (!m)
        for (UINT i = 0; i < s->displays.count && !m; i++)
            if (s->displays.list[i].attached) m = &s->displays.list[i];
    if (!m && s->displays.count) m = &s->displays.list[0];
    if (m) lstrcpynW( s->selected, m->device, ARRAY_SIZE(s->selected) );
    return m;
}

static DWORD orientation_of( const DEVMODEW *dm )
{
    return (dm->dmFields & DM_DISPLAYORIENTATION) ? dm->dmDisplayOrientation : DMDO_DEFAULT;
}

/**********************************************************************
 *          The controls
 */

static void fill_scale( HWND combo, const struct monitor *m )
{
    int min_rel, cur_rel, max_rel, rec;
    WCHAR text[64];

    if (!m->attached || !scale_get( m->device, &min_rel, &cur_rel, &max_rel ))
    {
        combo_add( combo, format_string( IDS_PERCENT, 100 ), 0, TRUE );
        EnableWindow( combo, FALSE );
        return;
    }
    rec = -min_rel;
    for (int rel = min_rel; rel <= max_rel; rel++)
    {
        if (rec + rel < 0 || rec + rel >= (int)ARRAY_SIZE(scale_steps)) continue;
        lstrcpynW( text, format_string( IDS_PERCENT, scale_steps[rec + rel] ), ARRAY_SIZE(text) );
        if (!rel) lstrcpynW( text, format_string( IDS_RECOMMENDED, text ), ARRAY_SIZE(text) );
        combo_add( combo, text, rel, rel == cur_rel );
    }
    EnableWindow( combo, max_rel > min_rel );
}

/* the sizes of the monitor's modes in the orientation it has, largest first */
static void fill_resolution( HWND combo, const struct monitor *m )
{
    DWORD orientation = orientation_of( &m->current ), sizes[256][2];
    UINT count = 0;

    for (UINT i = 0; i < m->mode_count && count < ARRAY_SIZE(sizes); i++)
    {
        const struct mode *mode = &m->modes[i];
        UINT k;

        if (mode->orientation != orientation) continue;
        for (k = 0; k < count; k++) if (sizes[k][0] == mode->width && sizes[k][1] == mode->height) break;
        if (k < count) continue;
        /* largest first */
        for (k = count; k > 0 && (ULONGLONG)sizes[k - 1][0] * sizes[k - 1][1] < (ULONGLONG)mode->width * mode->height; k--)
        {
            sizes[k][0] = sizes[k - 1][0];
            sizes[k][1] = sizes[k - 1][1];
        }
        sizes[k][0] = mode->width;
        sizes[k][1] = mode->height;
        count++;
    }
    for (UINT i = 0; i < count; i++)
    {
        WCHAR text[64];

        lstrcpynW( text, format_string( IDS_RES_FORMAT, sizes[i][0], sizes[i][1] ), ARRAY_SIZE(text) );
        /* the monitor's own, its largest, as Windows recommends it */
        if (!i) lstrcpynW( text, format_string( IDS_RECOMMENDED, text ), ARRAY_SIZE(text) );
        combo_add( combo, text, (sizes[i][0] << 16) | sizes[i][1],
                   sizes[i][0] == m->current.dmPelsWidth && sizes[i][1] == m->current.dmPelsHeight );
    }
    EnableWindow( combo, m->attached && count > 1 );
}

static void fill_orientation( HWND combo, const struct monitor *m )
{
    static const UINT names[4] = { IDS_LANDSCAPE, IDS_PORTRAIT, IDS_LANDSCAPE_FLIPPED, IDS_PORTRAIT_FLIPPED };
    DWORD current = orientation_of( &m->current );
    BOOL present[4] = { FALSE };
    UINT count = 0;

    present[current & 3] = TRUE;
    for (UINT i = 0; i < m->mode_count; i++) present[m->modes[i].orientation & 3] = TRUE;
    for (UINT o = 0; o < 4; o++)
    {
        if (!present[o]) continue;
        combo_add( combo, load_string( names[o] ), o, o == current );
        count++;
    }
    EnableWindow( combo, m->attached && count > 1 );
}

static void fill_scaling( HWND combo, const struct monitor *m )
{
    combo_add( combo, load_string( IDS_SCALING_ASPECT ), SCALING_ASPECT, m->scaling == SCALING_ASPECT );
    combo_add( combo, load_string( IDS_SCALING_STRETCH ), SCALING_STRETCH, m->scaling == SCALING_STRETCH );
    combo_add( combo, load_string( IDS_SCALING_CENTER ), SCALING_CENTER, m->scaling == SCALING_CENTER );
    /* kept per monitor model: a monitor without an EDID has nowhere to keep it */
    EnableWindow( combo, m->pnp[0] != 0 );
}

static void fill_multi( HWND combo, const struct display_state *s )
{
    UINT attached = displays_attached( &s->displays ), only = 0;

    for (UINT i = 0; i < s->displays.count; i++) if (s->displays.list[i].attached) only = i;
    combo_add( combo, load_string( IDS_EXTEND ), MULTI_EXTEND, attached == s->displays.count );
    for (UINT i = 0; i < s->displays.count; i++)
        combo_add( combo, format_string( IDS_ONLY_ON, s->displays.list[i].number ), i, attached == 1 && only == i );
}

/**********************************************************************
 *          The page
 */

static void advanced_link( struct view *view, UINT_PTR param )
{
    view_navigate( view, PAGE_ADVANCED, (DWORD)param );
}

static void graphics_link( struct view *view, UINT_PTR param )
{
    HMODULE power = LoadLibraryW( L"powercpl.dll" );
    BOOL (WINAPI *open)( const WCHAR * ) = power ? (void *)GetProcAddress( power, "ArcticOpenPowerOptions" ) : NULL;

    if (open) open( L"graphics" );
}

static UINT index_of( const struct display_state *s, const struct monitor *m )
{
    return m ? (UINT)(m - s->displays.list) : 0;
}

/* the monitors as they stand, Identify under them; Apply and Cancel once dragged */
static void arrangement( struct view *view, struct display_state *s, struct monitor *m )
{
    int button = px( 28 ), y = LAYOUT_HEIGHT + px( 12 );
    HWND control;

    layout_register();
    view_card_area( view, y + button, NULL, 0 );
    control = view_card_control( view, LAYOUT_CLASS, L"", WS_TABSTOP, 0, 0, 0, LAYOUT_HEIGHT, ID_LAYOUT );
    SendMessageW( control, LM_SETDISPLAYS, index_of( s, m ), (LPARAM)&s->displays );
    view_card_control( view, L"BUTTON", load_string( IDS_IDENTIFY ), BS_PUSHBUTTON | WS_TABSTOP, 0, y, px( 130 ), button,
                       ID_IDENTIFY );
    if (!s->moved) return;
    view_card_control( view, L"BUTTON", load_string( IDS_APPLY_LAYOUT ), BS_DEFPUSHBUTTON | WS_TABSTOP, px( 140 ), y,
                       px( 120 ), button, ID_APPLY_LAYOUT );
    view_card_control( view, L"BUTTON", load_string( IDS_CANCEL_LAYOUT ), BS_PUSHBUTTON | WS_TABSTOP, px( 270 ), y,
                       px( 120 ), button, ID_CANCEL_LAYOUT );
}

void display_build( struct view *view )
{
    struct display_state *s = display_state( view );
    struct arctic_cpu_info cpu;
    struct monitor *m;
    HWND control;

    if (!s) return;
    /* the places dragged and not applied stay; anything else as it is now */
    if (!s->moved) displays_load( &s->displays );
    m = chosen( s );

    nav_display( view, PAGE_DISPLAY );
    view_heading( view, load_string( IDS_DISPLAY ) );
    if (!m)
    {
        view_card( view, view_icon( ICON_DISPLAY ), load_string( IDS_FAILED ), NULL, NULL, NULL, 0, 0, 0, 0 );
        return;
    }

    /* the arrangement, with more than one monitor */
    if (s->displays.count >= 2) arrangement( view, s, m );

    /* Brightness and colour: the panel of a laptop */
    if (ArcticCpuInfo( &cpu ) && cpu.backlight)
    {
        WCHAR name[128];

        view_section( view, load_string( IDS_BRIGHTNESS_GROUP ) );
        lstrcpynW( name, load_string( IDS_BRIGHTNESS ), ARRAY_SIZE(name) );
        control = view_card( view, view_icon( ICON_BRIGHTNESS ), name, load_string( IDS_BRIGHTNESS_HINT ), TRACKBAR_CLASSW,
                             L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, COMBO_WIDTH, px( 26 ), ID_BRIGHTNESS );
        SendMessageW( control, TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
        SendMessageW( control, TBM_SETPAGESIZE, 0, 10 );
        SendMessageW( control, TBM_SETPOS, TRUE, cpu.brightness_percent );
    }

    view_section( view, load_string( IDS_SCALE_GROUP ) );
    fill_scale( card_combo( view, ICON_SCALE, IDS_SCALE, IDS_SCALE_HINT, ID_SCALE ), m );
    fill_resolution( card_combo( view, ICON_RESOLUTION, IDS_RESOLUTION, IDS_RESOLUTION_HINT, ID_RESOLUTION ), m );
    fill_orientation( card_combo( view, ICON_ORIENTATION, IDS_ORIENTATION, 0, ID_ORIENTATION ), m );
    fill_scaling( card_combo( view, ICON_SMALLER, IDS_SMALLER_MODES, IDS_SMALLER_HINT, ID_SCALING ), m );

    view_section( view, load_string( IDS_MULTI_GROUP ) );
    if (s->displays.count >= 2)
    {
        WCHAR name[128];

        fill_multi( card_combo( view, ICON_MULTI, IDS_MULTI_MODE, 0, ID_MULTI ), s );
        lstrcpynW( name, load_string( IDS_MAKE_MAIN ), ARRAY_SIZE(name) );
        control = view_card_toggle( view, view_icon( ICON_DISPLAY ), name, NULL, m->primary, ID_MAIN );
        EnableWindow( control, m->attached && !m->primary );
    }
    {
        WCHAR name[128];

        lstrcpynW( name, load_string( IDS_DETECT_OTHER ), ARRAY_SIZE(name) );
        view_card( view, view_icon( ICON_DETECT ), name, NULL, L"BUTTON", load_string( IDS_DETECT ),
                   BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_DETECT );
    }

    view_section( view, load_string( IDS_RELATED ) );
    {
        WCHAR name[128];

        lstrcpynW( name, load_string( IDS_ADVANCED_LINK ), ARRAY_SIZE(name) );
        view_card_link( view, view_icon( ICON_ADVANCED ), name, load_string( IDS_ADVANCED_HINT ), advanced_link,
                        index_of( s, m ) );
        lstrcpynW( name, load_string( IDS_GRAPHICS_LINK ), ARRAY_SIZE(name) );
        view_card_link( view, view_icon( ICON_GRAPHICS ), name, load_string( IDS_GRAPHICS_HINT ), graphics_link, 0 );
    }
}

/**********************************************************************
 *          What the controls change
 */

/* a mode of the chosen monitor, at once, kept if the user says so */
static void apply_mode( struct view *view, struct display_state *s, const struct mode *mode )
{
    struct snapshot before;
    struct monitor *m = chosen( s );
    DEVMODEW dm;
    LONG ret;

    if (!m || !mode) return;
    snapshot_take( &before, &s->displays );
    mode_to_devmode( mode, &dm );
    ret = monitor_set_mode( m, &dm, m->primary, TRUE );
    if (ret != DISP_CHANGE_SUCCESSFUL) show_failure( view_window( view ), ret );
    else if (!keep_settings( view_window( view ) )) snapshot_restore( &before );
}

/* the size and the orientation now, the depth and the rate as near as the monitor has them */
static void change_resolution( struct view *view, struct display_state *s, DWORD width, DWORD height )
{
    struct monitor *m = chosen( s );

    if (!m) return;
    apply_mode( view, s, monitor_best_mode( m, width, height, m->current.dmBitsPerPel, m->current.dmDisplayFrequency,
                                            orientation_of( &m->current ) ) );
}

static void change_orientation( struct view *view, struct display_state *s, DWORD orientation )
{
    struct monitor *m = chosen( s );
    DWORD current, width, height;
    const struct mode *mode;

    if (!m) return;
    current = orientation_of( &m->current );
    if (orientation == current) return;
    width = m->current.dmPelsWidth;
    height = m->current.dmPelsHeight;
    /* turned a quarter: the desktop is as wide as it was high */
    if ((orientation & 1) != (current & 1))
    {
        DWORD t = width;
        width = height;
        height = t;
    }
    if (!(mode = monitor_best_mode( m, width, height, m->current.dmBitsPerPel, m->current.dmDisplayFrequency, orientation )))
        mode = monitor_best_mode( m, ~0u, 0, m->current.dmBitsPerPel, m->current.dmDisplayFrequency, orientation );
    apply_mode( view, s, mode );
}

static void change_multi( struct view *view, struct display_state *s, LPARAM choice )
{
    struct snapshot before;
    LONG ret;

    snapshot_take( &before, &s->displays );
    if (choice == MULTI_EXTEND)
    {
        if (displays_attached( &s->displays ) == s->displays.count) return;
        ret = displays_extend( &s->displays );
    }
    else if (choice >= 0 && (UINT)choice < s->displays.count) ret = displays_only( &s->displays, (UINT)choice );
    else return;
    if (ret != DISP_CHANGE_SUCCESSFUL)
    {
        show_failure( view_window( view ), ret );
        snapshot_restore( &before );
    }
    else if (!keep_settings( view_window( view ) )) snapshot_restore( &before );
}

static void brightness( struct view *view, HWND slider, UINT code )
{
    struct arctic_power_apply apply;
    struct arctic_power_status status;
    DWORD level = SendMessageW( slider, TBM_GETPOS, 0, 0 );
    GUID *active = NULL;

    memset( &apply, 0xff, sizeof(apply) );
    apply.brightness_percent = level;
    ArcticPowerApply( &apply );
    /* the plan keeps it for this power source, as Windows' slider does */
    if (code != TB_ENDTRACK || PowerGetActiveScheme( NULL, &active ) || !active) return;
    if (!ArcticPowerStatus( &status ) || status.ac_online || !status.battery_count)
        PowerWriteACValueIndex( NULL, active, &sub_video, &set_brightness, level );
    else
        PowerWriteDCValueIndex( NULL, active, &sub_video, &set_brightness, level );
    PowerSetActiveScheme( NULL, active );
    LocalFree( active );
}

BOOL display_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct display_state *s = display_state( view );
    struct monitor *m;
    HWND content = view_window( view );

    if (!s) return FALSE;
    switch (id)
    {
    case ID_LAYOUT:
    {
        int index = (int)SendMessageW( control, LM_GETSELECTED, 0, 0 );

        if (index >= 0 && (UINT)index < s->displays.count)
            lstrcpynW( s->selected, s->displays.list[index].device, ARRAY_SIZE(s->selected) );
        if (code == LAYOUT_MOVED) s->moved = TRUE;
        defer_command( view, ID_DO_REBUILD );
        return TRUE;
    }
    case ID_IDENTIFY:
        if (code == BN_CLICKED) identify_monitors( &s->displays );
        return TRUE;
    case ID_APPLY_LAYOUT:
        if (code == BN_CLICKED) defer_command( view, ID_DO_LAYOUT );
        return TRUE;
    case ID_CANCEL_LAYOUT:
        if (code == BN_CLICKED)
        {
            s->moved = FALSE;
            defer_command( view, ID_DO_REBUILD );
        }
        return TRUE;
    case ID_BRIGHTNESS:
        if (code == TB_THUMBTRACK || code == TB_ENDTRACK || code == TB_LINEUP || code == TB_LINEDOWN ||
            code == TB_PAGEUP || code == TB_PAGEDOWN || code == TB_THUMBPOSITION || code == TB_TOP || code == TB_BOTTOM)
            brightness( view, control, code );
        return TRUE;
    case ID_SCALE:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_SCALE );
        return TRUE;
    case ID_RESOLUTION:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_RESOLUTION );
        return TRUE;
    case ID_ORIENTATION:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_ORIENTATION );
        return TRUE;
    case ID_SCALING:
        if (code == CBN_SELCHANGE && (m = chosen( s )) && combo_data( control ) >= 0)
        {
            m->scaling = (DWORD)combo_data( control );
            monitor_write_scaling( m );
        }
        return TRUE;
    case ID_MULTI:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_MULTI );
        return TRUE;
    case ID_MAIN:
        if (code == BN_CLICKED) defer_command( view, ID_DO_MAIN );
        return TRUE;
    case ID_DETECT:
        if (code == BN_CLICKED)
        {
            s->moved = FALSE;
            defer_command( view, ID_DO_REBUILD );
        }
        return TRUE;

    /* posted: the control's notification is over */
    case ID_DO_REBUILD:
        view_rebuild( view );
        return TRUE;
    case ID_DO_SCALE:
        if ((m = chosen( s )) && combo_data( GetDlgItem( content, ID_SCALE ) ) != -1)
            scale_set( m->device, (int)combo_data( GetDlgItem( content, ID_SCALE ) ) );
        view_rebuild( view );
        return TRUE;
    case ID_DO_RESOLUTION:
    {
        LPARAM size = combo_data( GetDlgItem( content, ID_RESOLUTION ) );

        if (size != -1) change_resolution( view, s, (DWORD)size >> 16, (DWORD)size & 0xffff );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_ORIENTATION:
    {
        LPARAM orientation = combo_data( GetDlgItem( content, ID_ORIENTATION ) );

        if (orientation != -1) change_orientation( view, s, (DWORD)orientation );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_MULTI:
    {
        LPARAM choice = combo_data( GetDlgItem( content, ID_MULTI ) );

        if (choice != -1) change_multi( view, s, choice );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_MAIN:
    {
        LONG ret;

        if (!(m = chosen( s )) || m->primary) return TRUE;
        ret = displays_set_primary( &s->displays, index_of( s, m ) );
        show_failure( content, ret );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_LAYOUT:
    {
        LONG ret = displays_set_layout( &s->displays );

        show_failure( content, ret );
        s->moved = FALSE;
        view_rebuild( view );
        return TRUE;
    }
    }
    return FALSE;
}
