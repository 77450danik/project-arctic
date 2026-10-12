/*
 * UserAccountControlSettings.exe: the settings of User Account Control
 *
 * Reached from "Облікові записи користувачів" of the Control Panel
 * (usercpl.dll). In Windows this window holds the slider of the four
 * notification levels; here it holds what Arctic's User Account Control
 * guards today, the Linux subsystem (docs/linux-gate.md): whether a password
 * is required for it, and the password itself.
 *
 * It is an ordinary dialog of the system - the frame, the caption and the
 * controls are Windows', in the visual style of the system.
 *
 * Nothing is applied until OK, and applying always goes through the consent
 * window with the password that is set now - as changing the settings of UAC
 * in Windows always asks, whatever consent the program already has. The host
 * (arctic-lxss, root) is what asks and what writes: gate.c speaks to it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"
#include "gate.h"

static BOOL required;   /* what the gate says now */

static const WCHAR *load( UINT id )
{
    static WCHAR text[4][1024];
    static int next;
    WCHAR *out = text[next++ & 3];

    if (!LoadStringW( GetModuleHandleW( NULL ), id, out, 1024 )) out[0] = 0;
    return out;
}

static void say( HWND owner, UINT id, UINT type )
{
    MessageBoxW( owner, load( id ), load( IDS_TITLE ), type | MB_OK );
}

/* "The operation was canceled by the user.", in the language of the system */
static void cancelled( HWND owner )
{
    WCHAR *text = NULL;

    if (FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                        NULL, ERROR_CANCELLED, 0, (WCHAR *)&text, 0, NULL ) && text)
    {
        MessageBoxW( owner, text, load( IDS_TITLE ), MB_ICONINFORMATION | MB_OK );
        LocalFree( text );
    }
    else say( owner, IDS_NOT_CHANGED, MB_ICONINFORMATION );
}

static void report( HWND owner, enum gate_result result )
{
    if (result == GATE_DENIED) cancelled( owner );
    else if (result == GATE_ERROR) say( owner, IDS_NO_SUBSYSTEM, MB_ICONERROR );
}

/* ---- the password ---- */

static INT_PTR CALLBACK password_proc( HWND dialog, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextW( dialog, IDC_PASSWORD_NOTE, load( IDS_PASSWORD_NOTE ) );
        SendMessageW( GetDlgItem( dialog, IDOK ), BCM_SETSHIELD, 0, TRUE );
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL)
        {
            EndDialog( dialog, 0 );
            return TRUE;
        }
        if (LOWORD(wp) == IDOK)
        {
            WCHAR new_password[256] = L"", confirm[256] = L"";
            enum gate_result result;

            GetDlgItemTextW( dialog, IDC_NEW_PASSWORD, new_password, ARRAY_SIZE(new_password) );
            GetDlgItemTextW( dialog, IDC_CONFIRM_PASSWORD, confirm, ARRAY_SIZE(confirm) );
            if (!new_password[0])
            {
                say( dialog, IDS_PASSWORD_EMPTY, MB_ICONWARNING );
                SetFocus( GetDlgItem( dialog, IDC_NEW_PASSWORD ) );
                return TRUE;
            }
            if (wcscmp( new_password, confirm ))
            {
                say( dialog, IDS_PASSWORDS_DIFFER, MB_ICONWARNING );
                SetDlgItemTextW( dialog, IDC_NEW_PASSWORD, L"" );
                SetDlgItemTextW( dialog, IDC_CONFIRM_PASSWORD, L"" );
                SetFocus( GetDlgItem( dialog, IDC_NEW_PASSWORD ) );
                return TRUE;
            }
            /* the host asks for the password set now, then takes the new one */
            result = gate_set_password( new_password );
            SecureZeroMemory( new_password, sizeof(new_password) );
            SecureZeroMemory( confirm, sizeof(confirm) );
            if (result != GATE_OK)
            {
                report( dialog, result );
                return TRUE;
            }
            /* a password that is set is a password that is meant to be asked
             * for: the box is ticked, and OK applies it like any other change */
            CheckDlgButton( GetParent( dialog ), IDC_REQUIRE, BST_CHECKED );
            EndDialog( dialog, 1 );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- the settings ---- */

static void apply_and_close( HWND dialog )
{
    BOOL asked = IsDlgButtonChecked( dialog, IDC_REQUIRE ) == BST_CHECKED;

    if (asked != required)
    {
        enum gate_result result = gate_require_password( asked );

        if (result != GATE_OK)
        {
            report( dialog, result );
            CheckDlgButton( dialog, IDC_REQUIRE, required ? BST_CHECKED : BST_UNCHECKED );
            return;
        }
        required = asked;
    }
    EndDialog( dialog, 1 );
}

static INT_PTR CALLBACK settings_proc( HWND dialog, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextW( dialog, IDC_HEADING, load( IDS_HEADING ) );
        SetDlgItemTextW( dialog, IDC_ABOUT_UAC, load( IDS_ABOUT_UAC ) );
        SetDlgItemTextW( dialog, IDC_ABOUT_LINUX, load( IDS_ABOUT_LINUX ) );
        SetDlgItemTextW( dialog, IDC_GROUP, load( IDS_GROUP ) );
        SetDlgItemTextW( dialog, IDC_REQUIRE, load( IDS_REQUIRE_PASSWORD ) );
        SetDlgItemTextW( dialog, IDC_WHEN_ASKED, load( IDS_WHEN_ASKED ) );
        SetDlgItemTextW( dialog, IDC_CHANGE, load( IDS_CHANGE_PASSWORD ) );
        SetDlgItemTextW( dialog, IDOK, load( IDS_OK ) );
        SetDlgItemTextW( dialog, IDCANCEL, load( IDS_CANCEL ) );
        CheckDlgButton( dialog, IDC_REQUIRE, required ? BST_CHECKED : BST_UNCHECKED );
        /* both ask the host, which puts up the consent window: the shield, as
         * on every button of Windows that leads to an elevation */
        SendMessageW( GetDlgItem( dialog, IDC_CHANGE ), BCM_SETSHIELD, 0, TRUE );
        SendMessageW( GetDlgItem( dialog, IDOK ), BCM_SETSHIELD, 0, TRUE );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_CHANGE:
            DialogBoxParamW( GetModuleHandleW( NULL ), (const WCHAR *)MAKEINTRESOURCE(IDD_PASSWORD), dialog,
                             password_proc, 0 );
            return TRUE;
        case IDOK:
            apply_and_close( dialog );
            return TRUE;
        case IDCANCEL:
            EndDialog( dialog, 0 );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    enum gate_result state;

    InitCommonControls();
    state = gate_password_required( &required );
    if (state != GATE_OK)
    {
        report( NULL, state );
        return 1;
    }
    DialogBoxParamW( instance, (const WCHAR *)MAKEINTRESOURCE(IDD_SETTINGS), NULL, settings_proc, 0 );
    return 0;
}
