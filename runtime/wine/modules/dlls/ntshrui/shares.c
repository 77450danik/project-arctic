/*
 * The shares: one <name>.share file each under C:\ProgramData\Arctic\Smb
 * \shares, which arctic-smbd (the host) watches and serves at once
 * (host/init/arctic-smbd.c, docs/M5-network.md). Only this side writes them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntshrui.h"
#include "shlobj.h"
#include "shellapi.h"

#define SHARES_DIR L"C:\\ProgramData\\Arctic\\Smb\\shares"

static const WCHAR *const rights[] = { L"none", L"read", L"change", L"full" };
static const WCHAR *const cachings[] = { L"manual", L"auto", L"vdo", L"none" };

static int word_index( const WCHAR *word, const WCHAR *const *words, int fallback )
{
    for (int i = 0; i < 4; i++)
        if (!wcscmp( word, words[i] )) return i;
    return fallback;
}

/* what Windows does not let the name of a share have */
const WCHAR *share_bad_characters( const WCHAR *name )
{
    static const WCHAR bad[] = L"\\ / : * ? \" < > | [ ] + = ; ,";

    for (; *name; name++)
        if (*name < ' ' || (*name != ' ' && wcschr( bad, *name ))) return bad;
    return NULL;
}

static BOOL read_share( const WCHAR *file, WCHAR *folder, UINT count, struct share *share )
{
    WCHAR path[MAX_PATH], *text, *line, *next;
    char *raw;
    DWORD size, got = 0;
    HANDLE handle;
    UINT len;

    swprintf( path, ARRAY_SIZE(path), SHARES_DIR L"\\%s", file );
    handle = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, 0, NULL );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize( handle, NULL );
    if (size > 65536 || !(raw = malloc( size + 1 )))
    {
        CloseHandle( handle );
        return FALSE;
    }
    if (!ReadFile( handle, raw, size, &got, NULL )) got = 0;
    CloseHandle( handle );
    if (!(text = malloc( (got + 1) * sizeof(WCHAR) )))
    {
        free( raw );
        return FALSE;
    }
    text[MultiByteToWideChar( CP_UTF8, 0, raw, got, text, got )] = 0;
    free( raw );

    memset( share, 0, sizeof(*share) );
    lstrcpynW( share->name, file, ARRAY_SIZE(share->name) );
    if ((len = wcslen( share->name )) > 6 && !lstrcmpiW( share->name + len - 6, L".share" )) share->name[len - 6] = 0;
    share->allow = RIGHT_READ;
    share->limit = MAX_LIMIT;
    folder[0] = 0;
    for (line = text; *line; line = next)
    {
        if ((next = wcspbrk( line, L"\r\n" ))) *next++ = 0;
        else next = line + wcslen( line );
        if (!wcsncmp( line, L"path=", 5 )) lstrcpynW( folder, line + 5, count );
        else if (!wcsncmp( line, L"comment=", 8 )) lstrcpynW( share->comment, line + 8, ARRAY_SIZE(share->comment) );
        else if (!wcsncmp( line, L"allow=", 6 )) share->allow = word_index( line + 6, rights, RIGHT_NONE );
        else if (!wcsncmp( line, L"deny=", 5 )) share->deny = word_index( line + 5, rights, RIGHT_NONE );
        else if (!wcsncmp( line, L"limit=", 6 )) share->limit = wcstoul( line + 6, NULL, 10 );
        else if (!wcsncmp( line, L"caching=", 8 )) share->caching = word_index( line + 8, cachings, CACHE_MANUAL );
    }
    free( text );
    if (!share->limit || share->limit > MAX_LIMIT) share->limit = MAX_LIMIT;
    return folder[0] != 0;
}

static int compare_shares( const void *a, const void *b )
{
    return lstrcmpiW( ((const struct share *)a)->name, ((const struct share *)b)->name );
}

/* the shares a folder is given out as: usually none or one */
UINT shares_of_folder( const WCHAR *folder, struct share *list, UINT max )
{
    WIN32_FIND_DATAW found;
    WCHAR theirs[MAX_PATH];
    HANDLE find;
    UINT count = 0;

    if ((find = FindFirstFileW( SHARES_DIR L"\\*.share", &found )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (count < max && read_share( found.cFileName, theirs, ARRAY_SIZE(theirs), &list[count] ) &&
            !lstrcmpiW( theirs, folder ))
            count++;
    } while (FindNextFileW( find, &found ));
    FindClose( find );
    qsort( list, count, sizeof(*list), compare_shares );
    return count;
}

/* the folder a name is taken by */
BOOL share_owner( const WCHAR *name, WCHAR *folder, UINT count )
{
    WCHAR file[100];
    struct share share;

    if (!lstrcmpiW( name, L"IPC$" ))
    {
        folder[0] = 0;
        return TRUE; /* the pipes of the server: nobody's folder */
    }
    swprintf( file, ARRAY_SIZE(file), L"%s.share", name );
    return read_share( file, folder, count, &share );
}

/* written beside and moved over: the host reads a whole file or the one before */
BOOL share_write( const WCHAR *folder, const struct share *share )
{
    WCHAR path[MAX_PATH], temp[MAX_PATH], text[1024];
    char utf8[4096];
    DWORD written;
    HANDLE handle;
    int len;

    SHCreateDirectoryExW( NULL, SHARES_DIR, NULL );
    swprintf( path, ARRAY_SIZE(path), SHARES_DIR L"\\%s.share", share->name );
    swprintf( temp, ARRAY_SIZE(temp), SHARES_DIR L"\\%s.share.new", share->name );
    swprintf( text, ARRAY_SIZE(text), L"path=%s\r\ncomment=%s\r\nallow=%s\r\ndeny=%s\r\nlimit=%u\r\ncaching=%s\r\n",
              folder, share->comment, rights[share->allow], rights[share->deny], share->limit,
              cachings[share->caching] );
    if (!(len = WideCharToMultiByte( CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL ))) return FALSE;
    handle = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile( handle, utf8, len - 1, &written, NULL )) written = 0;
    CloseHandle( handle );
    if (written != (DWORD)len - 1 || !MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING ))
    {
        DeleteFileW( temp );
        return FALSE;
    }
    return TRUE;
}

void share_delete( const WCHAR *name )
{
    WCHAR path[MAX_PATH];

    swprintf( path, ARRAY_SIZE(path), SHARES_DIR L"\\%s.share", name );
    DeleteFileW( path );
}

/* "Password protected sharing" of the Network and Sharing Center */
BOOL password_protected(void)
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                  L"PasswordProtectedSharing", RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

/* A folder was shared: file and printer sharing is on from now on, as Windows
 * turns it on with the first share. The Network and Sharing Center tells the host. */
void file_sharing_on(void)
{
    static const WCHAR key[] = L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters";
    DWORD value = 0, size = sizeof(value);
    HKEY hkey;

    RegGetValueW( HKEY_LOCAL_MACHINE, key, L"FileSharing", RRF_RT_REG_DWORD, NULL, &value, &size );
    if (value) return;
    value = 1;
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &hkey, NULL )) return;
    RegSetValueExW( hkey, L"FileSharing", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( hkey );
    ShellExecuteW( NULL, NULL, L"rundll32.exe", L"netcenter.dll,ApplySharingSettings", NULL, SW_HIDE );
}
