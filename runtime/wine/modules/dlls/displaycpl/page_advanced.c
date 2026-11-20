/*
 * Display of the Control Panel: the page Advanced display
 *
 * Windows 11's Advanced display: which monitor, what it shows (its own name,
 * the card, the desktop's resolution and the signal's, the refresh rate, the
 * colour), the refresh rate to choose, variable refresh where the monitor
 * and the card take it, and the colour depth desk.cpl's Settings page had;
 * the adapter's own properties are its dialog, as in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "displaycpl.h"

#define ID_MONITOR      100
#define ID_REFRESH      101
#define ID_VRR          102
#define ID_DEPTH        103

#define ID_CUSTOM_RATE  104
#define ID_CUSTOM_TEST  105
#define ID_CUSTOM_ON    106
#define ID_CUSTOM_BLANK 107
#define ID_CUSTOM_DEL   110   /* one per rate kept */

#define ID_DO_MONITOR   200
#define ID_DO_REFRESH   201
#define ID_DO_DEPTH     202
#define ID_DO_REBUILD   203
#define ID_DO_TEST      204
#define ID_DO_DEL       210   /* one per rate kept */

static struct monitor *page_monitor( struct view *view, struct display_state *s )
{
    UINT index = view_state( view )->param;

    if (!s->displays.count) return NULL;
    return &s->displays.list[index < s->displays.count ? index : 0];
}

static DWORD orientation_of( const DEVMODEW *dm )
{
    return (dm->dmFields & DM_DISPLAYORIENTATION) ? dm->dmDisplayOrientation : DMDO_DEFAULT;
}

static void adapter_properties( struct view *view, UINT_PTR param )
{
    struct display_state *s = display_state( view );
    struct monitor *m = s ? page_monitor( view, s ) : NULL;
    HMODULE desk = LoadLibraryW( L"desk.cpl" );
    BOOL (WINAPI *properties)( HWND, const WCHAR * ) = desk ? (void *)GetProcAddress( desk, "ArcticDisplayAdapterProperties" ) : NULL;

    if (!m || !properties) return;
    properties( GetAncestor( view_window( view ), GA_ROOT ), m->device );
    /* the dialog's Monitor page may have changed the mode */
    defer_command( view, ID_DO_REBUILD );
}

/* the monitor's card: its name, where it is connected, then label and value */
static void paint_info( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    struct display_state *s = (struct display_state *)param;
    int y = rect->top, line = px( 24 ), column = rect->left + max( px( 220 ), (rect->right - rect->left) * 2 / 5 );
    RECT r;
    HGDIOBJ old = SelectObject( hdc, view_font( STYLE_BOLD ) );

    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, COLOR_TEXT );
    SetRect( &r, rect->left, y, rect->right, y + px( 20 ) );
    DrawTextW( hdc, s->info.name, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    y += px( 22 );
    SelectObject( hdc, view_font( STYLE_BODY ) );
    SetTextColor( hdc, COLOR_GRAY );
    SetRect( &r, rect->left, y, rect->right, y + px( 18 ) );
    DrawTextW( hdc, s->info.connected, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    y += px( 32 );
    for (UINT i = 0; i < s->info.count; i++, y += line)
    {
        SetTextColor( hdc, COLOR_GRAY );
        SetRect( &r, rect->left, y, column - px( 12 ), y + line );
        DrawTextW( hdc, s->info.label[i], -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
        SetTextColor( hdc, COLOR_TEXT );
        SetRect( &r, column, y, rect->right, y + line );
        DrawTextW( hdc, s->info.value[i], -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    }
    SelectObject( hdc, old );
}

static void info_pair( struct display_state *s, UINT label, const WCHAR *value )
{
    UINT i = s->info.count;

    if (i >= ARRAY_SIZE(s->info.label)) return;
    lstrcpynW( s->info.label[i], load_string( label ), ARRAY_SIZE(s->info.label[i]) );
    lstrcpynW( s->info.value[i], value, ARRAY_SIZE(s->info.value[i]) );
    s->info.count++;
}

static BOOL rate_in( const DWORD *rates, UINT count, DWORD hz )
{
    for (UINT i = 0; i < count; i++) if (rates[i] == hz) return TRUE;
    return FALSE;
}

/* Refresh rates above the monitor's own: one to try, with the question
 * after it as for any mode (dwm.exe takes it back on its own besides); the
 * ones kept, each to remove; whether they are offered at all. They show
 * among the monitor's modes at every size the card scales up, to games too. */
/* what was last tried: the page is built anew after each try */
static DWORD last_hz, last_blank;

static void custom_build( struct view *view, struct monitor *m, const DWORD *rates, UINT rate_count )
{
    DWORD kept[CUSTOM_MAX], kept_hz[CUSTOM_MAX], own = 0;
    UINT kept_count = monitor_custom_rates( m, kept, ARRAY_SIZE(kept) ), first, last, pick;
    WCHAR name[128];
    HWND control;

    for (UINT i = 0; i < kept_count; i++) kept_hz[i] = CUSTOM_HZ( kept[i] );
    /* the fastest rate the monitor has of its own */
    for (UINT i = 0; i < rate_count; i++) if (!rate_in( kept_hz, kept_count, rates[i] )) own = max( own, rates[i] );
    if (!own) own = m->current.dmDisplayFrequency;
    if (own < 24) return;
    first = own + 1;
    last = own * 3 / 2;
    pick = last_hz >= first && last_hz <= last ? last_hz : min( own * 5 / 4, own + 15 );

    view_section( view, load_string( IDS_CUSTOM_GROUP ) );
    control = card_combo( view, ICON_RESOLUTION, IDS_CUSTOM_TRY, IDS_CUSTOM_TRY_HINT, ID_CUSTOM_RATE );
    for (UINT hz = first; hz <= last; hz++)
        if (!rate_in( kept_hz, kept_count, hz )) combo_add( control, format_string( IDS_HZ, hz ), hz, hz == pick );
    EnableWindow( control, m->attached && kept_count < CUSTOM_MAX );
    control = view_card_extra( view, L"BUTTON", load_string( IDS_CUSTOM_TEST ), BS_PUSHBUTTON | WS_TABSTOP, px( 110 ),
                               px( 28 ), ID_CUSTOM_TEST );
    EnableWindow( control, m->attached && kept_count < CUSTOM_MAX );
    /* less blank: a higher rate at the pixel clock the panel takes */
    control = card_combo( view, ICON_ADVANCED, IDS_CUSTOM_BLANK, IDS_CUSTOM_BLANK_HINT, ID_CUSTOM_BLANK );
    for (UINT b = 0; b < 3; b++) combo_add( control, load_string( IDS_BLANK_NATIVE + b ), b, b == last_blank );
    EnableWindow( control, m->attached && kept_count < CUSTOM_MAX );

    for (UINT i = 0; i < kept_count; i++)
    {
        lstrcpynW( name, format_string( IDS_HZ, kept_hz[i] ), ARRAY_SIZE(name) );
        view_card( view, view_icon( ICON_RESOLUTION ), name, load_string( IDS_BLANK_NATIVE + min( CUSTOM_BLANK( kept[i] ), 2 ) ),
                   L"BUTTON", load_string( IDS_CUSTOM_REMOVE ), BS_PUSHBUTTON | WS_TABSTOP, px( 110 ), px( 28 ),
                   ID_CUSTOM_DEL + i );
    }
    if (kept_count)
    {
        lstrcpynW( name, load_string( IDS_CUSTOM_ON ), ARRAY_SIZE(name) );
        view_card_toggle( view, view_icon( ICON_GRAPHICS ), name, load_string( IDS_CUSTOM_ON_HINT ),
                          monitor_custom_on( m ), ID_CUSTOM_ON );
    }
}

/* the rate shows among the monitor's modes once dwm.exe made it */
static BOOL wait_for_rate( const struct monitor *m, DWORD hz )
{
    for (int tries = 0; tries < 40; tries++)
    {
        DEVMODEW dm = { .dmSize = sizeof(dm) };

        for (DWORD i = 0; EnumDisplaySettingsExW( m->device, i, &dm, 0 ); i++)
        {
            if (dm.dmPelsWidth == m->current.dmPelsWidth && dm.dmPelsHeight == m->current.dmPelsHeight &&
                dm.dmDisplayFrequency == hz)
                return TRUE;
            memset( &dm, 0, sizeof(dm) );
            dm.dmSize = sizeof(dm);
        }
        Sleep( 100 );
    }
    return FALSE;
}

/* the rate on trial, then the question: kept, it joins the monitor's own;
 * else the monitor goes back to what it showed */
static void try_rate( struct view *view, struct display_state *s, struct monitor *m, DWORD hz, DWORD blank )
{
    HWND owner = GetAncestor( view_window( view ), GA_ROOT );
    DWORD kept[CUSTOM_MAX], rate = CUSTOM_RATE( hz, blank );
    UINT kept_count;
    struct snapshot before;
    DEVMODEW dm;
    LONG ret = DISP_CHANGE_FAILED;
    BOOL listed = FALSE;

    last_hz = hz;
    last_blank = blank;
    snapshot_take( &before, &s->displays );
    monitor_custom_trial( m, rate );
    if (wait_for_rate( m, hz ))
    {
        dm = m->current;
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        dm.dmDisplayFrequency = hz;
        ret = monitor_set_mode( m, &dm, m->primary, TRUE );
    }
    if (ret != DISP_CHANGE_SUCCESSFUL)
    {
        monitor_custom_trial( m, 0 );
        MessageBoxW( owner, format_string( IDS_CUSTOM_FAILED, hz ), load_string( IDS_DISPLAY ), MB_OK | MB_ICONERROR );
        return;
    }
    if (!keep_settings( view_window( view ) ))
    {
        snapshot_restore( &before );
        monitor_custom_trial( m, 0 );
        return;
    }
    /* kept first, then the trial ends: the mode never goes away meanwhile */
    kept_count = monitor_custom_rates( m, kept, ARRAY_SIZE(kept) );
    for (UINT i = 0; i < kept_count; i++) if (CUSTOM_HZ( kept[i] ) == hz) listed = TRUE;
    if (!listed && kept_count < CUSTOM_MAX) kept[kept_count++] = rate;
    monitor_custom_set_rates( m, kept, kept_count );
    monitor_custom_set_on( m, TRUE );
    monitor_custom_trial( m, 0 );
}

static void remove_rate( struct monitor *m, UINT index )
{
    DWORD kept[CUSTOM_MAX];
    UINT count = monitor_custom_rates( m, kept, ARRAY_SIZE(kept) );

    if (index >= count) return;
    memmove( kept + index, kept + index + 1, (count - index - 1) * sizeof(*kept) );
    /* a monitor showing it goes back to its own rate (dwm.exe) */
    monitor_custom_set_rates( m, kept, count - 1 );
}

void advanced_build( struct view *view )
{
    struct display_state *s = display_state( view );
    struct monitor *m;
    WCHAR text[256], name[256];
    HWND combo;
    DWORD rates[64], depths[8];
    UINT rate_count = 0, depth_count = 0;
    DWORD orientation;

    if (!s) return;
    displays_load( &s->displays );
    nav_display( view, PAGE_ADVANCED );
    view_heading( view, load_string( IDS_ADVANCED ) );
    if (!(m = page_monitor( view, s ))) return;

    if (s->displays.count >= 2)
    {
        combo = card_combo( view, ICON_DISPLAY, IDS_SELECT_DISPLAY, 0, ID_MONITOR );
        for (UINT i = 0; i < s->displays.count; i++)
        {
            const struct monitor *other = &s->displays.list[i];

            combo_add( combo, format_string( IDS_DISPLAY_N, other->number, other->name[0] ? other->name : other->adapter ),
                       i, other == m );
        }
    }

    view_section( view, load_string( IDS_INFO_GROUP ) );
    lstrcpynW( s->info.name, m->name[0] ? m->name : format_string( IDS_DISPLAY_N, m->number, m->adapter ),
               ARRAY_SIZE(s->info.name) );
    lstrcpynW( s->info.connected, format_string( IDS_CONNECTED_TO, m->number, m->adapter ), ARRAY_SIZE(s->info.connected) );
    s->info.count = 0;
    lstrcpynW( text, format_string( IDS_RES_FORMAT, m->current.dmPelsWidth, m->current.dmPelsHeight ), ARRAY_SIZE(text) );
    info_pair( s, IDS_DESKTOP_RES, text );
    info_pair( s, IDS_SIGNAL_RES, text );
    info_pair( s, IDS_REFRESH_RATE, format_string( IDS_HZ, m->current.dmDisplayFrequency ) );
    info_pair( s, IDS_BIT_DEPTH, format_string( IDS_BITS, m->current.dmBitsPerPel ) );
    info_pair( s, IDS_COLOR_FORMAT, load_string( IDS_RGB ) );
    info_pair( s, IDS_COLOR_SPACE, load_string( IDS_SDR ) );
    view_card_area( view, px( 54 ) + s->info.count * px( 24 ), paint_info, (UINT_PTR)s );
    lstrcpynW( name, format_string( IDS_ADAPTER_PROPS, m->number ), ARRAY_SIZE(name) );
    view_card_link( view, view_icon( ICON_ADVANCED ), name, NULL, adapter_properties, 0 );

    /* the rates and depths the monitor has at the size it shows */
    orientation = orientation_of( &m->current );
    for (UINT i = 0; i < m->mode_count; i++)
    {
        const struct mode *mode = &m->modes[i];
        UINT k;

        if (mode->width != m->current.dmPelsWidth || mode->height != m->current.dmPelsHeight ||
            mode->orientation != orientation)
            continue;
        if (mode->bpp == m->current.dmBitsPerPel && mode->freq > 1)
        {
            for (k = 0; k < rate_count && rates[k] != mode->freq; k++) ;
            if (k == rate_count && rate_count < ARRAY_SIZE(rates))
            {
                for (k = rate_count; k > 0 && rates[k - 1] > mode->freq; k--) rates[k] = rates[k - 1];
                rates[k] = mode->freq;
                rate_count++;
            }
        }
        for (k = 0; k < depth_count && depths[k] != mode->bpp; k++) ;
        if (k == depth_count && depth_count < ARRAY_SIZE(depths))
        {
            for (k = depth_count; k > 0 && depths[k - 1] < mode->bpp; k--) depths[k] = depths[k - 1];
            depths[k] = mode->bpp;
            depth_count++;
        }
    }

    view_section( view, load_string( IDS_REFRESH_RATE ) );
    combo = card_combo( view, ICON_RESOLUTION, IDS_REFRESH_GROUP, IDS_REFRESH_HINT, ID_REFRESH );
    for (UINT i = 0; i < rate_count; i++)
        combo_add( combo, format_string( IDS_HZ, rates[i] ), rates[i], rates[i] == m->current.dmDisplayFrequency );
    if (!rate_count) combo_add( combo, format_string( IDS_HZ, m->current.dmDisplayFrequency ), m->current.dmDisplayFrequency, TRUE );
    EnableWindow( combo, m->attached && rate_count > 1 );
    /* only where the monitor and the card take it (dwm.exe found out) */
    if (m->vrr_capable)
    {
        lstrcpynW( name, load_string( IDS_VRR ), ARRAY_SIZE(name) );
        view_card_toggle( view, view_icon( ICON_GRAPHICS ), name, load_string( IDS_VRR_HINT ), m->vrr, ID_VRR );
    }
    /* kept in the monitor's own key: one with an EDID */
    if (m->pnp[0] && m->attached) custom_build( view, m, rates, rate_count );

    view_section( view, load_string( IDS_DEPTH_GROUP ) );
    combo = card_combo( view, ICON_COLORS, IDS_BIT_DEPTH, 0, ID_DEPTH );
    for (UINT i = 0; i < depth_count; i++)
        combo_add( combo, format_string( IDS_BITS, depths[i] ), depths[i], depths[i] == m->current.dmBitsPerPel );
    if (!depth_count) combo_add( combo, format_string( IDS_BITS, m->current.dmBitsPerPel ), m->current.dmBitsPerPel, TRUE );
    EnableWindow( combo, m->attached && depth_count > 1 );
}

/* the mode at once, kept if the user says so within 15 seconds */
static void apply( struct view *view, struct display_state *s, const struct mode *mode )
{
    struct monitor *m = page_monitor( view, s );
    struct snapshot before;
    DEVMODEW dm;
    LONG ret;

    if (!m || !mode) return;
    snapshot_take( &before, &s->displays );
    mode_to_devmode( mode, &dm );
    ret = monitor_set_mode( m, &dm, m->primary, TRUE );
    if (ret != DISP_CHANGE_SUCCESSFUL) show_failure( view_window( view ), ret );
    else if (!keep_settings( view_window( view ) )) snapshot_restore( &before );
}

BOOL advanced_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct display_state *s = display_state( view );
    HWND content = view_window( view );
    struct monitor *m;

    if (!s) return FALSE;
    switch (id)
    {
    case ID_MONITOR:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_MONITOR );
        return TRUE;
    case ID_REFRESH:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_REFRESH );
        return TRUE;
    case ID_DEPTH:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_DEPTH );
        return TRUE;
    case ID_VRR:
        if (code == BN_CLICKED && (m = page_monitor( view, s )))
        {
            m->vrr = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
            monitor_write_vrr( m );
        }
        return TRUE;
    case ID_DO_MONITOR:
    {
        LPARAM index = combo_data( GetDlgItem( content, ID_MONITOR ) );

        if (index != -1) view_navigate( view, PAGE_ADVANCED, (DWORD)index );
        return TRUE;
    }
    case ID_DO_REFRESH:
    {
        LPARAM rate = combo_data( GetDlgItem( content, ID_REFRESH ) );

        if (rate != -1 && (m = page_monitor( view, s )))
            apply( view, s, monitor_best_mode( m, m->current.dmPelsWidth, m->current.dmPelsHeight,
                                               m->current.dmBitsPerPel, (DWORD)rate, orientation_of( &m->current ) ) );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_DEPTH:
    {
        LPARAM depth = combo_data( GetDlgItem( content, ID_DEPTH ) );

        if (depth != -1 && (m = page_monitor( view, s )))
            apply( view, s, monitor_best_mode( m, m->current.dmPelsWidth, m->current.dmPelsHeight, (DWORD)depth,
                                               m->current.dmDisplayFrequency, orientation_of( &m->current ) ) );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_REBUILD:
        view_rebuild( view );
        return TRUE;
    case ID_CUSTOM_TEST:
        if (code == BN_CLICKED) defer_command( view, ID_DO_TEST );
        return TRUE;
    case ID_CUSTOM_ON:
        if (code == BN_CLICKED && (m = page_monitor( view, s )))
            monitor_custom_set_on( m, SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
        return TRUE;
    case ID_DO_TEST:
    {
        LPARAM rate = combo_data( GetDlgItem( content, ID_CUSTOM_RATE ) );
        LPARAM blank = combo_data( GetDlgItem( content, ID_CUSTOM_BLANK ) );

        if (rate > 0 && (m = page_monitor( view, s )))
            try_rate( view, s, m, (DWORD)rate, blank >= 0 && blank <= 2 ? (DWORD)blank : 0 );
        view_rebuild( view );
        return TRUE;
    }
    }
    if (id >= ID_CUSTOM_DEL && id < ID_CUSTOM_DEL + CUSTOM_MAX)
    {
        if (code == BN_CLICKED) defer_command( view, ID_DO_DEL + id - ID_CUSTOM_DEL );
        return TRUE;
    }
    if (id >= ID_DO_DEL && id < ID_DO_DEL + CUSTOM_MAX)
    {
        if ((m = page_monitor( view, s ))) remove_rate( m, id - ID_DO_DEL );
        /* dwm.exe takes the rate out of the modes first */
        Sleep( 300 );
        view_rebuild( view );
        return TRUE;
    }
    return FALSE;
}
