/*
 * Programs and Features of the Control Panel: the details of a program
 *
 * The pane under the list of Windows 7: the chosen program's icon and name,
 * then what it says of itself, in two columns of labels and values (what
 * appwiz.cpl's "Support Information" dialog showed). Its links open.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "programscpl.h"
#include "shellapi.h"
#include "shlwapi.h"

struct field
{
    UINT label;
    WCHAR value[512];
    BOOL link;
    RECT rect;               /* where the value stands, once drawn */
};

struct details
{
    BOOL shown;
    WCHAR name[256];
    HICON icon;
    struct field fields[12];
    UINT count;
    int hot;
};

static void add_field( struct details *d, UINT label, const WCHAR *value, BOOL link )
{
    struct field *f;

    if (!value || !value[0] || d->count >= ARRAY_SIZE(d->fields)) return;
    f = &d->fields[d->count++];
    f->label = label;
    lstrcpynW( f->value, value, ARRAY_SIZE(f->value) );
    f->link = link;
    SetRectEmpty( &f->rect );
}

static BOOL is_url( const WCHAR *text )
{
    return text && (!wcsnicmp( text, L"http://", 7 ) || !wcsnicmp( text, L"https://", 8 ) || !wcsnicmp( text, L"www.", 4 ) ||
                    !wcsnicmp( text, L"mailto:", 7 ));
}

static void set_app( struct details *d, const struct app *app )
{
    WCHAR text[64];

    if (d->icon) DestroyIcon( d->icon );
    d->icon = NULL;
    d->count = 0;
    d->hot = -1;
    if (!(d->shown = app != NULL)) return;
    lstrcpynW( d->name, app->name, ARRAY_SIZE(d->name) );
    if (app->icon) ExtractIconExW( app->icon, app->icon_index, &d->icon, NULL, 1 );
    add_field( d, IDS_PUBLISHER, app->publisher, FALSE );
    add_field( d, IDS_VERSION, app->version, FALSE );
    if (app->installed.wYear &&
        GetDateFormatW( LOCALE_USER_DEFAULT, DATE_SHORTDATE, &app->installed, NULL, text, ARRAY_SIZE(text) ))
        add_field( d, IDS_INSTALLED_ON, text, FALSE );
    if (app->size_kb && StrFormatByteSizeW( (LONGLONG)app->size_kb * 1024, text, ARRAY_SIZE(text) ))
        add_field( d, IDS_SIZE, text, FALSE );
    add_field( d, IDS_SUPPORT_LINK, app->about_link, is_url( app->about_link ) );
    add_field( d, IDS_HELP_LINK, app->help_link, is_url( app->help_link ) );
    add_field( d, IDS_UPDATE_LINK, app->update_link, is_url( app->update_link ) );
    add_field( d, IDS_CONTACT, app->contact, FALSE );
    add_field( d, IDS_PHONE, app->phone, FALSE );
    add_field( d, IDS_README, app->readme, TRUE );
    add_field( d, IDS_LOCATION, app->location, TRUE );
    add_field( d, IDS_COMMENTS, app->comments, FALSE );
}

static HFONT details_font( int weight, BOOL underline, int height )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfUnderline = underline;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

static void paint( HWND hwnd, struct details *d, HDC hdc )
{
    RECT client, rect;
    HBRUSH back = CreateSolidBrush( COLOR_CARD );  /* on the card it stands in */
    HFONT bold, body, link;
    int icon = px( 48 ), left, column, y, label_width = px( 150 );

    GetClientRect( hwnd, &client );
    FillRect( hdc, &client, back );
    DeleteObject( back );
    if (!d->shown) return;
    bold = details_font( FW_SEMIBOLD, FALSE, px( 15 ) );
    body = details_font( FW_NORMAL, FALSE, px( 12 ) );
    link = details_font( FW_NORMAL, TRUE, px( 12 ) );
    SetBkMode( hdc, TRANSPARENT );

    if (d->icon) DrawIconEx( hdc, px( 12 ), px( 12 ), d->icon, icon, icon, 0, NULL, DI_NORMAL );
    left = px( 12 ) + icon + px( 14 );
    SelectObject( hdc, bold );
    SetTextColor( hdc, COLOR_TEXT );
    SetRect( &rect, left, px( 10 ), client.right - px( 10 ), px( 32 ) );
    DrawTextW( hdc, d->name, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );

    /* two columns of label and value */
    column = max( (client.right - left - px( 10 )) / 2, label_width + px( 60 ) );
    y = px( 36 );
    for (UINT i = 0; i < d->count; i++)
    {
        struct field *f = &d->fields[i];
        int x = left + (int)(i % 2) * column, top = y + (int)(i / 2) * px( 18 );
        RECT label = { x, top, x + label_width, top + px( 18 ) };

        if (top + px( 18 ) > client.bottom) break;
        SelectObject( hdc, body );
        SetTextColor( hdc, COLOR_GRAY );
        DrawTextW( hdc, load_string( f->label ), -1, &label, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
        SetRect( &f->rect, x + label_width, top, x + column - px( 10 ), top + px( 18 ) );
        SelectObject( hdc, f->link && d->hot == (int)i ? link : body );
        SetTextColor( hdc, f->link ? COLOR_LINK : COLOR_TEXT );
        DrawTextW( hdc, f->value, -1, &f->rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
    }
    DeleteObject( bold );
    DeleteObject( body );
    DeleteObject( link );
}

static int field_at( struct details *d, POINT pt )
{
    for (UINT i = 0; i < d->count; i++)
        if (d->fields[i].link && PtInRect( &d->fields[i].rect, pt )) return i;
    return -1;
}

static void open_field( const struct field *f )
{
    WCHAR target[600];

    if (!wcsnicmp( f->value, L"www.", 4 )) swprintf( target, ARRAY_SIZE(target), L"http://%s", f->value );
    else lstrcpynW( target, f->value, ARRAY_SIZE(target) );
    ShellExecuteW( NULL, NULL, target, NULL, NULL, SW_SHOWNORMAL );
}

static LRESULT WINAPI details_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct details *d = (struct details *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(d = calloc( 1, sizeof(*d) ))) return FALSE;
        d->hot = -1;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)d );
        break;
    case WM_NCDESTROY:
        if (d && d->icon) DestroyIcon( d->icon );
        free( d );
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        break;
    case DM_SETAPP:
        set_app( d, (const struct app *)lp );
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps ), mem = CreateCompatibleDC( hdc );
        RECT client;
        HBITMAP bitmap;

        GetClientRect( hwnd, &client );
        if ((bitmap = CreateCompatibleBitmap( hdc, max( client.right, 1 ), max( client.bottom, 1 ) )))
        {
            HGDIOBJ old = SelectObject( mem, bitmap );
            paint( hwnd, d, mem );
            BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int hot = field_at( d, pt );

        if (hot != d->hot)
        {
            d->hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (d->hot >= 0)
        {
            d->hot = -1;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_SETCURSOR:
        if (d->hot >= 0)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_HAND ) );
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        int index = field_at( d, pt );

        if (index >= 0) open_field( &d->fields[index] );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

void details_register(void)
{
    static BOOL done;
    WNDCLASSW cls = { 0 };

    if (done) return;
    cls.lpfnWndProc = details_proc;
    cls.hInstance = cp_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = DETAILS_CLASS;
    done = RegisterClassW( &cls ) != 0;
}
