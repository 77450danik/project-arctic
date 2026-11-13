/*
 * GetOpenFileName and GetSaveFileName through the Common Item Dialog: the
 * system has one Open and one Save dialog, the Windows 10 one, and the old
 * API goes to it. From Arctic NT's comdlg32.
 *
 * The OPENFILENAME a program passes becomes the dialog's options, file types,
 * folder and name; what the user chose goes back into the structure the way
 * the documentation of OPENFILENAME has it (the directory and the names NUL
 * separated for several files, nFileOffset, nFileExtension, nFilterIndex,
 * lpstrFileTitle, the custom filter, OFN_READONLY, OFN_EXTENSIONDIFFERENT).
 * Only the dialogs of Windows 3.1's look (a hook or several files without
 * OFN_EXPLORER) stay the old ones, as in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "winreg.h"
#include "shlwapi.h"
#include "commdlg.h"
#include "cderr.h"
#include "cdlg.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(commdlg);

/* the Open button's drop-down when the program shows the read-only check box */
#define OFN_READONLY_CHOICES  1
#define OFN_CHOICE_OPEN       0
#define OFN_CHOICE_READONLY   1

/* The filter pairs "Text\0*.txt\0All\0*.*\0\0" as file types */
static UINT parse_filters(LPCWSTR filter, LPCWSTR custom, COMDLG_FILTERSPEC **specs)
{
    UINT count = 0, i = 0;
    LPCWSTR p;

    *specs = NULL;
    for (p = filter; p && *p; p += lstrlenW(p) + 1)
    {
        p += lstrlenW(p) + 1;
        if (!*p)
            break;
        count++;
    }
    if (custom && *custom)
        count++;
    if (!count)
        return 0;
    *specs = calloc(1, count * sizeof(**specs));
    if (!*specs)
        return 0;
    if (custom && *custom)
    {
        /* the custom filter, kept by the program from the last time, comes first */
        (*specs)[i].pszName = custom;
        (*specs)[i].pszSpec = custom + lstrlenW(custom) + 1;
        if (!*(*specs)[i].pszSpec)
            (*specs)[i].pszSpec = L"*.*";
        i++;
    }
    for (p = filter; p && *p && i < count; i++)
    {
        (*specs)[i].pszName = p;
        p += lstrlenW(p) + 1;
        (*specs)[i].pszSpec = p;
        p += lstrlenW(p) + 1;
    }
    return count;
}

static IShellItem *item_from_path(LPCWSTR path)
{
    IShellItem *psi = NULL;
    WCHAR full[MAX_PATH];

    if (!path || !*path)
        return NULL;
    if (!GetFullPathNameW(path, ARRAY_SIZE(full), full, NULL))
        return NULL;
    if (FAILED(SHCreateItemFromParsingName(full, NULL, &IID_IShellItem, (void **)&psi)))
        return NULL;
    return psi;
}

static FILEOPENDIALOGOPTIONS options_from_flags(DWORD flags, BOOL save)
{
    FILEOPENDIALOGOPTIONS fos = FOS_FORCEFILESYSTEM;

    if (flags & OFN_OVERWRITEPROMPT)    fos |= FOS_OVERWRITEPROMPT;
    if (flags & OFN_ALLOWMULTISELECT)   fos |= FOS_ALLOWMULTISELECT;
    if (flags & OFN_PATHMUSTEXIST)      fos |= FOS_PATHMUSTEXIST;
    if (flags & OFN_FILEMUSTEXIST)      fos |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (flags & OFN_CREATEPROMPT)       fos |= FOS_CREATEPROMPT;
    if (flags & OFN_SHAREAWARE)         fos |= FOS_SHAREAWARE;
    if (flags & OFN_NOREADONLYRETURN)   fos |= FOS_NOREADONLYRETURN;
    if (flags & OFN_NOTESTFILECREATE)   fos |= FOS_NOTESTFILECREATE;
    if (flags & OFN_NODEREFERENCELINKS) fos |= FOS_NODEREFERENCELINKS;
    if (flags & OFN_DONTADDTORECENT)    fos |= FOS_DONTADDTORECENT;
    if (flags & OFN_FORCESHOWHIDDEN)    fos |= FOS_FORCESHOWHIDDEN;
    if (flags & OFN_NOVALIDATE)         fos |= FOS_NOVALIDATE;
    /* the program's current directory stays as it is; with OFN_NOCHANGEDIR
     * clear, it is set to the chosen folder afterwards, as the old dialog did */
    fos |= FOS_NOCHANGEDIR;
    return fos;
}

/* The chosen path(s) into lpstrFile: one path, or the folder and the names */
static BOOL store_results(OPENFILENAMEW *ofn, IFileDialog *pfd, BOOL multi)
{
    IShellItemArray *psia = NULL;
    WCHAR *buf = NULL, *folder = NULL;
    DWORD count = 0, i, len = 0, used, offset;
    WCHAR *first = NULL;
    BOOL ret = FALSE;

    if (multi)
    {
        IFileOpenDialog *pfod;
        if (SUCCEEDED(IFileDialog_QueryInterface(pfd, &IID_IFileOpenDialog, (void **)&pfod)))
        {
            IFileOpenDialog_GetResults(pfod, &psia);
            IFileOpenDialog_Release(pfod);
        }
        if (psia && FAILED(IShellItemArray_GetCount(psia, &count)))
            count = 0;
    }

    if (count <= 1)
    {
        IShellItem *psi = NULL;
        if (psia)
            IShellItemArray_GetItemAt(psia, 0, &psi);
        else
            IFileDialog_GetResult(pfd, &psi);
        if (!psi || FAILED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &first)))
        {
            if (psi) IShellItem_Release(psi);
            if (psia) IShellItemArray_Release(psia);
            COMDLG32_SetCommDlgExtendedError(CDERR_GENERALCODES);
            return FALSE;
        }
        IShellItem_Release(psi);
        len = lstrlenW(first);
        if (len + 2 > ofn->nMaxFile)
        {
            if (ofn->lpstrFile && ofn->nMaxFile >= 1)
                *(WORD *)ofn->lpstrFile = (WORD)(len + 2);
            COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
            goto done;
        }
        lstrcpyW(ofn->lpstrFile, first);
        ofn->lpstrFile[len + 1] = 0;
        {
            WCHAR *name = PathFindFileNameW(ofn->lpstrFile);
            WCHAR *ext = PathFindExtensionW(name);
            ofn->nFileOffset = (WORD)(name - ofn->lpstrFile);
            ofn->nFileExtension = (*ext == '.') ? (WORD)(ext - ofn->lpstrFile + 1) : 0;
        }
        ret = TRUE;
        goto done;
    }

    /* several files: "folder\0name\0name\0\0", the names relative to the folder */
    for (i = 0; i < count; i++)
    {
        IShellItem *psi;
        WCHAR *path;
        if (FAILED(IShellItemArray_GetItemAt(psia, i, &psi)))
            continue;
        if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &path)))
        {
            WCHAR *name = PathFindFileNameW(path);
            if (!folder)
            {
                folder = malloc((name - path + 1) * sizeof(WCHAR));
                lstrcpynW(folder, path, name - path + 1);
                PathRemoveBackslashW(folder);
                len = lstrlenW(folder) + 1;
            }
            len += lstrlenW(name) + 1;
            CoTaskMemFree(path);
        }
        IShellItem_Release(psi);
    }
    len++;
    if (!folder || len > ofn->nMaxFile)
    {
        if (ofn->lpstrFile && ofn->nMaxFile >= 1)
            *(WORD *)ofn->lpstrFile = (WORD)len;
        COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
        goto done;
    }
    buf = ofn->lpstrFile;
    lstrcpyW(buf, folder);
    used = lstrlenW(folder) + 1;
    offset = used;
    for (i = 0; i < count; i++)
    {
        IShellItem *psi;
        WCHAR *path;
        if (FAILED(IShellItemArray_GetItemAt(psia, i, &psi)))
            continue;
        if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &path)))
        {
            WCHAR *name = PathFindFileNameW(path);
            lstrcpyW(buf + used, name);
            used += lstrlenW(name) + 1;
            CoTaskMemFree(path);
        }
        IShellItem_Release(psi);
    }
    buf[used] = 0;
    ofn->nFileOffset = (WORD)offset;
    ofn->nFileExtension = 0;
    ret = TRUE;

done:
    CoTaskMemFree(first);
    free(folder);
    if (psia)
        IShellItemArray_Release(psia);
    return ret;
}

/* ---------------------------------------------------------------- hooks */

/* A program's hook and template, in the Windows 10 dialog: the hook's dialog
 * (its template, or an empty one) is a child of the dialog, over the names,
 * as the Explorer-style dialog had it; it gets WM_INITDIALOG with the
 * OPENFILENAME and the CDN_ notifications, and the dialog answers its CDM_
 * messages (itemdlg.c). */
typedef struct
{
    IFileDialogEvents IFileDialogEvents_iface;
    LONG ref;
    OPENFILENAMEW *ofn;
    OPENFILENAMEA *ofnA;        /* the program's own structure, for the A functions */
    HWND dlg, child;
    UINT fileok_msg, sharevi_msg;
} hook_events;

static inline hook_events *impl_from_events(IFileDialogEvents *iface)
{
    return CONTAINING_RECORD(iface, hook_events, IFileDialogEvents_iface);
}

static HRESULT WINAPI hook_QueryInterface(IFileDialogEvents *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFileDialogEvents))
    {
        *ppv = iface;
        IFileDialogEvents_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hook_AddRef(IFileDialogEvents *iface)
{
    return InterlockedIncrement(&impl_from_events(iface)->ref);
}

static ULONG WINAPI hook_Release(IFileDialogEvents *iface)
{
    hook_events *This = impl_from_events(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    if (!ref)
        free(This);
    return ref;
}

/* CDN_ to the hook's dialog: what it put in DWLP_MSGRESULT */
static LONG_PTR hook_notify(hook_events *This, UINT code, LPCWSTR file)
{
    if (!This->child)
        return 0;
    SetWindowLongPtrW(This->child, DWLP_MSGRESULT, 0);
    if (This->ofnA)
    {
        OFNOTIFYA n = { { This->dlg, 0, code }, This->ofnA, NULL };
        char fileA[MAX_PATH];
        if (file)
        {
            WideCharToMultiByte(CP_ACP, 0, file, -1, fileA, sizeof(fileA), NULL, NULL);
            n.pszFile = fileA;
        }
        SendMessageA(This->child, WM_NOTIFY, 0, (LPARAM)&n);
    }
    else
    {
        OFNOTIFYW n = { { This->dlg, 0, code }, This->ofn, (LPWSTR)file };
        SendMessageW(This->child, WM_NOTIFY, 0, (LPARAM)&n);
    }
    return GetWindowLongPtrW(This->child, DWLP_MSGRESULT);
}

static INT_PTR CALLBACK hook_template_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    /* a template without a hook: the controls are shown, nothing is answered */
    return msg == WM_INITDIALOG;
}

/* The hook's dialog, the first time the dialog says where it is */
static void hook_attach(hook_events *This, IFileDialog *pfd)
{
    static const struct { DLGTEMPLATE t; WORD menu, cls, title; } empty =
        { { WS_CHILD | DS_CONTROL | DS_3DLOOK, 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
    IOleWindow *pow;
    LPCDLGTEMPLATEW tmpl = (LPCDLGTEMPLATEW)&empty;
    HINSTANCE inst = COMDLG32_hInstance;
    DLGPROC proc;
    DWORD flags = This->ofn->Flags;
    LPARAM param = This->ofnA ? (LPARAM)This->ofnA : (LPARAM)This->ofn;

    if (This->dlg || FAILED(IFileDialog_QueryInterface(pfd, &IID_IOleWindow, (void **)&pow)))
        return;
    IOleWindow_GetWindow(pow, &This->dlg);
    IOleWindow_Release(pow);
    if (!This->dlg)
        return;
    if (This->ofnA)
        SetPropW(This->dlg, L"itemdlg_ansi", (HANDLE)1);

    if (flags & OFN_ENABLETEMPLATEHANDLE)
    {
        tmpl = (LPCDLGTEMPLATEW)This->ofn->hInstance;
        inst = COMDLG32_hInstance;
    }
    else if (flags & OFN_ENABLETEMPLATE)
    {
        HRSRC res = This->ofnA ? FindResourceA(This->ofnA->hInstance, This->ofnA->lpTemplateName, (LPCSTR)RT_DIALOG)
                               : FindResourceW(This->ofn->hInstance, This->ofn->lpTemplateName, (LPCWSTR)RT_DIALOG);
        HGLOBAL mem = res ? LoadResource(This->ofn->hInstance, res) : NULL;
        if (mem && (tmpl = LockResource(mem)))
            inst = This->ofn->hInstance;
        else
            tmpl = (LPCDLGTEMPLATEW)&empty;
    }
    proc = (flags & OFN_ENABLEHOOK) && This->ofn->lpfnHook ? (DLGPROC)This->ofn->lpfnHook : hook_template_proc;

    This->child = This->ofnA ? CreateDialogIndirectParamA(inst, (LPCDLGTEMPLATEA)tmpl, This->dlg, proc, param)
                             : CreateDialogIndirectParamW(inst, tmpl, This->dlg, proc, param);
    if (!This->child)
    {
        ERR("the hook's dialog: %lu\n", GetLastError());
        return;
    }
    /* a child under the dialog's names, whatever the template said */
    SetWindowLongPtrW(This->child, GWL_STYLE,
                      (GetWindowLongPtrW(This->child, GWL_STYLE) & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) |
                      WS_CHILD | WS_CLIPSIBLINGS);
    ShowWindow(This->child, SW_SHOWNA);
    ITEMDLG_SetHookChild(This->dlg, This->child);
    hook_notify(This, CDN_INITDONE, NULL);
}

static HRESULT WINAPI hook_OnFileOk(IFileDialogEvents *iface, IFileDialog *pfd)
{
    hook_events *This = impl_from_events(iface);
    IShellItem *psi = NULL;
    WCHAR *path = NULL;
    LONG_PTR refuse;

    /* the name chosen is in the structure while the hook looks at it */
    if (SUCCEEDED(IFileDialog_GetResult(pfd, &psi)) && SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &path)))
    {
        lstrcpynW(This->ofn->lpstrFile, path, This->ofn->nMaxFile);
        This->ofn->nFileOffset = (WORD)(PathFindFileNameW(This->ofn->lpstrFile) - This->ofn->lpstrFile);
        if (This->ofnA)
        {
            WideCharToMultiByte(CP_ACP, 0, path, -1, This->ofnA->lpstrFile, This->ofnA->nMaxFile, NULL, NULL);
            This->ofnA->nFileOffset = (WORD)(PathFindFileNameA(This->ofnA->lpstrFile) - This->ofnA->lpstrFile);
        }
    }
    if (psi)
        IShellItem_Release(psi);
    refuse = hook_notify(This, CDN_FILEOK, NULL);
    /* the registered message the old hooks have, if CDN_FILEOK said nothing */
    if (!refuse && This->child)
        refuse = SendMessageW(This->child, This->fileok_msg, 0, This->ofnA ? (LPARAM)This->ofnA : (LPARAM)This->ofn);
    CoTaskMemFree(path);
    return refuse ? S_FALSE : S_OK;
}

static HRESULT WINAPI hook_OnFolderChanging(IFileDialogEvents *iface, IFileDialog *pfd, IShellItem *psi)
{
    return S_OK;
}

static HRESULT WINAPI hook_OnFolderChange(IFileDialogEvents *iface, IFileDialog *pfd)
{
    hook_events *This = impl_from_events(iface);
    hook_attach(This, pfd);
    hook_notify(This, CDN_FOLDERCHANGE, NULL);
    return S_OK;
}

static HRESULT WINAPI hook_OnSelectionChange(IFileDialogEvents *iface, IFileDialog *pfd)
{
    hook_events *This = impl_from_events(iface);
    hook_attach(This, pfd);
    hook_notify(This, CDN_SELCHANGE, NULL);
    return S_OK;
}

static HRESULT WINAPI hook_OnShareViolation(IFileDialogEvents *iface, IFileDialog *pfd, IShellItem *psi,
                                            FDE_SHAREVIOLATION_RESPONSE *response)
{
    hook_events *This = impl_from_events(iface);
    WCHAR *path = NULL;
    LONG_PTR ret;

    if (FAILED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &path)))
        return E_NOTIMPL;
    ret = hook_notify(This, CDN_SHAREVIOLATION, path);
    CoTaskMemFree(path);
    *response = ret == OFN_SHAREFALLTHROUGH ? FDESVR_ACCEPT : ret == OFN_SHARENOWARN ? FDESVR_REFUSE : FDESVR_DEFAULT;
    return S_OK;
}

static HRESULT WINAPI hook_OnTypeChange(IFileDialogEvents *iface, IFileDialog *pfd)
{
    hook_events *This = impl_from_events(iface);
    UINT type;
    hook_attach(This, pfd);
    if (SUCCEEDED(IFileDialog_GetFileTypeIndex(pfd, &type)))
    {
        This->ofn->nFilterIndex = type;
        if (This->ofnA)
            This->ofnA->nFilterIndex = type;
    }
    hook_notify(This, CDN_TYPECHANGE, NULL);
    return S_OK;
}

static HRESULT WINAPI hook_OnOverwrite(IFileDialogEvents *iface, IFileDialog *pfd, IShellItem *psi,
                                       FDE_OVERWRITE_RESPONSE *response)
{
    return E_NOTIMPL;
}

static const IFileDialogEventsVtbl hook_events_vtbl =
{
    hook_QueryInterface, hook_AddRef, hook_Release,
    hook_OnFileOk, hook_OnFolderChanging, hook_OnFolderChange, hook_OnSelectionChange,
    hook_OnShareViolation, hook_OnTypeChange, hook_OnOverwrite,
};

static hook_events *hook_create(OPENFILENAMEW *ofn, OPENFILENAMEA *ofnA)
{
    hook_events *This = calloc(1, sizeof(*This));
    if (!This)
        return NULL;
    This->IFileDialogEvents_iface.lpVtbl = &hook_events_vtbl;
    This->ref = 1;
    This->ofn = ofn;
    This->ofnA = ofnA;
    This->fileok_msg = RegisterWindowMessageW(FILEOKSTRINGW);
    This->sharevi_msg = RegisterWindowMessageW(SHAREVISTRINGW);
    return This;
}

/* ---------------------------------------------------------------- the dialog */

static BOOL get_file_name_w(OPENFILENAMEW *ofn, BOOL save, OPENFILENAMEA *ofnA);

BOOL FILEDLG_GetFileNameW(OPENFILENAMEW *ofn, BOOL save)
{
    return get_file_name_w(ofn, save, NULL);
}

static BOOL get_file_name_w(OPENFILENAMEW *ofn, BOOL save, OPENFILENAMEA *ofnA)
{
    COMDLG_FILTERSPEC *specs = NULL;
    IFileDialogCustomize *pfdc = NULL;
    IFileDialog *pfd = NULL;
    IShellItem *folder = NULL;
    UINT spec_count, custom = 0;
    WCHAR *name = NULL, *dir = NULL;
    BOOL readonly_choice = FALSE;
    BOOL com, ret = FALSE;
    hook_events *hook = NULL;
    DWORD hook_cookie = 0;
    HRESULT hr;

    COMDLG32_SetCommDlgExtendedError(0);
    if (!ofn || ofn->lStructSize < OPENFILENAME_SIZE_VERSION_400W)
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_STRUCTSIZE);
        return FALSE;
    }
    if (!ofn->lpstrFile || !ofn->nMaxFile)
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_INITIALIZATION);
        return FALSE;
    }

    com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

    hr = save ? FileSaveDialog_Constructor(NULL, &IID_IFileDialog, (void **)&pfd)
              : FileOpenDialog_Constructor(NULL, &IID_IFileDialog, (void **)&pfd);
    if (FAILED(hr))
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_INITIALIZATION);
        goto done;
    }

    IFileDialog_SetOptions(pfd, options_from_flags(ofn->Flags, save));

    spec_count = parse_filters(ofn->lpstrFilter,
                               (ofn->lpstrCustomFilter && ofn->nMaxCustFilter) ? ofn->lpstrCustomFilter : NULL,
                               &specs);
    custom = (ofn->lpstrCustomFilter && ofn->nMaxCustFilter && *ofn->lpstrCustomFilter) ? 1 : 0;
    if (spec_count)
    {
        UINT index = ofn->nFilterIndex ? ofn->nFilterIndex + custom : 1;
        IFileDialog_SetFileTypes(pfd, spec_count, specs);
        IFileDialog_SetFileTypeIndex(pfd, min(index, spec_count));
    }
    if (ofn->lpstrTitle)
        IFileDialog_SetTitle(pfd, ofn->lpstrTitle);
    if (ofn->lpstrDefExt)
        IFileDialog_SetDefaultExtension(pfd, ofn->lpstrDefExt);

    /* the folder: of lpstrFile if it has one, else lpstrInitialDir, else the current one */
    if (*ofn->lpstrFile)
    {
        WCHAR *file = malloc((lstrlenW(ofn->lpstrFile) + 1) * sizeof(WCHAR));
        lstrcpyW(file, ofn->lpstrFile);
        if (StrChrW(file, '\\') || StrChrW(file, '/') || (file[0] && file[1] == ':'))
        {
            WCHAR *base = PathFindFileNameW(file);
            name = malloc((lstrlenW(base) + 1) * sizeof(WCHAR));
            lstrcpyW(name, base);
            *base = 0;
            folder = item_from_path(file);
        }
        else
        {
            name = file;
            file = NULL;
        }
        free(file);
    }
    if (!folder && ofn->lpstrInitialDir && *ofn->lpstrInitialDir)
    {
        DWORD len = ExpandEnvironmentStringsW(ofn->lpstrInitialDir, NULL, 0);
        dir = malloc(len * sizeof(WCHAR));
        ExpandEnvironmentStringsW(ofn->lpstrInitialDir, dir, len);
        folder = item_from_path(dir);
    }
    if (folder)
    {
        IFileDialog_SetFolder(pfd, folder);
    }
    else
    {
        WCHAR cur[MAX_PATH];
        if (GetCurrentDirectoryW(ARRAY_SIZE(cur), cur) && (folder = item_from_path(cur)))
            IFileDialog_SetDefaultFolder(pfd, folder);
    }
    /* "*.txt" in the name is the old dialog's filter, not a name */
    if (name && *name && !StrChrW(name, '*') && !StrChrW(name, '?'))
        IFileDialog_SetFileName(pfd, name);

    /* the read-only check box: Windows' dialog has it as a choice of the Open button */
    if (!save && !(ofn->Flags & OFN_HIDEREADONLY) &&
        SUCCEEDED(IFileDialog_QueryInterface(pfd, &IID_IFileDialogCustomize, (void **)&pfdc)))
    {
        WCHAR open[64], readonly[64];
        if (!LoadStringW(COMDLG32_hInstance, IDS_ITEMDLG_OPEN_BUTTON, open, ARRAY_SIZE(open)))
            lstrcpyW(open, L"&Open");
        if (!LoadStringW(COMDLG32_hInstance, IDS_ITEMDLG_OPEN_READONLY, readonly, ARRAY_SIZE(readonly)))
            lstrcpyW(readonly, L"Open as &read-only");
        if (SUCCEEDED(IFileDialogCustomize_EnableOpenDropDown(pfdc, OFN_READONLY_CHOICES)) &&
            SUCCEEDED(IFileDialogCustomize_AddControlItem(pfdc, OFN_READONLY_CHOICES, OFN_CHOICE_OPEN, open)) &&
            SUCCEEDED(IFileDialogCustomize_AddControlItem(pfdc, OFN_READONLY_CHOICES, OFN_CHOICE_READONLY, readonly)))
        {
            readonly_choice = TRUE;
            if (ofn->Flags & OFN_READONLY)
                IFileDialogCustomize_SetSelectedControlItem(pfdc, OFN_READONLY_CHOICES, OFN_CHOICE_READONLY);
        }
    }

    if ((ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) &&
        (hook = hook_create(ofn, ofnA)) &&
        FAILED(IFileDialog_Advise(pfd, &hook->IFileDialogEvents_iface, &hook_cookie)))
    {
        IFileDialogEvents_Release(&hook->IFileDialogEvents_iface);
        hook = NULL;
    }

    hr = IFileDialog_Show(pfd, ofn->hwndOwner);
    if (hook)
    {
        IFileDialog_Unadvise(pfd, hook_cookie);
        IFileDialogEvents_Release(&hook->IFileDialogEvents_iface);
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        goto done;
    if (FAILED(hr))
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_DIALOGFAILURE);
        goto done;
    }

    if (!store_results(ofn, pfd, (ofn->Flags & OFN_ALLOWMULTISELECT) && !save))
        goto done;

    /* the file type, the custom filter's pattern */
    {
        UINT type = 0;
        if (spec_count && SUCCEEDED(IFileDialog_GetFileTypeIndex(pfd, &type)) && type)
        {
            ofn->nFilterIndex = (custom && type == 1) ? 0 : type - custom;
            if (ofn->lpstrCustomFilter && ofn->nMaxCustFilter)
            {
                LPWSTR pattern = ofn->lpstrCustomFilter + lstrlenW(ofn->lpstrCustomFilter) + 1;
                DWORD room = ofn->nMaxCustFilter - (DWORD)(pattern - ofn->lpstrCustomFilter);
                if ((INT)room > 1)
                {
                    lstrcpynW(pattern, specs[type - 1].pszSpec, room - 1);
                    pattern[lstrlenW(pattern) + 1] = 0;
                }
            }
        }
    }

    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle)
        lstrcpynW(ofn->lpstrFileTitle, ofn->lpstrFile + ofn->nFileOffset, ofn->nMaxFileTitle);

    ofn->Flags &= ~(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
    if (readonly_choice)
    {
        DWORD choice = OFN_CHOICE_OPEN;
        if (SUCCEEDED(IFileDialogCustomize_GetSelectedControlItem(pfdc, OFN_READONLY_CHOICES, &choice)) &&
            choice == OFN_CHOICE_READONLY)
            ofn->Flags |= OFN_READONLY;
    }
    if (ofn->lpstrDefExt && ofn->nFileExtension &&
        lstrcmpiW(ofn->lpstrFile + ofn->nFileExtension, ofn->lpstrDefExt))
        ofn->Flags |= OFN_EXTENSIONDIFFERENT;

    if (!(ofn->Flags & OFN_NOCHANGEDIR))
    {
        WCHAR *cur = malloc((ofn->nFileOffset + 1) * sizeof(WCHAR));
        lstrcpynW(cur, ofn->lpstrFile, ofn->nFileOffset + 1);
        SetCurrentDirectoryW(cur);
        free(cur);
    }
    ret = TRUE;

done:
    if (pfdc)
        IFileDialogCustomize_Release(pfdc);
    if (folder)
        IShellItem_Release(folder);
    if (pfd)
        IFileDialog_Release(pfd);
    free(specs);
    free(name);
    free(dir);
    if (com)
        CoUninitialize();
    return ret;
}

/* The ANSI structure: its strings in Unicode for the dialog, and back */
static WCHAR *dup_multi_AtoW(LPCSTR str, DWORD *chars)
{
    LPCSTR p = str;
    DWORD len;
    WCHAR *ret;

    if (!str)
        return NULL;
    while (*p)
        p += strlen(p) + 1;
    len = MultiByteToWideChar(CP_ACP, 0, str, p - str + 1, NULL, 0);
    ret = malloc(len * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, str, p - str + 1, ret, len);
    if (chars)
        *chars = len;
    return ret;
}

static WCHAR *dup_AtoW(LPCSTR str)
{
    DWORD len;
    WCHAR *ret;

    if (!str)
        return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
    ret = malloc(len * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, str, -1, ret, len);
    return ret;
}

BOOL FILEDLG_GetFileNameA(OPENFILENAMEA *ofn, BOOL save)
{
    OPENFILENAMEW ofnW;
    WCHAR *custom = NULL;
    BOOL ret;

    COMDLG32_SetCommDlgExtendedError(0);
    if (!ofn || ofn->lStructSize < OPENFILENAME_SIZE_VERSION_400A)
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_STRUCTSIZE);
        return FALSE;
    }
    if (!ofn->lpstrFile || !ofn->nMaxFile)
    {
        COMDLG32_SetCommDlgExtendedError(CDERR_INITIALIZATION);
        return FALSE;
    }

    memset(&ofnW, 0, sizeof(ofnW));
    ofnW.lStructSize = sizeof(ofnW);
    ofnW.hwndOwner = ofn->hwndOwner;
    ofnW.hInstance = ofn->hInstance;
    ofnW.lpstrFilter = dup_multi_AtoW(ofn->lpstrFilter, NULL);
    if (ofn->lpstrCustomFilter && ofn->nMaxCustFilter)
    {
        /* "name\0pattern\0", with room for the new pattern */
        DWORD len = ofn->nMaxCustFilter * 2 + 2;
        custom = calloc(1, len * sizeof(WCHAR));
        if (*ofn->lpstrCustomFilter)
        {
            LPCSTR pattern = ofn->lpstrCustomFilter + strlen(ofn->lpstrCustomFilter) + 1;
            int n = MultiByteToWideChar(CP_ACP, 0, ofn->lpstrCustomFilter, -1, custom, len);
            if (n > 0)
                MultiByteToWideChar(CP_ACP, 0, pattern, -1, custom + n, len - n);
        }
        ofnW.lpstrCustomFilter = custom;
        ofnW.nMaxCustFilter = len;
    }
    ofnW.nFilterIndex = ofn->nFilterIndex;
    ofnW.nMaxFile = ofn->nMaxFile;
    ofnW.lpstrFile = calloc(1, ofnW.nMaxFile * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, ofn->lpstrFile, -1, ofnW.lpstrFile, ofnW.nMaxFile);
    ofnW.nMaxFileTitle = ofn->nMaxFileTitle;
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle)
        ofnW.lpstrFileTitle = calloc(1, ofn->nMaxFileTitle * sizeof(WCHAR));
    ofnW.lpstrInitialDir = dup_AtoW(ofn->lpstrInitialDir);
    ofnW.lpstrTitle = dup_AtoW(ofn->lpstrTitle);
    ofnW.Flags = ofn->Flags;
    ofnW.lpstrDefExt = dup_AtoW(ofn->lpstrDefExt);
    ofnW.lCustData = ofn->lCustData;
    ofnW.lpfnHook = ofn->lpfnHook;
    ofnW.FlagsEx = ofn->lStructSize >= sizeof(OPENFILENAMEA) ? ofn->FlagsEx : 0;

    ret = get_file_name_w(&ofnW, save, ofn);
    if (ret)
    {
        /* the names, and the offsets in bytes as the ANSI structure counts them */
        DWORD wlen = ofnW.nFileOffset, total = 0;
        WCHAR *p = ofnW.lpstrFile;
        int n;

        while (*p)
            p += lstrlenW(p) + 1;
        total = p - ofnW.lpstrFile + 1;
        n = WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, total, NULL, 0, NULL, NULL);
        if (n > (int)ofn->nMaxFile)
        {
            *(WORD *)ofn->lpstrFile = (WORD)n;
            COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
            ret = FALSE;
        }
        else
        {
            WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, total, ofn->lpstrFile, ofn->nMaxFile, NULL, NULL);
            ofn->nFileOffset = (WORD)WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, wlen, NULL, 0, NULL, NULL);
            ofn->nFileExtension = ofnW.nFileExtension ?
                (WORD)WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFile, ofnW.nFileExtension, NULL, 0, NULL, NULL) : 0;
            ofn->nFilterIndex = ofnW.nFilterIndex;
            ofn->Flags = ofnW.Flags;
            if (ofn->lpstrFileTitle && ofn->nMaxFileTitle)
                WideCharToMultiByte(CP_ACP, 0, ofnW.lpstrFileTitle, -1, ofn->lpstrFileTitle, ofn->nMaxFileTitle, NULL, NULL);
            if (custom && *custom)
            {
                LPSTR pattern = ofn->lpstrCustomFilter + strlen(ofn->lpstrCustomFilter) + 1;
                DWORD room = ofn->nMaxCustFilter - (DWORD)(pattern - ofn->lpstrCustomFilter);
                if ((INT)room > 1)
                {
                    WideCharToMultiByte(CP_ACP, 0, custom + lstrlenW(custom) + 1, -1, pattern, room - 1, NULL, NULL);
                    pattern[strlen(pattern) + 1] = 0;
                }
            }
        }
    }
    else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
    {
        *(WORD *)ofn->lpstrFile = *(WORD *)ofnW.lpstrFile;
    }

    free((void *)ofnW.lpstrFilter);
    free(custom);
    free(ofnW.lpstrFile);
    free(ofnW.lpstrFileTitle);
    free((void *)ofnW.lpstrInitialDir);
    free((void *)ofnW.lpstrTitle);
    free((void *)ofnW.lpstrDefExt);
    return ret;
}
