/*
 * Sound of the Control Panel: the properties of a device
 *
 * Windows' sheet (mmsys.cpl's dialogs 121, 124 with its rows 141, 126 and
 * the balance 145 with its rows 144): "Загальні" with the name, the
 * controller and whether the device is used; "Рівні" with the level, the
 * mute and "Баланс", a level for each channel; "Додатково" with the
 * default format and "Перевірити". The levels apply as the slider moves,
 * as in Windows; the rest on OK.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "mmsys.h"
#include "prsht.h"
#include "shellapi.h"
#include "mmdeviceapi.h"
#include "uxtheme.h"

#define TIMER_LEVELS 1

struct props
{
    struct device dev;
    HWND row;                 /* the level's row on "Рівні" */
    HICON speaker, muted;
    struct format_choice formats[16];
    UINT format_count;
};

#define prop_changed( dlg ) SendMessageW( GetParent( dlg ), PSM_CHANGED, (WPARAM)(dlg), 0 )

/**********************************************************************
 *          Баланс
 */

struct balance
{
    const struct device *dev;
    HWND rows[8];
    UINT count;
};

static void balance_row_update( HWND row, int percent )
{
    WCHAR text[16];

    swprintf( text, ARRAY_SIZE(text), L"%d", percent );
    SetDlgItemTextW( row, IDC_BAL_VALUE, text );
}

static INT_PTR CALLBACK balance_row_proc( HWND row, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        return TRUE;
    case WM_HSCROLL:
        /* to the balance dialog, which knows the channel */
        SendMessageW( GetParent( row ), WM_HSCROLL, wp, (LPARAM)row );
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK balance_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct balance *b = (struct balance *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        RECT row_rect = { 0 }, client, ok;
        UINT channels = 2;
        float level;
        int y, width;
        HWND button = GetDlgItem( dlg, IDOK );

        b = (struct balance *)lp;
        SetWindowLongPtrW( dlg, DWLP_USER, lp );
        /* Windows' template row is a pattern: the rows are made from 144 */
        ShowWindow( GetDlgItem( dlg, 1520 ), SW_HIDE );
        ShowWindow( GetDlgItem( dlg, 1521 ), SW_HIDE );
        ShowWindow( GetDlgItem( dlg, 1522 ), SW_HIDE );
        volume_get( b->dev->id, &level, NULL, &channels );
        b->count = min( channels, ARRAY_SIZE(b->rows) );
        y = px( 12 );
        width = 0;
        for (UINT i = 0; i < b->count; i++)
        {
            HWND row = CreateDialogParamW( mmsys_instance, MAKEINTRESOURCEW( IDD_BALANCE_ROW ), dlg, balance_row_proc, 0 );
            int percent = (int)(volume_channel( b->dev->id, i ) * 100.f + 0.5f);

            b->rows[i] = row;
            GetWindowRect( row, &row_rect );
            width = row_rect.right - row_rect.left;
            SetWindowPos( row, NULL, px( 8 ), y, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
            SetDlgItemTextW( row, IDC_BAL_LABEL, load_string( IDS_CHANNEL_SHORT + min( i, 17 ) ) );
            SendDlgItemMessageW( row, IDC_BAL_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
            SendDlgItemMessageW( row, IDC_BAL_SLIDER, TBM_SETPOS, TRUE, percent );
            balance_row_update( row, percent );
            ShowWindow( row, SW_SHOW );
            y += row_rect.bottom - row_rect.top + px( 8 );
        }
        /* OK under the rows, the window around them */
        GetWindowRect( button, &ok );
        y += px( 6 );
        SetWindowPos( button, NULL, px( 8 ) + width - (ok.right - ok.left), y, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
        y += ok.bottom - ok.top + px( 10 );
        SetRect( &client, 0, 0, width + px( 16 ), y );
        AdjustWindowRectEx( &client, GetWindowLongW( dlg, GWL_STYLE ), FALSE, GetWindowLongW( dlg, GWL_EXSTYLE ) );
        SetWindowPos( dlg, NULL, 0, 0, client.right - client.left, client.bottom - client.top, SWP_NOMOVE | SWP_NOZORDER );
        return TRUE;
    }
    case WM_HSCROLL:
        for (UINT i = 0; b && i < b->count; i++)
        {
            int percent;

            if ((HWND)lp != b->rows[i]) continue;
            percent = SendDlgItemMessageW( b->rows[i], IDC_BAL_SLIDER, TBM_GETPOS, 0, 0 );
            balance_row_update( b->rows[i], percent );
            volume_set_channel( b->dev->id, i, percent / 100.f );
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL) EndDialog( dlg, IDOK );
        return TRUE;
    }
    return FALSE;
}

void show_balance( HWND owner, const struct device *dev )
{
    struct balance b = { dev };

    DialogBoxParamW( mmsys_instance, MAKEINTRESOURCEW( IDD_BALANCE ), owner, balance_proc, (LPARAM)&b );
}

/**********************************************************************
 *          Рівні
 */

/* the speaker of the mute button, and the same crossed out */
static HICON speaker_icon( BOOL crossed )
{
    int size = GetSystemMetrics( SM_CXSMICON );
    HICON base = LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_SOUND ), IMAGE_ICON, size, size, LR_SHARED ), icon;
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    HDC screen = GetDC( NULL ), hdc = CreateCompatibleDC( screen );
    DWORD *bits = NULL;
    HBITMAP color = CreateDIBSection( screen, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    HBITMAP mask = CreateBitmap( size, size, 1, 1, NULL );
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HGDIOBJ old = SelectObject( hdc, color );

    DrawIconEx( hdc, 0, 0, base, size, size, 0, NULL, DI_NORMAL );
    if (crossed && bits)
    {
        /* a red disc with a bar, bottom right, opaque */
        int r = size * 5 / 16, cx = size - r - 1, cy = size - r - 1;

        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
            {
                int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;

                if (d2 > r * r) continue;
                bits[y * size + x] = (d2 >= (r - 2) * (r - 2) || abs( dx + dy ) <= 1) ? 0xffd02020 : 0xffffffff;
            }
    }
    SelectObject( hdc, old );
    icon = CreateIconIndirect( &ii );
    DeleteObject( color );
    DeleteObject( mask );
    DeleteDC( hdc );
    ReleaseDC( NULL, screen );
    return icon;
}

static void row_refresh( struct props *p )
{
    WCHAR text[16];
    BOOL mute = FALSE;
    float level;
    int percent;

    if (!p->row || !volume_get( p->dev.id, &level, &mute, NULL )) return;
    percent = (int)(level * 100.f + 0.5f);
    if (GetCapture() != GetDlgItem( p->row, IDC_ROW_SLIDER ) &&
        SendDlgItemMessageW( p->row, IDC_ROW_SLIDER, TBM_GETPOS, 0, 0 ) != percent)
        SendDlgItemMessageW( p->row, IDC_ROW_SLIDER, TBM_SETPOS, TRUE, percent );
    swprintf( text, ARRAY_SIZE(text), L"%d", percent );
    SetDlgItemTextW( p->row, IDC_ROW_VALUE, text );
    SendDlgItemMessageW( p->row, IDC_ROW_MUTE, BM_SETCHECK, mute ? BST_CHECKED : BST_UNCHECKED, 0 );
    SendDlgItemMessageW( p->row, IDC_ROW_MUTE, BM_SETIMAGE, IMAGE_ICON, (LPARAM)(mute ? p->muted : p->speaker) );
}

static INT_PTR CALLBACK level_row_proc( HWND row, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( row, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        p = (struct props *)lp;
        SetWindowLongPtrW( row, DWLP_USER, lp );
        p->row = row;
        EnableThemeDialogTexture( row, ETDT_ENABLETAB );
        SetDlgItemTextW( row, IDC_ROW_GROUP, p->dev.name );
        SendDlgItemMessageW( row, IDC_ROW_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
        SendDlgItemMessageW( row, IDC_ROW_SLIDER, TBM_SETPAGESIZE, 0, 10 );
        row_refresh( p );
        return TRUE;
    case WM_HSCROLL:
        if (p)
        {
            int percent = SendDlgItemMessageW( row, IDC_ROW_SLIDER, TBM_GETPOS, 0, 0 );
            WCHAR text[16];

            volume_set( p->dev.id, percent / 100.f );
            swprintf( text, ARRAY_SIZE(text), L"%d", percent );
            SetDlgItemTextW( row, IDC_ROW_VALUE, text );
        }
        return TRUE;
    case WM_COMMAND:
        if (!p) break;
        switch (LOWORD( wp ))
        {
        case IDC_ROW_MUTE:
            volume_set_mute( p->dev.id, IsDlgButtonChecked( row, IDC_ROW_MUTE ) == BST_CHECKED );
            row_refresh( p );
            return TRUE;
        case IDC_ROW_BALANCE:
            show_balance( GetAncestor( row, GA_ROOT ), &p->dev );
            row_refresh( p );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK levels_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND row;

        p = (struct props *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)p );
        ShowWindow( GetDlgItem( dlg, IDC_LEVELS_LINE ), SW_HIDE );
        ShowWindow( GetDlgItem( dlg, 1508 ), SW_HIDE );
        p->speaker = speaker_icon( FALSE );
        p->muted = speaker_icon( TRUE );
        row = CreateDialogParamW( mmsys_instance, MAKEINTRESOURCEW( IDD_LEVEL_ROW ), dlg, level_row_proc, (LPARAM)p );
        SetWindowPos( row, HWND_TOP, px( 12 ), px( 14 ), 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW );
        SetTimer( dlg, TIMER_LEVELS, 500, NULL );
        return TRUE;
    }
    case WM_TIMER:
        /* the volume keys, the tray's flyout: the row follows */
        if (p && wp == TIMER_LEVELS) row_refresh( p );
        return TRUE;
    case WM_DESTROY:
        KillTimer( dlg, TIMER_LEVELS );
        if (p)
        {
            if (p->speaker) DestroyIcon( p->speaker );
            if (p->muted) DestroyIcon( p->muted );
            p->speaker = p->muted = NULL;
            p->row = NULL;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Загальні
 */

static INT_PTR CALLBACK general_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        /* what Arctic does not know of a device: its jack, its enhancements */
        static const UINT hidden[] = { 1203, 1205, IDC_GEN_JACK, 1226, 1227, 1228, IDC_GEN_CHANGE_ICON };
        HWND usage = GetDlgItem( dlg, IDC_GEN_USAGE );

        p = (struct props *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)p );
        for (UINT i = 0; i < ARRAY_SIZE(hidden); i++) ShowWindow( GetDlgItem( dlg, hidden[i] ), SW_HIDE );
        SetDlgItemTextW( dlg, IDC_GEN_NAME, p->dev.name );
        SendDlgItemMessageW( dlg, IDC_GEN_NAME, EM_LIMITTEXT, 63, 0 );
        SendDlgItemMessageW( dlg, IDC_GEN_ICON, STM_SETICON,
                             (WPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( p->dev.icon ), IMAGE_ICON, px( 48 ),
                                                 px( 48 ), LR_SHARED ), 0 );
        SendDlgItemMessageW( dlg, IDC_GEN_CONTROLLER_ICON, STM_SETICON,
                             (WPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_SOUND_CARD ), IMAGE_ICON,
                                                 GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ),
                                                 LR_SHARED ), 0 );
        {
            /* the name beside its icon */
            RECT rect;
            HWND name = GetDlgItem( dlg, IDC_GEN_CONTROLLER );
            GetWindowRect( name, &rect );
            MapWindowPoints( NULL, dlg, (POINT *)&rect, 2 );
            SetWindowPos( name, NULL, rect.left + px( 22 ), rect.top + px( 2 ), rect.right - rect.left - px( 22 ),
                          rect.bottom - rect.top, SWP_NOZORDER );
        }
        SetDlgItemTextW( dlg, IDC_GEN_CONTROLLER, p->dev.adapter );
        SendMessageW( usage, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_USE_DEVICE ) );
        SendMessageW( usage, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_DONT_USE_DEVICE ) );
        SendMessageW( usage, CB_SETCURSEL, p->dev.state == DEVICE_STATE_DISABLED ? 1 : 0, 0 );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_GEN_NAME:
            if (HIWORD( wp ) == EN_CHANGE && p) prop_changed( dlg );
            return TRUE;
        case IDC_GEN_USAGE:
            if (HIWORD( wp ) == CBN_SELCHANGE) prop_changed( dlg );
            return TRUE;
        case IDC_GEN_CONTROLLER_PROPS:
            ShellExecuteW( dlg, NULL, L"rundll32.exe", L"devmgr.dll,DeviceManager_ExecuteW", NULL, SW_SHOWNORMAL );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY && p)
        {
            WCHAR name[64];
            BOOL use = SendDlgItemMessageW( dlg, IDC_GEN_USAGE, CB_GETCURSEL, 0, 0 ) == 0;

            GetDlgItemTextW( dlg, IDC_GEN_NAME, name, ARRAY_SIZE(name) );
            if (name[0] && wcscmp( name, p->dev.name ))
            {
                device_set_name( &p->dev, name );
                lstrcpynW( p->dev.name, name, ARRAY_SIZE(p->dev.name) );
            }
            if (use != (p->dev.state != DEVICE_STATE_DISABLED))
            {
                device_set_enabled( &p->dev, use );
                p->dev.state = use ? DEVICE_STATE_ACTIVE : DEVICE_STATE_DISABLED;
            }
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Додатково
 */

static void format_text( const struct format_choice *fmt, WCHAR *text, size_t count )
{
    UINT quality = fmt->bits > 16 || fmt->rate > 48000 ? 8 : fmt->rate >= 48000 ? 7 : fmt->rate >= 44100 ? 6 :
                   fmt->rate >= 32000 ? 5 : fmt->rate >= 22050 ? 4 : fmt->rate >= 16000 ? 2 : 1;

    swprintf( text, count, load_string( IDS_FORMAT_CHANNELS ), fmt->channels, fmt->bits, fmt->rate,
              load_string( IDS_QUALITY_FIRST + quality ) );
}

static void fill_formats( HWND dlg, struct props *p )
{
    static const UINT rates[] = { 44100, 48000, 88200, 96000, 192000 };
    static const UINT depths[] = { 16, 24 };
    HWND combo = GetDlgItem( dlg, IDC_ADV_FORMAT );
    struct format_choice mix = { 2, 16, 48000 }, chosen;
    BOOL has_choice;
    int sel = -1;

    format_get_mix( p->dev.id, &mix );
    chosen = mix;
    has_choice = format_get_chosen( p->dev.id, &chosen );
    SendMessageW( combo, CB_RESETCONTENT, 0, 0 );
    p->format_count = 0;
    for (UINT d = 0; d < ARRAY_SIZE(depths); d++)
        for (UINT r = 0; r < ARRAY_SIZE(rates); r++)
        {
            struct format_choice *fmt = &p->formats[p->format_count];
            WCHAR text[160];

            fmt->channels = mix.channels;
            fmt->bits = depths[d];
            fmt->rate = rates[r];
            format_text( fmt, text, ARRAY_SIZE(text) );
            SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
            if (fmt->rate == chosen.rate && (has_choice ? fmt->bits == chosen.bits : fmt->bits == 16)) sel = p->format_count;
            p->format_count++;
        }
    /* a rate of the device's that Windows' list has not */
    if (sel < 0)
    {
        struct format_choice *fmt = &p->formats[p->format_count];
        WCHAR text[160];

        *fmt = chosen;
        fmt->channels = mix.channels;
        format_text( fmt, text, ARRAY_SIZE(text) );
        SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
        sel = p->format_count++;
    }
    SendMessageW( combo, CB_SETCURSEL, sel, 0 );
}

static INT_PTR CALLBACK advanced_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        static const UINT hidden[] = { 1413, 1414, 1415, 1418, 1419, 1420, 1421, 1422, 1423, 1424 };

        p = (struct props *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)p );
        for (UINT i = 0; i < ARRAY_SIZE(hidden); i++) ShowWindow( GetDlgItem( dlg, hidden[i] ), SW_HIDE );
        fill_formats( dlg, p );
        /* Wine's drivers always let a program have the device to itself */
        CheckDlgButton( dlg, IDC_ADV_EXCLUSIVE, BST_CHECKED );
        CheckDlgButton( dlg, IDC_ADV_EXCLUSIVE_PRIORITY, BST_CHECKED );
        EnableWindow( GetDlgItem( dlg, IDC_ADV_EXCLUSIVE ), FALSE );
        EnableWindow( GetDlgItem( dlg, IDC_ADV_EXCLUSIVE_PRIORITY ), FALSE );
        if (p->dev.capture) ShowWindow( GetDlgItem( dlg, IDC_ADV_TEST ), SW_HIDE );
        return TRUE;
    }
    case WM_COMMAND:
        if (!p) break;
        switch (LOWORD( wp ))
        {
        case IDC_ADV_FORMAT:
            if (HIWORD( wp ) == CBN_SELCHANGE) prop_changed( dlg );
            return TRUE;
        case IDC_ADV_TEST:
        {
            int sel = SendDlgItemMessageW( dlg, IDC_ADV_FORMAT, CB_GETCURSEL, 0, 0 );
            HCURSOR old = SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_WAIT ) );

            if (sel >= 0 && sel < (int)p->format_count && !format_test( p->dev.id, &p->formats[sel] ))
                MessageBoxW( dlg, load_string( IDS_FORMAT_ERROR ), load_string( IDS_SOUND ), MB_OK | MB_ICONEXCLAMATION );
            SetCursor( old );
            return TRUE;
        }
        case IDC_ADV_RESTORE:
            format_choose( p->dev.id, NULL );
            fill_formats( dlg, p );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY && p)
        {
            int sel = SendDlgItemMessageW( dlg, IDC_ADV_FORMAT, CB_GETCURSEL, 0, 0 );
            struct format_choice mix, chosen;

            if (sel >= 0 && sel < (int)p->format_count)
            {
                /* the device's own format chosen again is no choice */
                BOOL had = format_get_chosen( p->dev.id, &chosen );
                if (had || !format_get_mix( p->dev.id, &mix ) || mix.rate != p->formats[sel].rate ||
                    p->formats[sel].bits != 16)
                    format_choose( p->dev.id, &p->formats[sel] );
            }
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Слухати
 */

#define IDC_LISTEN_TARGET   1600
#define IDC_LISTEN          1601
#define IDC_LISTEN_FROM     1602
#define IDC_LISTEN_TO       1603
#define IDC_LISTEN_TEXT     1604
#define IDC_LISTEN_BATTERY  1606
#define IDC_LISTEN_SAVE     1607

static INT_PTR CALLBACK listen_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        struct device outputs[32];
        WCHAR target[128], format[300], text[400];
        HWND combo = GetDlgItem( dlg, IDC_LISTEN_TARGET );
        UINT count, i;
        int sel = 0;

        p = (struct props *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)p );
        GetDlgItemTextW( dlg, IDC_LISTEN_TEXT, format, ARRAY_SIZE(format) );
        if (wcsstr( format, L"%1" ))
        {
            WCHAR *at = wcsstr( format, L"%1" );
            *at = 0;
            swprintf( text, ARRAY_SIZE(text), L"%s%s%s", format, p->dev.name, at + 2 );
            SetDlgItemTextW( dlg, IDC_LISTEN_TEXT, text );
        }
        SendDlgItemMessageW( dlg, IDC_LISTEN_FROM, STM_SETICON, (WPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( p->dev.icon ), IMAGE_ICON, px( 32 ), px( 32 ), LR_SHARED ), 0 );
        SendDlgItemMessageW( dlg, IDC_LISTEN_TO, STM_SETICON, (WPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_SPEAKERS ), IMAGE_ICON, px( 32 ), px( 32 ), LR_SHARED ), 0 );
        CheckDlgButton( dlg, IDC_LISTEN, listen_get( p->dev.id, target, ARRAY_SIZE(target) ) ? BST_CHECKED : BST_UNCHECKED );
        SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_DEFAULT_PLAYBACK ) );
        count = devices_list( FALSE, outputs, ARRAY_SIZE(outputs) );
        for (i = 0; i < count; i++)
        {
            int index;

            if (outputs[i].state != DEVICE_STATE_ACTIVE) continue;
            swprintf( text, ARRAY_SIZE(text), L"%s (%s)", outputs[i].name, outputs[i].adapter );
            index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
            SendMessageW( combo, CB_SETITEMDATA, index, (LPARAM)wcsdup( outputs[i].id ) );
            if (target[0] && !wcscmp( target, outputs[i].id )) sel = index;
        }
        SendMessageW( combo, CB_SETCURSEL, sel, 0 );
        /* the power of a laptop does not stop it: as Windows with the first box checked */
        CheckDlgButton( dlg, IDC_LISTEN_BATTERY, BST_CHECKED );
        EnableWindow( GetDlgItem( dlg, IDC_LISTEN_BATTERY ), FALSE );
        EnableWindow( GetDlgItem( dlg, IDC_LISTEN_SAVE ), FALSE );
        return TRUE;
    }
    case WM_COMMAND:
        if ((LOWORD( wp ) == IDC_LISTEN && HIWORD( wp ) == BN_CLICKED) ||
            (LOWORD( wp ) == IDC_LISTEN_TARGET && HIWORD( wp ) == CBN_SELCHANGE))
            prop_changed( dlg );
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY)
        {
            HWND combo = GetDlgItem( dlg, IDC_LISTEN_TARGET );
            int sel = SendMessageW( combo, CB_GETCURSEL, 0, 0 );
            const WCHAR *target = sel > 0 ? (const WCHAR *)SendMessageW( combo, CB_GETITEMDATA, sel, 0 ) : NULL;

            listen_set( p->dev.id, IsDlgButtonChecked( dlg, IDC_LISTEN ) == BST_CHECKED, target ? target : L"" );
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    case WM_DESTROY:
    {
        HWND combo = GetDlgItem( dlg, IDC_LISTEN_TARGET );
        int count = SendMessageW( combo, CB_GETCOUNT, 0, 0 );

        for (int i = 1; i < count; i++) free( (void *)SendMessageW( combo, CB_GETITEMDATA, i, 0 ) );
        break;
    }
    }
    return FALSE;
}

void show_device_properties( HWND owner, const struct device *dev )
{
    static const struct { UINT dialog, title; DLGPROC proc; } tabs[] =
    {
        { IDD_GENERAL, IDS_GENERAL, general_proc },
        { IDD_LISTEN, IDS_LISTEN, listen_proc },
        { IDD_LEVELS, IDS_LEVELS, levels_proc },
        { IDD_ADVANCED, IDS_ADVANCED, advanced_proc },
    };
    PROPSHEETPAGEW pages[ARRAY_SIZE(tabs)];
    PROPSHEETHEADERW header = { sizeof(header) };
    struct props *p = calloc( 1, sizeof(*p) );
    WCHAR title[160];
    UINT count = 0;

    if (!p) return;
    p->dev = *dev;
    memset( pages, 0, sizeof(pages) );
    for (UINT i = 0; i < ARRAY_SIZE(tabs); i++)
    {
        /* a device that is off has no levels and no format to show */
        if (dev->state != DEVICE_STATE_ACTIVE && tabs[i].dialog != IDD_GENERAL) continue;
        /* a microphone is listened to through an output */
        if (tabs[i].dialog == IDD_LISTEN && !dev->capture) continue;
        pages[count].dwSize = sizeof(pages[count]);
        pages[count].dwFlags = PSP_USETITLE;
        pages[count].hInstance = mmsys_instance;
        pages[count].pszTemplate = MAKEINTRESOURCEW( tabs[i].dialog );
        pages[count].pszTitle = MAKEINTRESOURCEW( tabs[i].title );
        pages[count].pfnDlgProc = tabs[i].proc;
        pages[count].lParam = (LPARAM)p;
        count++;
    }
    swprintf( title, ARRAY_SIZE(title), load_string( IDS_PROPERTIES_TITLE ), dev->name );
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP;
    header.hwndParent = owner;
    header.hInstance = mmsys_instance;
    header.pszCaption = title;
    header.nPages = count;
    header.ppsp = pages;
    PropertySheetW( &header );
    free( p );
}
