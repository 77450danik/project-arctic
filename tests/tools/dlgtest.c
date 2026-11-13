/* Opens one of the file dialogs a program can ask for, for a look at it:
 *   dlgtest ofn | ofnsave | ofnhook | old | ifd | ifdsave | folder | bff  [dpi] [close=SECONDS]
 * ofn: GetOpenFileName (Explorer style), ofnsave: GetSaveFileName, ofnhook:
 * with a hook and a template, old: without OFN_EXPLORER, ifd: IFileOpenDialog,
 * ifdsave: IFileSaveDialog, folder: IFileOpenDialog with FOS_PICKFOLDERS,
 * bff: SHBrowseForFolder. "dpi" makes the program per-monitor aware (as
 * Chrome is). The dialog's rectangle goes to stdout; it closes after the
 * given seconds (default: stays).
 *   x86_64-w64-mingw32-gcc -O1 -o dlgtest.exe tests/tools/dlgtest.c -lcomdlg32 -lole32 -lshell32 -luuid */
#define COBJMACROS
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

static DWORD main_tid;
static int close_after;

static BOOL CALLBACK find_dialog(HWND hwnd, LPARAM lp)
{
    if (IsWindowVisible(hwnd)) { *(HWND *)lp = hwnd; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK print_child(HWND hwnd, LPARAM lp)
{
    WCHAR cls[64];
    RECT r;

    if (!IsWindowVisible(hwnd)) return TRUE;
    GetClassNameW(hwnd, cls, 64);
    GetWindowRect(hwnd, &r);
    MapWindowPoints(NULL, (HWND)lp, (POINT *)&r, 2);
    printf("child %p id %d %ls %ld %ld %ld %ld parent %p\n", hwnd, GetDlgCtrlID(hwnd), cls, r.left, r.top,
           r.right - r.left, r.bottom - r.top, GetParent(hwnd));
    return TRUE;
}

static DWORD WINAPI watcher(void *arg)
{
    HWND dlg = NULL;
    RECT r;
    int i;

    for (i = 0; i < 100 && !dlg; i++)
    {
        Sleep(100);
        EnumThreadWindows(main_tid, find_dialog, (LPARAM)&dlg);
    }
    Sleep(1500);
    if (dlg && GetWindowRect(dlg, &r))
    {
        printf("rect %ld %ld %ld %ld dpi %u\n", r.left, r.top, r.right - r.left, r.bottom - r.top, GetDpiForWindow(dlg));
        EnumChildWindows(dlg, print_child, (LPARAM)dlg);
        if (getenv("DLGTEST_REDRAW"))
            RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        fflush(stdout);
    }
    if (close_after > 0)
    {
        Sleep(close_after * 1000);
        if (dlg) PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        Sleep(1000);
        ExitProcess(0);
    }
    return 0;
}

static UINT_PTR CALLBACK hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NOTIFY && ((OFNOTIFYW *)lp)->hdr.code == CDN_INITDONE)
        printf("hook: CDN_INITDONE\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "ifd";
    WCHAR file[MAX_PATH] = L"";
    int i;

    for (i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "dpi")) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else if (!strncmp(argv[i], "close=", 6)) close_after = atoi(argv[i] + 6);
    }
    main_tid = GetCurrentThreadId();
    CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!strncmp(mode, "ofn", 3) || !strcmp(mode, "old"))
    {
        OPENFILENAMEW ofn = { sizeof(ofn) };
        BOOL ok;

        ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (!strcmp(mode, "old")) ofn.Flags = OFN_FILEMUSTEXIST;
        if (!strcmp(mode, "ofnhook"))
        {
            ofn.Flags |= OFN_ENABLEHOOK;
            ofn.lpfnHook = hook;
        }
        ok = !strcmp(mode, "ofnsave") ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        printf("result %d %ls err %lx\n", ok, file, CommDlgExtendedError());
    }
    else if (!strcmp(mode, "bff"))
    {
        BROWSEINFOW bi = { 0 };
        LPITEMIDLIST pidl;

        bi.lpszTitle = L"Choose a folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (pidl) SHGetPathFromIDListW(pidl, file);
        printf("result %p %ls\n", pidl, file);
    }
    else
    {
        IFileDialog *fd;
        HRESULT hr;
        BOOL save = !strcmp(mode, "ifdsave");

        hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileDialog, (void **)&fd);
        if (FAILED(hr)) { printf("CoCreateInstance %lx\n", hr); return 1; }
        if (!strcmp(mode, "folder"))
            IFileDialog_SetOptions(fd, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        else
        {
            COMDLG_FILTERSPEC spec[] = { { L"Text files", L"*.txt" }, { L"All files", L"*.*" } };
            IFileDialog_SetFileTypes(fd, 2, spec);
        }
        hr = IFileDialog_Show(fd, NULL);
        printf("Show %lx\n", hr);
        IFileDialog_Release(fd);
    }
    return 0;
}
