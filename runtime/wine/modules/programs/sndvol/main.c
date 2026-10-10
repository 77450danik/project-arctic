/*
 * Volume Mixer (sndvol.exe)
 *
 * Windows' mixer: the output's volume on the left, and a column for every
 * program that plays on it, with its own level, mute and meter. The programs'
 * sessions come from mmdevapi's shared rows (wine/arctic_audio.h): what is set
 * here is the session's volume in its own process. The volume icon's menu
 * opens it (sndvolsso.dll), as in Windows.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "winreg.h"
#include "commctrl.h"
#include "shellapi.h"
#include "objbase.h"
#include "mmdeviceapi.h"
#include "endpointvolume.h"
#include "wine/arctic_audio.h"
#include "sndvol.h"

/* Core Audio's GUIDs, which libuuid does not carry */
static const CLSID clsid_enumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID iid_enumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID iid_endpoint_volume =
    { 0x5cdf2c82, 0x841e, 0x4546, { 0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a } };
static const PROPERTYKEY key_friendly_name =   /* PKEY_Device_FriendlyName */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const PROPERTYKEY key_device_desc =     /* PKEY_Device_DeviceDesc */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
static const PROPERTYKEY key_form_factor =     /* PKEY_AudioEndpoint_FormFactor */
    { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

#define TIMER_METER    1
#define METER_MS       40
#define MAX_COLUMNS    32
#define LIST_EVERY     25          /* ticks between looks at which programs still run */

struct column
{
    HWND dlg;
    int row;                       /* in the shared rows; -1 for System Sounds that never played */
    DWORD pid;
    GUID session;
    BOOL system;
    float shown;                   /* the meter, falling slowly */
    HICON icon;
};

static HINSTANCE instance;
static HWND mixer;
static IMMDeviceEnumerator *devices;
static IAudioEndpointVolume *volume;
static WCHAR device_id[256];
static GUID device_guid;
static struct arctic_audio_sessions *shared;
static struct column columns[MAX_COLUMNS];
static UINT column_count;
static float device_shown;
static HICON speaker_icon, muted_icon;
static DWORD shell_pid;
static BOOL alive[ARCTIC_AUDIO_SESSIONS];
static UINT tick;

static WCHAR *load_string( UINT id )
{
    static WCHAR buffers[4][256];
    static UINT next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

/**********************************************************************
 *          The programs' sessions
 */

static struct arctic_audio_sessions *open_shared(void)
{
    HANDLE mapping;
    struct arctic_audio_sessions *view;

    if (shared) return shared;
    if (!(mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*view),
                                        ARCTIC_AUDIO_SESSIONS_SECTION )))
        return NULL;
    if (!(view = MapViewOfFile( mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*view) ))) return NULL;
    InterlockedCompareExchange( (LONG *)&view->version, ARCTIC_AUDIO_SESSIONS_VERSION, 0 );
    if (view->version != ARCTIC_AUDIO_SESSIONS_VERSION) return NULL;
    return shared = view;
}

static void set_row( int row, const float *level, const BOOL *mute )
{
    HANDLE mutex;

    if (!shared || row < 0) return;
    mutex = CreateMutexW( NULL, FALSE, ARCTIC_AUDIO_SESSIONS_MUTEX );
    if (mutex) WaitForSingleObject( mutex, INFINITE );
    if (level) shared->rows[row].volume = *level;
    if (mute) shared->rows[row].mute = *mute;
    shared->rows[row].changed++;
    InterlockedIncrement( &shared->serial );
    if (mutex)
    {
        ReleaseMutex( mutex );
        CloseHandle( mutex );
    }
}

static BOOL process_alive( DWORD pid )
{
    HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
    DWORD code = 0;

    if (!process) return FALSE;
    GetExitCodeProcess( process, &code );
    CloseHandle( process );
    return code == STILL_ACTIVE;
}

static void look_at_processes(void)
{
    HWND shell = GetShellWindow();
    UINT i;

    shell_pid = 0;
    if (shell) GetWindowThreadProcessId( shell, &shell_pid );
    for (i = 0; i < ARCTIC_AUDIO_SESSIONS; i++)
        alive[i] = shared && shared->rows[i].pid && process_alive( shared->rows[i].pid );
}

static BOOL row_shown( UINT i )
{
    return shared && alive[i] && shared->rows[i].pid && IsEqualGUID( &shared->rows[i].device, &device_guid );
}

/* the shell's sounds are "System Sounds", as Windows names its own */
static BOOL row_system( UINT i )
{
    const WCHAR *icon = shared->rows[i].icon, *file = wcsrchr( icon, '\\' );

    return shared->rows[i].pid == shell_pid || (file && !wcsicmp( file + 1, L"explorer.exe" ));
}

/**********************************************************************
 *          Icons
 */

static HICON make_speaker( BOOL crossed )
{
    int size = GetSystemMetrics( SM_CXSMICON );
    HICON base = LoadImageW( instance, MAKEINTRESOURCEW( IDI_SNDVOL ), IMAGE_ICON, size, size, LR_SHARED ), icon;
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
        /* a red disc with a bar, bottom right */
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

/* "C:\path\app.exe" or "%windir%\x.dll,-123" */
static HICON program_icon( const WCHAR *location )
{
    WCHAR path[MAX_PATH], *comma;
    HICON icon = NULL;
    int index = 0;

    if (!location[0]) return LoadIconW( NULL, (const WCHAR *)IDI_APPLICATION );
    ExpandEnvironmentStringsW( location, path, ARRAY_SIZE(path) );
    if ((comma = wcsrchr( path, ',' )) && (comma[1] == '-' || (comma[1] >= '0' && comma[1] <= '9')))
    {
        index = _wtoi( comma + 1 );
        *comma = 0;
    }
    if (path[0] == '@') memmove( path, path + 1, (wcslen( path ) + 1) * sizeof(WCHAR) );
    if (ExtractIconExW( path, index, &icon, NULL, 1 ) != 1 || !icon)
        icon = LoadIconW( NULL, (const WCHAR *)IDI_APPLICATION );
    return icon;
}

static UINT device_icon_id( IMMDevice *device )
{
    IPropertyStore *props;
    PROPVARIANT pv;
    UINT id = IDI_SPEAKERS;

    if (FAILED(IMMDevice_OpenPropertyStore( device, STGM_READ, &props ))) return id;
    PropVariantInit( &pv );
    if (SUCCEEDED(IPropertyStore_GetValue( props, &key_form_factor, &pv )) && pv.vt == VT_UI4)
    {
        switch (pv.ulVal)
        {
        case Headphones:
        case Headset: id = IDI_HEADPHONES; break;
        case SPDIF: id = IDI_DIGITAL; break;
        case DigitalAudioDisplayDevice: id = IDI_DISPLAY; break;
        }
    }
    PropVariantClear( &pv );
    IPropertyStore_Release( props );
    return id;
}

static void get_string( IMMDevice *device, const PROPERTYKEY *key, WCHAR *out, UINT count )
{
    IPropertyStore *props;
    PROPVARIANT pv;

    out[0] = 0;
    if (FAILED(IMMDevice_OpenPropertyStore( device, STGM_READ, &props ))) return;
    PropVariantInit( &pv );
    if (SUCCEEDED(IPropertyStore_GetValue( props, key, &pv )) && pv.vt == VT_LPWSTR)
        lstrcpynW( out, pv.pwszVal, count );
    PropVariantClear( &pv );
    IPropertyStore_Release( props );
}

/* "Speakers" in the user's language, as the Sound control panel names it:
 * Wine's drivers name the endpoints in English */
static void translate( WCHAR *name, UINT count )
{
    static const struct { const WCHAR *english; UINT id; } kinds[] =
    {
        { L"Speakers", IDS_SPEAKERS },
        { L"Headphones", IDS_HEADPHONES },
        { L"Digital Output", IDS_DIGITAL_OUTPUT },
        { L"Digital Display", IDS_DIGITAL_DISPLAY },
    };
    WCHAR rest[256];

    for (UINT i = 0; i < ARRAY_SIZE(kinds); i++)
    {
        UINT len = wcslen( kinds[i].english );

        if (wcsncmp( name, kinds[i].english, len ) || (name[len] && name[len] != ' ')) continue;
        lstrcpynW( rest, name + len, ARRAY_SIZE(rest) );
        swprintf( name, count, L"%s%s", load_string( kinds[i].id ), rest );
        return;
    }
}

/**********************************************************************
 *          The meter in a slider's channel
 */

static LRESULT draw_slider( NMCUSTOMDRAW *draw, float peak )
{
    if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->dwDrawStage == CDDS_ITEMPREPAINT && draw->dwItemSpec == TBCD_CHANNEL)
    {
        RECT rect = draw->rc, fill;
        int cx = (rect.left + rect.right) / 2, half = max( 3, (rect.right - rect.left) / 2 + 1 );
        HBRUSH green = CreateSolidBrush( RGB( 0x2b, 0xc4, 0x3d ) );

        rect.left = cx - half;
        rect.right = cx + half;
        FillRect( draw->hdc, &rect, GetSysColorBrush( COLOR_BTNFACE ) );
        DrawEdge( draw->hdc, &rect, BDR_SUNKENOUTER, BF_RECT );
        InflateRect( &rect, -1, -1 );
        fill = rect;
        fill.top = rect.bottom - (int)((rect.bottom - rect.top) * min( 1.f, peak ) + 0.5f);
        if (fill.top < fill.bottom) FillRect( draw->hdc, &fill, green );
        DeleteObject( green );
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

static void set_slider( HWND slider, float level )
{
    /* a vertical slider has its minimum on top */
    if (GetCapture() == slider) return;
    SendMessageW( slider, TBM_SETPOS, TRUE, 100 - (int)(level * 100.f + 0.5f) );
}

static float slider_level( HWND slider )
{
    return (100 - SendMessageW( slider, TBM_GETPOS, 0, 0 )) / 100.f;
}

static void prepare_slider( HWND slider )
{
    SendMessageW( slider, TBM_SETRANGE, FALSE, MAKELPARAM( 0, 100 ) );
    SendMessageW( slider, TBM_SETPAGESIZE, 0, 10 );
    SendMessageW( slider, TBM_SETLINESIZE, 0, 2 );
}

/**********************************************************************
 *          A program's column
 */

static INT_PTR CALLBACK column_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct column *col = (struct column *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW( dlg, DWLP_USER, lp );
        prepare_slider( GetDlgItem( dlg, IDC_SLIDER ) );
        return FALSE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wp, GetSysColor( COLOR_WINDOW ) );
        return (INT_PTR)GetSysColorBrush( COLOR_WINDOW );
    case WM_VSCROLL:
        if (col && col->row >= 0 && (HWND)lp == GetDlgItem( dlg, IDC_SLIDER ))
        {
            float level = slider_level( (HWND)lp );
            set_row( col->row, &level, NULL );
        }
        return TRUE;
    case WM_COMMAND:
        if (col && col->row >= 0 && LOWORD( wp ) == IDC_MUTE && HIWORD( wp ) == BN_CLICKED)
        {
            BOOL mute = !shared->rows[col->row].mute;
            set_row( col->row, NULL, &mute );
            SendDlgItemMessageW( dlg, IDC_MUTE, BM_SETIMAGE, IMAGE_ICON, (LPARAM)(mute ? muted_icon : speaker_icon) );
        }
        return TRUE;
    case WM_NOTIFY:
        if (col && ((NMHDR *)lp)->code == NM_CUSTOMDRAW && ((NMHDR *)lp)->idFrom == IDC_SLIDER)
        {
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, draw_slider( (NMCUSTOMDRAW *)lp, col->shown ) );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void destroy_columns(void)
{
    UINT i;

    for (i = 0; i < column_count; i++)
    {
        DestroyWindow( columns[i].dlg );
        if (columns[i].icon && !columns[i].system) DestroyIcon( columns[i].icon );
    }
    column_count = 0;
}

static void add_column( int row, BOOL system )
{
    struct column *col = &columns[column_count];
    const WCHAR *name;
    WCHAR file[64];

    memset( col, 0, sizeof(*col) );
    col->row = row;
    col->system = system;
    if (row >= 0)
    {
        col->pid = shared->rows[row].pid;
        col->session = shared->rows[row].session;
    }
    if (!(col->dlg = CreateDialogParamW( instance, MAKEINTRESOURCEW( IDD_APP ), mixer, column_proc, (LPARAM)col )))
        return;
    if (system)
    {
        name = load_string( IDS_SYSTEM_SOUNDS );
        col->icon = LoadImageW( instance, MAKEINTRESOURCEW( IDI_SNDVOL ), IMAGE_ICON, GetSystemMetrics( SM_CXICON ),
                                GetSystemMetrics( SM_CYICON ), LR_SHARED );
    }
    else
    {
        name = shared->rows[row].name;
        if (!name[0])
        {
            const WCHAR *slash = wcsrchr( shared->rows[row].icon, '\\' );
            lstrcpynW( file, slash ? slash + 1 : shared->rows[row].icon, ARRAY_SIZE(file) );
            name = file;
        }
        col->icon = program_icon( shared->rows[row].icon );
    }
    SetDlgItemTextW( col->dlg, IDC_APP_NAME, name );
    SendDlgItemMessageW( col->dlg, IDC_APP_ICON, STM_SETICON, (WPARAM)col->icon, 0 );
    if (row < 0)
    {
        EnableWindow( GetDlgItem( col->dlg, IDC_SLIDER ), FALSE );
        EnableWindow( GetDlgItem( col->dlg, IDC_MUTE ), FALSE );
        set_slider( GetDlgItem( col->dlg, IDC_SLIDER ), 1.f );
    }
    SendDlgItemMessageW( col->dlg, IDC_MUTE, BM_SETIMAGE, IMAGE_ICON,
                         (LPARAM)(row >= 0 && shared->rows[row].mute ? muted_icon : speaker_icon) );
    column_count++;
}

/* the window as wide as its columns, as Windows' grows */
static void layout(void)
{
    RECT area = { 73, 18, 136, 157 }, group, window, client, work;
    int width, x, i, extra, frame_w;

    MapDialogRect( mixer, &area );
    width = area.right - area.left;
    for (i = 0; i < (int)column_count; i++)
    {
        x = area.left + i * width;
        SetWindowPos( columns[i].dlg, NULL, x, area.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW );
    }
    group = (RECT){ 71, 6, 0, 159 };
    MapDialogRect( mixer, &group );
    extra = area.left - group.left;
    group.right = area.left + max( 3, (int)column_count ) * width + extra;
    SetWindowPos( GetDlgItem( mixer, IDC_APPS ), NULL, group.left, group.top, group.right - group.left,
                  group.bottom - group.top, SWP_NOZORDER );
    GetWindowRect( mixer, &window );
    GetClientRect( mixer, &client );
    frame_w = (window.right - window.left) - client.right;
    x = group.right + extra + frame_w;
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    x = min( x, work.right - work.left );
    /* it stays by the notification area, where the volume icon is */
    SetWindowPos( mixer, NULL, work.right - x - 8, window.top, x, window.bottom - window.top, SWP_NOZORDER );
    InvalidateRect( mixer, NULL, TRUE );
}

static void rebuild(void)
{
    UINT i;
    BOOL system = FALSE;

    destroy_columns();
    if (!shared) return;
    /* System Sounds first, then the programs as they came */
    for (i = 0; i < ARCTIC_AUDIO_SESSIONS && column_count < MAX_COLUMNS; i++)
    {
        if (!row_shown( i ) || !row_system( i ) || system) continue;
        add_column( i, TRUE );
        system = TRUE;
    }
    if (!system) add_column( -1, TRUE );
    for (i = 0; i < ARCTIC_AUDIO_SESSIONS && column_count < MAX_COLUMNS; i++)
        if (row_shown( i ) && !row_system( i )) add_column( i, FALSE );
    layout();
}

/* does the list of columns still match the rows? */
static BOOL columns_stale(void)
{
    UINT i, n = 0;
    BOOL system_row = FALSE;

    for (i = 0; i < ARCTIC_AUDIO_SESSIONS; i++)
    {
        if (!row_shown( i )) continue;
        if (row_system( i ))
        {
            if (system_row) continue;
            system_row = TRUE;
            if (!column_count || columns[0].row != (int)i) return TRUE;
            continue;
        }
        n++;
        if (n >= column_count || columns[n].row != (int)i || columns[n].pid != shared->rows[i].pid) return TRUE;
    }
    if (!system_row && column_count && columns[0].row >= 0) return TRUE;
    return n + 1 != column_count;
}

/**********************************************************************
 *          The device
 */

static void choose_device( const WCHAR *id )
{
    IMMDevice *device = NULL;
    WCHAR friendly[256] = L"", desc[128] = L"", title[320], *brace;

    if (volume)
    {
        IAudioEndpointVolume_Release( volume );
        volume = NULL;
    }
    device_id[0] = 0;
    if (id) IMMDeviceEnumerator_GetDevice( devices, id, &device );
    if (!device) IMMDeviceEnumerator_GetDefaultAudioEndpoint( devices, eRender, eMultimedia, &device );
    ShowWindow( GetDlgItem( mixer, IDC_NO_DEVICE ), device ? SW_HIDE : SW_SHOW );
    if (!device)
    {
        SetWindowTextW( mixer, load_string( IDS_TITLE ) );
        return;
    }
    if (SUCCEEDED(IMMDevice_GetId( device, &brace )))
    {
        lstrcpynW( device_id, brace, ARRAY_SIZE(device_id) );
        CoTaskMemFree( brace );
    }
    if ((brace = wcsrchr( device_id, '{' ))) CLSIDFromString( brace, &device_guid );
    IMMDevice_Activate( device, &iid_endpoint_volume, CLSCTX_INPROC_SERVER, NULL, (void **)&volume );
    get_string( device, &key_friendly_name, friendly, ARRAY_SIZE(friendly) );
    get_string( device, &key_device_desc, desc, ARRAY_SIZE(desc) );
    translate( friendly, ARRAY_SIZE(friendly) );
    translate( desc, ARRAY_SIZE(desc) );
    swprintf( title, ARRAY_SIZE(title), load_string( IDS_TITLE_DEVICE ), friendly );
    SetWindowTextW( mixer, title );
    SetDlgItemTextW( mixer, IDC_DEVICE_NAME, desc[0] ? desc : friendly );
    SendDlgItemMessageW( mixer, IDC_DEVICE_ICON, BM_SETIMAGE, IMAGE_ICON,
                         (LPARAM)LoadImageW( instance, MAKEINTRESOURCEW( device_icon_id( device ) ), IMAGE_ICON,
                                             GetSystemMetrics( SM_CXICON ), GetSystemMetrics( SM_CYICON ), LR_SHARED ) );
    IMMDevice_Release( device );
    look_at_processes();
    rebuild();
}

/* the device button: the outputs to show */
static void device_menu(void)
{
    IMMDeviceCollection *list;
    HMENU menu = CreatePopupMenu();
    WCHAR *ids[32] = { 0 }, name[256];
    UINT count = 0, i;
    RECT rect;
    int chosen;

    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints( devices, eRender, DEVICE_STATE_ACTIVE, &list )))
    {
        IMMDeviceCollection_GetCount( list, &count );
        for (i = 0; i < count && i < ARRAY_SIZE(ids); i++)
        {
            IMMDevice *device;

            if (FAILED(IMMDeviceCollection_Item( list, i, &device ))) continue;
            IMMDevice_GetId( device, &ids[i] );
            get_string( device, &key_friendly_name, name, ARRAY_SIZE(name) );
            translate( name, ARRAY_SIZE(name) );
            AppendMenuW( menu, MF_STRING | (ids[i] && !wcscmp( ids[i], device_id ) ? MF_CHECKED : 0), i + 1, name );
            IMMDevice_Release( device );
        }
        IMMDeviceCollection_Release( list );
    }
    GetWindowRect( GetDlgItem( mixer, IDC_DEVICE_ICON ), &rect );
    chosen = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_LEFTALIGN, rect.left, rect.bottom, 0, mixer, NULL );
    if (chosen > 0 && ids[chosen - 1]) choose_device( ids[chosen - 1] );
    for (i = 0; i < ARRAY_SIZE(ids); i++) CoTaskMemFree( ids[i] );
    DestroyMenu( menu );
}

static void update(void)
{
    float level = 0.f, loudest = 0.f;
    BOOL mute = FALSE;
    UINT i;

    if (!shared) open_shared();
    if (!(++tick % LIST_EVERY)) look_at_processes();
    if (shared && columns_stale()) rebuild();

    for (i = 0; i < column_count; i++)
    {
        struct column *col = &columns[i];
        float peak = 0.f;

        if (col->row >= 0)
        {
            struct arctic_audio_session *row = &shared->rows[col->row];

            peak = row->peak;
            row->peak = 0.f;
            set_slider( GetDlgItem( col->dlg, IDC_SLIDER ), row->volume );
            SendDlgItemMessageW( col->dlg, IDC_MUTE, BM_SETIMAGE, IMAGE_ICON,
                                 (LPARAM)(row->mute ? muted_icon : speaker_icon) );
        }
        loudest = max( loudest, peak );
        peak = max( peak, col->shown * 0.82f );
        if (fabsf( peak - col->shown ) > 0.005f)
        {
            col->shown = peak;
            InvalidateRect( GetDlgItem( col->dlg, IDC_SLIDER ), NULL, FALSE );
        }
    }
    if (volume)
    {
        IAudioEndpointVolume_GetMasterVolumeLevelScalar( volume, &level );
        IAudioEndpointVolume_GetMute( volume, &mute );
        set_slider( GetDlgItem( mixer, IDC_SLIDER ), level );
        SendDlgItemMessageW( mixer, IDC_MUTE, BM_SETIMAGE, IMAGE_ICON, (LPARAM)(mute ? muted_icon : speaker_icon) );
        /* what the device plays is what its programs play, after its own volume */
        loudest = mute ? 0.f : min( 1.f, loudest * level );
    }
    loudest = max( loudest, device_shown * 0.82f );
    if (fabsf( loudest - device_shown ) > 0.005f)
    {
        device_shown = loudest;
        InvalidateRect( GetDlgItem( mixer, IDC_SLIDER ), NULL, FALSE );
    }
}

static INT_PTR CALLBACK mixer_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        mixer = dlg;
        SetPropW( dlg, L"ArcticSndVol", (HANDLE)1 );
        SendMessageW( dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW( instance, MAKEINTRESOURCEW( IDI_SNDVOL ),
                      IMAGE_ICON, GetSystemMetrics( SM_CXICON ), GetSystemMetrics( SM_CYICON ), LR_SHARED ) );
        SendMessageW( dlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW( instance, MAKEINTRESOURCEW( IDI_SNDVOL ),
                      IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), LR_SHARED ) );
        prepare_slider( GetDlgItem( dlg, IDC_SLIDER ) );
        open_shared();
        choose_device( NULL );
        update();
        SetTimer( dlg, TIMER_METER, METER_MS, NULL );
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_METER) update();
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wp, GetSysColor( COLOR_WINDOW ) );
        return (INT_PTR)GetSysColorBrush( COLOR_WINDOW );
    case WM_VSCROLL:
        if (volume && (HWND)lp == GetDlgItem( dlg, IDC_SLIDER ))
            IAudioEndpointVolume_SetMasterVolumeLevelScalar( volume, slider_level( (HWND)lp ), NULL );
        return TRUE;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW && ((NMHDR *)lp)->idFrom == IDC_SLIDER)
        {
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, draw_slider( (NMCUSTOMDRAW *)lp, device_shown ) );
            return TRUE;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_MUTE:
            if (volume)
            {
                BOOL mute = FALSE;
                IAudioEndpointVolume_GetMute( volume, &mute );
                IAudioEndpointVolume_SetMute( volume, !mute, NULL );
            }
            return TRUE;
        case IDC_DEVICE_ICON:
            device_menu();
            return TRUE;
        case IDCANCEL:
            DestroyWindow( dlg );
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow( dlg );
        return TRUE;
    case WM_DESTROY:
        KillTimer( dlg, TIMER_METER );
        destroy_columns();
        PostQuitMessage( 0 );
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK find_mixer( HWND hwnd, LPARAM lp )
{
    if (!GetPropW( hwnd, L"ArcticSndVol" )) return TRUE;
    *(HWND *)lp = hwnd;
    return FALSE;
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    HANDLE once = CreateMutexW( NULL, FALSE, L"ArcticSndVol" );
    HWND dlg = NULL;
    MSG msg;

    /* one mixer, as Windows has: a second start brings the first forward */
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        EnumWindows( find_mixer, (LPARAM)&dlg );
        if (dlg)
        {
            ShowWindow( dlg, SW_RESTORE );
            SetForegroundWindow( dlg );
        }
        return 0;
    }
    instance = inst;
    SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
    InitCommonControlsEx( &icc );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    if (FAILED(CoCreateInstance( &clsid_enumerator, NULL, CLSCTX_INPROC_SERVER, &iid_enumerator,
                                 (void **)&devices )))
        return 1;
    speaker_icon = make_speaker( FALSE );
    muted_icon = make_speaker( TRUE );
    if (!(dlg = CreateDialogParamW( inst, MAKEINTRESOURCEW( IDD_MIXER ), NULL, mixer_proc, 0 ))) return 1;
    ShowWindow( dlg, show );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (IsDialogMessageW( dlg, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    if (volume) IAudioEndpointVolume_Release( volume );
    IMMDeviceEnumerator_Release( devices );
    CoUninitialize();
    CloseHandle( once );
    return 0;
}
