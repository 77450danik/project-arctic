/*
 * Paint: Resize and skew, Image properties
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "paint.h"

static int base_w, base_h;
static BOOL updating;

static int get_int( HWND dlg, int id )
{
    WCHAR buf[32];
    GetDlgItemTextW( dlg, id, buf, ARRAY_SIZE(buf) );
    return _wtoi( buf );
}

static void set_int( HWND dlg, int id, int value )
{
    WCHAR buf[32];
    swprintf( buf, ARRAY_SIZE(buf), L"%d", value );
    updating = TRUE;
    SetDlgItemTextW( dlg, id, buf );
    updating = FALSE;
}

static void set_texts( HWND dlg, const int *ids, const WCHAR *const *texts, int count )
{
    for (int i = 0; i < count; i++) SetDlgItemTextW( dlg, ids[i], texts[i] );
}

static INT_PTR CALLBACK resize_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    BOOL pixels = IsDlgButtonChecked( dlg, IDC_BY_PIXELS ) == BST_CHECKED;

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        static const int ids[] = { 100, IDC_BY_PERCENT, IDC_BY_PIXELS, 101, 102, IDC_KEEP_RATIO, 103, 104, 105, IDOK, IDCANCEL };
        const WCHAR *texts[] =
        {
            tr( L"Resize", L"Змінити розмір" ), tr( L"Percentage", L"У відсотках" ), tr( L"Pixels", L"У пікселях" ),
            tr( L"Horizontal:", L"По горизонталі:" ), tr( L"Vertical:", L"По вертикалі:" ),
            tr( L"Maintain aspect ratio", L"Зберігати пропорції" ), tr( L"Skew (degrees)", L"Нахил (градуси)" ),
            tr( L"Horizontal:", L"По горизонталі:" ), tr( L"Vertical:", L"По вертикалі:" ), L"OK", tr( L"Cancel", L"Скасувати" ),
        };
        SetWindowTextW( dlg, tr( L"Resize and skew", L"Змінити розмір і нахил" ) );
        set_texts( dlg, ids, texts, ARRAY_SIZE(ids) );
        canvas_selection_size( &base_w, &base_h );
        CheckRadioButton( dlg, IDC_BY_PERCENT, IDC_BY_PIXELS, IDC_BY_PERCENT );
        CheckDlgButton( dlg, IDC_KEEP_RATIO, BST_CHECKED );
        set_int( dlg, IDC_HORZ, 100 );
        set_int( dlg, IDC_VERT, 100 );
        set_int( dlg, IDC_SKEW_H, 0 );
        set_int( dlg, IDC_SKEW_V, 0 );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case IDC_BY_PERCENT:
        case IDC_BY_PIXELS:
            if (HIWORD( wparam ) != BN_CLICKED) break;
            if (pixels)
            {
                set_int( dlg, IDC_HORZ, base_w );
                set_int( dlg, IDC_VERT, base_h );
            }
            else
            {
                set_int( dlg, IDC_HORZ, 100 );
                set_int( dlg, IDC_VERT, 100 );
            }
            break;
        case IDC_HORZ:
        case IDC_VERT:
            if (HIWORD( wparam ) != EN_CHANGE || updating || IsDlgButtonChecked( dlg, IDC_KEEP_RATIO ) != BST_CHECKED) break;
            if (LOWORD( wparam ) == IDC_HORZ)
            {
                int v = get_int( dlg, IDC_HORZ );
                set_int( dlg, IDC_VERT, pixels ? MulDiv( v, base_h, max( base_w, 1 ) ) : v );
            }
            else
            {
                int v = get_int( dlg, IDC_VERT );
                set_int( dlg, IDC_HORZ, pixels ? MulDiv( v, base_w, max( base_h, 1 ) ) : v );
            }
            break;
        case IDOK:
        {
            int hz = get_int( dlg, IDC_HORZ ), vt = get_int( dlg, IDC_VERT );
            int sh = get_int( dlg, IDC_SKEW_H ), sv = get_int( dlg, IDC_SKEW_V );
            int w = pixels ? hz : MulDiv( base_w, hz, 100 ), h = pixels ? vt : MulDiv( base_h, vt, 100 );
            if (w < 1 || h < 1 || w > 30000 || h > 30000 || abs( sh ) > 89 || abs( sv ) > 89)
            {
                MessageBeep( MB_ICONWARNING );
                return TRUE;
            }
            EndDialog( dlg, IDOK );
            if (w != base_w || h != base_h || sh || sv) canvas_resize_skew( w, h, sh, sv );
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

void dialog_resize( HWND owner )
{
    DialogBoxW( instance, MAKEINTRESOURCEW( IDD_RESIZE ), owner, resize_proc );
}

static INT_PTR CALLBACK properties_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        static const int ids[] = { 100, 101, IDOK, IDCANCEL };
        const WCHAR *texts[] = { tr( L"Width (pixels):", L"Ширина (пікселі):" ), tr( L"Height (pixels):", L"Висота (пікселі):" ),
                                 L"OK", tr( L"Cancel", L"Скасувати" ) };
        SetWindowTextW( dlg, tr( L"Image properties", L"Властивості зображення" ) );
        set_texts( dlg, ids, texts, ARRAY_SIZE(ids) );
        set_int( dlg, IDC_WIDTH, pic->width );
        set_int( dlg, IDC_HEIGHT, pic->height );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD( wparam ) == IDOK)
        {
            int w = get_int( dlg, IDC_WIDTH ), h = get_int( dlg, IDC_HEIGHT );
            struct image *im;
            if (w < 1 || h < 1 || w > 30000 || h > 30000)
            {
                MessageBeep( MB_ICONWARNING );
                return TRUE;
            }
            EndDialog( dlg, IDOK );
            if ((w != pic->width || h != pic->height) && (im = image_create( w, h, 0 )))
            {
                for (int y = 0; y < min( h, pic->height ); y++)
                    memcpy( im->bits + (size_t)y * w, pic->bits + (size_t)y * pic->width, min( w, pic->width ) * 4 );
                history_push();
                set_picture( im );
            }
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

void dialog_properties( HWND owner )
{
    DialogBoxW( instance, MAKEINTRESOURCEW( IDD_PROPERTIES ), owner, properties_proc );
}
