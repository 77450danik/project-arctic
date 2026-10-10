/*
 * Sound of the Control Panel: "Відтворення" and "Запис"
 *
 * Windows' pages (mmsys.cpl's dialogs 110 and 111): the endpoints as tiles
 * of an icon, the name, the adapter and the state, the default one with a
 * green tick; "Установити за замовчуванням" with its menu for the
 * communications default, "Властивості", and a menu on the right button.
 * A microphone shows what it hears on a meter of ten bars, read from it
 * here: Wine has no IAudioMeterInformation. The list looks again every
 * second: a device plugged in or made the default elsewhere shows up.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <math.h>

#include "mmsys.h"
#include "prsht.h"
#include "objbase.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "mmreg.h"

#define TIMER_REFRESH  1
#define TIMER_METER    2
#define ROW_HEIGHT     px(56)
#define ICON_SIZE      px(40)
#define MAX_DEVICES    32

#define ID_MENU_TEST         100
#define ID_MENU_DISABLE      101
#define ID_MENU_DEFAULT      102
#define ID_MENU_DEFAULT_COMM 103
#define ID_MENU_PROPERTIES   104

static const IID iid_audio_client =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID iid_capture_client =
    { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const CLSID clsid_enumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID iid_enumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };

/* what a microphone hears, for its meter */
struct meter
{
    WCHAR id[128];
    IAudioClient *client;
    IAudioCaptureClient *capture;
    WORD channels, bits;
    BOOL is_float;
    float peak;
};

struct page
{
    BOOL capture;
    struct device devices[MAX_DEVICES];
    UINT count;
    struct meter meters[8];
    UINT meter_count;
    HFONT bold;
};

/**********************************************************************
 *          The meters
 */

static void meters_close( struct page *page )
{
    for (UINT i = 0; i < page->meter_count; i++)
    {
        if (page->meters[i].client) IAudioClient_Stop( page->meters[i].client );
        if (page->meters[i].capture) IAudioCaptureClient_Release( page->meters[i].capture );
        if (page->meters[i].client) IAudioClient_Release( page->meters[i].client );
    }
    page->meter_count = 0;
}

static void meters_open( struct page *page )
{
    IMMDeviceEnumerator *enumerator = NULL;

    meters_close( page );
    if (!page->capture) return;
    if (FAILED(CoCreateInstance( &clsid_enumerator, NULL, CLSCTX_INPROC_SERVER, &iid_enumerator, (void **)&enumerator )))
        return;
    for (UINT i = 0; i < page->count && page->meter_count < ARRAY_SIZE(page->meters); i++)
    {
        struct meter *meter = &page->meters[page->meter_count];
        WAVEFORMATEXTENSIBLE *fmt = NULL;
        IMMDevice *device = NULL;

        if (page->devices[i].state != DEVICE_STATE_ACTIVE) continue;
        memset( meter, 0, sizeof(*meter) );
        lstrcpyW( meter->id, page->devices[i].id );
        if (FAILED(IMMDeviceEnumerator_GetDevice( enumerator, meter->id, &device ))) continue;
        if (SUCCEEDED(IMMDevice_Activate( device, &iid_audio_client, CLSCTX_INPROC_SERVER, NULL, (void **)&meter->client )) &&
            SUCCEEDED(IAudioClient_GetMixFormat( meter->client, (WAVEFORMATEX **)&fmt )) && fmt &&
            SUCCEEDED(IAudioClient_Initialize( meter->client, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, &fmt->Format, NULL )) &&
            SUCCEEDED(IAudioClient_GetService( meter->client, &iid_capture_client, (void **)&meter->capture )) &&
            SUCCEEDED(IAudioClient_Start( meter->client )))
        {
            meter->channels = fmt->Format.nChannels;
            meter->bits = fmt->Format.wBitsPerSample;
            meter->is_float = fmt->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                              (fmt->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->SubFormat.Data1 == 3);
            page->meter_count++;
        }
        else
        {
            if (meter->capture) IAudioCaptureClient_Release( meter->capture );
            if (meter->client) IAudioClient_Release( meter->client );
        }
        CoTaskMemFree( fmt );
        IMMDevice_Release( device );
    }
    IMMDeviceEnumerator_Release( enumerator );
}

/* the loudest sample since the last look, falling off as Windows' meter does */
static void meters_read( struct page *page )
{
    for (UINT m = 0; m < page->meter_count; m++)
    {
        struct meter *meter = &page->meters[m];
        float peak = 0;
        UINT32 frames;
        DWORD flags;
        BYTE *data;

        while (SUCCEEDED(IAudioCaptureClient_GetBuffer( meter->capture, &data, &frames, &flags, NULL, NULL )) && frames)
        {
            UINT32 samples = frames * meter->channels;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                for (UINT32 i = 0; i < samples; i++)
                {
                    float v;
                    if (meter->is_float) v = fabsf( ((float *)data)[i] );
                    else if (meter->bits == 16) v = abs( ((short *)data)[i] ) / 32768.f;
                    else if (meter->bits == 32) v = fabsf( ((int *)data)[i] / 2147483648.f );
                    else v = 0;
                    if (v > peak) peak = v;
                }
            }
            IAudioCaptureClient_ReleaseBuffer( meter->capture, frames );
        }
        meter->peak = max( peak, meter->peak * 0.7f );
    }
}

static float meter_peak( struct page *page, const WCHAR *id )
{
    for (UINT m = 0; m < page->meter_count; m++)
        if (!wcscmp( page->meters[m].id, id )) return page->meters[m].peak;
    return -1;
}

/**********************************************************************
 *          The list
 */

static int selected( HWND dlg )
{
    return SendDlgItemMessageW( dlg, IDC_DEVICES, LVM_GETNEXTITEM, -1, LVNI_SELECTED );
}

static void update_buttons( HWND dlg, struct page *page )
{
    int sel = selected( dlg );
    const struct device *dev = sel >= 0 && sel < (int)page->count ? &page->devices[sel] : NULL;

    EnableWindow( GetDlgItem( dlg, IDC_CONFIGURE ), FALSE );
    EnableWindow( GetDlgItem( dlg, IDC_SET_DEFAULT ), dev && dev->state == DEVICE_STATE_ACTIVE &&
                  (!dev->is_default || !dev->is_default_comm) );
    EnableWindow( GetDlgItem( dlg, IDC_DEVICE_PROPERTIES ), dev != NULL );
}

static void fill_list( HWND dlg, struct page *page, BOOL keep )
{
    HWND list = GetDlgItem( dlg, IDC_DEVICES );
    struct device now[MAX_DEVICES];
    UINT count = devices_list( page->capture, now, MAX_DEVICES );
    WCHAR chosen[128] = L"";
    int sel = selected( dlg );

    if (keep && count == page->count && !memcmp( now, page->devices, count * sizeof(now[0]) )) return;
    if (sel >= 0 && sel < (int)page->count) lstrcpyW( chosen, page->devices[sel].id );
    memcpy( page->devices, now, count * sizeof(now[0]) );
    page->count = count;

    SendMessageW( list, WM_SETREDRAW, FALSE, 0 );
    SendMessageW( list, LVM_DELETEALLITEMS, 0, 0 );
    for (UINT i = 0; i < count; i++)
    {
        LVITEMW item = { LVIF_TEXT | LVIF_PARAM };

        item.iItem = i;
        item.pszText = page->devices[i].name;
        item.lParam = i;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        if (chosen[0] ? !wcscmp( chosen, page->devices[i].id ) : page->devices[i].is_default)
        {
            LVITEMW state = { 0 };
            state.state = state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW( list, LVM_SETITEMSTATE, i, (LPARAM)&state );
        }
    }
    SendMessageW( list, WM_SETREDRAW, TRUE, 0 );
    InvalidateRect( list, NULL, TRUE );
    update_buttons( dlg, page );
    meters_open( page );
}

static const WCHAR *state_text( const struct device *dev )
{
    if (dev->state == DEVICE_STATE_DISABLED) return load_string( IDS_DISABLED_STATE );
    if (dev->state != DEVICE_STATE_ACTIVE) return load_string( IDS_NOT_PLUGGED );
    if (dev->is_default) return load_string( IDS_DEFAULT_DEVICE );
    if (dev->is_default_comm) return load_string( IDS_DEFAULT_COMM );
    return load_string( IDS_READY );
}

/* the green disc with a tick of the default device */
static void draw_tick( HDC hdc, int x, int y, int size )
{
    HBRUSH brush = CreateSolidBrush( RGB( 0x2e, 0xa0, 0x43 ) );
    HPEN white = CreatePen( PS_SOLID, max( 2, size / 8 ), RGB( 255, 255, 255 ) ), none = GetStockObject( NULL_PEN );
    HGDIOBJ old_brush = SelectObject( hdc, brush ), old_pen = SelectObject( hdc, none );

    Ellipse( hdc, x, y, x + size + 1, y + size + 1 );
    SelectObject( hdc, white );
    MoveToEx( hdc, x + size * 27 / 100, y + size * 52 / 100, NULL );
    LineTo( hdc, x + size * 43 / 100, y + size * 68 / 100 );
    LineTo( hdc, x + size * 74 / 100, y + size * 34 / 100 );
    SelectObject( hdc, old_brush );
    SelectObject( hdc, old_pen );
    DeleteObject( brush );
    DeleteObject( white );
}

static void draw_meter( HDC hdc, const RECT *row, float peak )
{
    int bars = 10, bar_h = px( 3 ), gap = px( 1 ), width = px( 10 );
    int lit = peak <= 0 ? 0 : (int)(min( 1.f, sqrtf( peak ) ) * bars + 0.5f);
    int x = row->right - width - px( 12 ), y = row->bottom - px( 8 ) - bar_h;
    HBRUSH on = CreateSolidBrush( RGB( 0x33, 0xc4, 0x4a ) ), off = CreateSolidBrush( RGB( 0xd0, 0xd0, 0xd0 ) );

    for (int i = 0; i < bars; i++)
    {
        RECT bar = { x, y - i * (bar_h + gap), x + width, y - i * (bar_h + gap) + bar_h };
        FillRect( hdc, &bar, i < lit ? on : off );
    }
    DeleteObject( on );
    DeleteObject( off );
}

static void draw_item( HWND dlg, struct page *page, const DRAWITEMSTRUCT *dis )
{
    const struct device *dev;
    RECT row = dis->rcItem, text;
    HDC hdc = dis->hDC;
    BOOL chosen = dis->itemState & ODS_SELECTED, off;
    HFONT font = (HFONT)SendMessageW( dlg, WM_GETFONT, 0, 0 );
    HGDIOBJ old_font;
    int line, y;
    HICON icon;
    float peak;

    if (dis->itemID >= page->count) return;
    dev = &page->devices[dis->itemID];
    off = dev->state != DEVICE_STATE_ACTIVE;

    FillRect( hdc, &row, GetSysColorBrush( COLOR_WINDOW ) );
    if (chosen)
    {
        /* the selection of Windows 7's lists: light blue with a border */
        HBRUSH fill = CreateSolidBrush( RGB( 0xd9, 0xeb, 0xfc ) ), frame = CreateSolidBrush( RGB( 0x7d, 0xa2, 0xce ) );
        RECT sel = row;

        InflateRect( &sel, -px( 1 ), -px( 1 ) );
        FillRect( hdc, &sel, fill );
        FrameRect( hdc, &sel, frame );
        DeleteObject( fill );
        DeleteObject( frame );
    }
    icon = LoadImageW( mmsys_instance, MAKEINTRESOURCEW( dev->icon ), IMAGE_ICON, ICON_SIZE, ICON_SIZE, LR_SHARED );
    y = row.top + (row.bottom - row.top - ICON_SIZE) / 2;
    DrawIconEx( hdc, row.left + px( 8 ), y, icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL );
    if (!off && (dev->is_default || dev->is_default_comm))
        draw_tick( hdc, row.left + px( 8 ) + ICON_SIZE - px( 14 ), y + ICON_SIZE - px( 16 ), px( 16 ) );

    SetBkMode( hdc, TRANSPARENT );
    old_font = SelectObject( hdc, font );
    {
        TEXTMETRICW tm;
        GetTextMetricsW( hdc, &tm );
        line = tm.tmHeight + px( 1 );
    }
    SetRect( &text, row.left + px( 16 ) + ICON_SIZE, row.top + (row.bottom - row.top - 3 * line) / 2, row.right - px( 30 ), 0 );
    text.bottom = text.top + line;
    SetTextColor( hdc, off ? GetSysColor( COLOR_GRAYTEXT ) : GetSysColor( COLOR_WINDOWTEXT ) );
    DrawTextW( hdc, dev->name, -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    OffsetRect( &text, 0, line );
    DrawTextW( hdc, dev->adapter, -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    OffsetRect( &text, 0, line );
    DrawTextW( hdc, state_text( dev ), -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old_font );

    if (!off && (peak = meter_peak( page, dev->id )) >= 0) draw_meter( hdc, &row, peak );
}

/**********************************************************************
 *          What the buttons and the menus do
 */

static void set_default( HWND dlg, struct page *page, BOOL communications_only )
{
    int sel = selected( dlg );

    if (sel < 0 || sel >= (int)page->count) return;
    device_set_default( &page->devices[sel], communications_only );
    /* mmdevapi looks at its devices again every two seconds at most */
    Sleep( 300 );
    fill_list( dlg, page, FALSE );
}

static void default_menu( HWND dlg, struct page *page )
{
    HMENU menu = CreatePopupMenu();
    RECT rect;
    UINT cmd;

    GetWindowRect( GetDlgItem( dlg, IDC_SET_DEFAULT ), &rect );
    AppendMenuW( menu, MF_STRING, ID_MENU_DEFAULT, load_string( IDS_DEFAULT_DEVICE ) );
    AppendMenuW( menu, MF_STRING, ID_MENU_DEFAULT_COMM, load_string( IDS_DEFAULT_COMM ) );
    cmd = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_NONOTIFY, rect.left, rect.bottom, 0, dlg, NULL );
    DestroyMenu( menu );
    if (cmd == ID_MENU_DEFAULT) set_default( dlg, page, FALSE );
    if (cmd == ID_MENU_DEFAULT_COMM) set_default( dlg, page, TRUE );
}

static void open_properties( HWND dlg, struct page *page )
{
    int sel = selected( dlg );

    if (sel < 0 || sel >= (int)page->count) return;
    show_device_properties( GetParent( dlg ), &page->devices[sel] );
    fill_list( dlg, page, FALSE );
}

static void context_menu( HWND dlg, struct page *page, POINT pt )
{
    int sel = selected( dlg );
    const struct device *dev;
    struct format_choice fmt;
    HMENU menu;
    UINT cmd;

    if (sel < 0 || sel >= (int)page->count) return;
    dev = &page->devices[sel];
    menu = CreatePopupMenu();
    if (!page->capture && dev->state == DEVICE_STATE_ACTIVE)
        AppendMenuW( menu, MF_STRING, ID_MENU_TEST, load_string( IDS_TEST ) );
    AppendMenuW( menu, MF_STRING, ID_MENU_DISABLE,
                 load_string( dev->state == DEVICE_STATE_DISABLED ? IDS_ENABLE_DEVICE : IDS_DISABLE_DEVICE ) );
    if (dev->state == DEVICE_STATE_ACTIVE)
    {
        AppendMenuW( menu, MF_STRING | (dev->is_default ? MF_GRAYED : 0), ID_MENU_DEFAULT, load_string( IDS_SET_DEFAULT ) );
        AppendMenuW( menu, MF_STRING | (dev->is_default_comm ? MF_GRAYED : 0), ID_MENU_DEFAULT_COMM,
                     load_string( IDS_SET_DEFAULT_COMM ) );
    }
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    AppendMenuW( menu, MF_STRING, ID_MENU_PROPERTIES, load_string( IDS_PROPERTIES ) );
    SetMenuDefaultItem( menu, ID_MENU_PROPERTIES, FALSE );
    cmd = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, dlg, NULL );
    DestroyMenu( menu );
    switch (cmd)
    {
    case ID_MENU_TEST:
        if (!format_get_mix( dev->id, &fmt ) || !format_test( dev->id, &fmt )) MessageBeep( MB_ICONEXCLAMATION );
        break;
    case ID_MENU_DISABLE:
        device_set_enabled( dev, dev->state == DEVICE_STATE_DISABLED );
        Sleep( 2100 );   /* mmdevapi's next look at its devices */
        fill_list( dlg, page, FALSE );
        break;
    case ID_MENU_DEFAULT: set_default( dlg, page, FALSE ); break;
    case ID_MENU_DEFAULT_COMM: set_default( dlg, page, TRUE ); break;
    case ID_MENU_PROPERTIES: open_properties( dlg, page ); break;
    }
}

INT_PTR CALLBACK devices_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct page *page = (struct page *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND list = GetDlgItem( dlg, IDC_DEVICES );
        LVCOLUMNW column = { LVCF_WIDTH };
        RECT rect;

        if (!(page = calloc( 1, sizeof(*page) ))) return FALSE;
        page->capture = ((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)page );
        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
        GetClientRect( list, &rect );
        column.cx = rect.right - GetSystemMetrics( SM_CXVSCROLL );
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
        fill_list( dlg, page, FALSE );
        SetTimer( dlg, TIMER_REFRESH, 2000, NULL );
        if (page->capture) SetTimer( dlg, TIMER_METER, 80, NULL );
        return TRUE;
    }
    case WM_MEASUREITEM:
        if (wp == IDC_DEVICES)
        {
            ((MEASUREITEMSTRUCT *)lp)->itemHeight = ROW_HEIGHT;
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IDC_DEVICES && page)
        {
            draw_item( dlg, page, (const DRAWITEMSTRUCT *)lp );
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (!page) break;
        if (wp == TIMER_REFRESH) fill_list( dlg, page, TRUE );
        if (wp == TIMER_METER && page->meter_count)
        {
            meters_read( page );
            InvalidateRect( GetDlgItem( dlg, IDC_DEVICES ), NULL, FALSE );
        }
        return TRUE;
    case WM_COMMAND:
        if (!page) break;
        switch (LOWORD( wp ))
        {
        case IDC_SET_DEFAULT:
            set_default( dlg, page, FALSE );
            return TRUE;
        case IDC_DEVICE_PROPERTIES:
            open_properties( dlg, page );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (!page) break;
        if (hdr->idFrom == IDC_SET_DEFAULT && hdr->code == BCN_DROPDOWN)
        {
            default_menu( dlg, page );
            return TRUE;
        }
        if (hdr->idFrom != IDC_DEVICES) break;
        switch (hdr->code)
        {
        case LVN_ITEMCHANGED:
            update_buttons( dlg, page );
            return TRUE;
        case NM_DBLCLK:
            open_properties( dlg, page );
            return TRUE;
        case NM_RCLICK:
        {
            POINT pt;
            GetCursorPos( &pt );
            context_menu( dlg, page, pt );
            return TRUE;
        }
        }
        break;
    }
    case WM_DESTROY:
        if (page)
        {
            KillTimer( dlg, TIMER_REFRESH );
            KillTimer( dlg, TIMER_METER );
            meters_close( page );
            free( page );
            SetWindowLongPtrW( dlg, DWLP_USER, 0 );
        }
        break;
    }
    return FALSE;
}
