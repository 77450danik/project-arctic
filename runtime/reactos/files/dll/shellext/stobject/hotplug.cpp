/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        dll/shellext/stobject/hotplug.cpp
 * PURPOSE:     Removable devices notification icon handler
 * PROGRAMMERS: Shriraj Sawant a.k.a SR13 <sr.official@hotmail.com>
 */

/* Safely Remove Hardware as Windows 10 has it. The icon shows while a disk
 * that can be taken out is plugged in: one on USB, or with a removable
 * medium. Its menu lists each such disk and, under it, its volumes; choosing
 * one asks the storage stack to flush, dismount and stop the disk
 * (IOCTL_STORAGE_EJECT_MEDIA) and then says it can be pulled out, or that a
 * program still uses it. */

#include "precomp.h"

#include <atlsimpcoll.h>
#include <dbt.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <shlwapi.h>
#include <strsafe.h>

#define WM_HOTPLUG_EJECTED        (WM_USER + 223)
#define HOTPLUG_BALLOON_TIMER_ID  6
#define HOTPLUG_BALLOON_TIME      6000 /* the toast: 5 s, then 1 s going */

#define IDM_HOTPLUG_DEVICES       1
#define IDM_HOTPLUG_EJECT         100

struct HOTPLUG_DISK
{
    DWORD dwNumber;       /* IOCTL_STORAGE_GET_DEVICE_NUMBER: the same for all its volumes */
    WCHAR szName[128];    /* the product name the disk reports */
    WCHAR szDrives[27];   /* the letters of its volumes */
};

static CSimpleArray<HOTPLUG_DISK> g_Disks;
static CString g_strHotplugTooltip;
static HICON g_hIconHotplug = NULL;
static BOOL g_bBalloonShown = FALSE; /* the icon stays while its notification does */
static BOOL g_bEjecting = FALSE;

/* whether the drive is a volume of a disk that can be taken out, and which */
static BOOL GetEjectableDisk(WCHAR chDrive, DWORD *pdwNumber, LPWSTR pszName, DWORD cchName)
{
    WCHAR szDevice[] = L"\\\\.\\?:";
    STORAGE_PROPERTY_QUERY query = { StorageDeviceProperty, PropertyStandardQuery };
    STORAGE_DEVICE_NUMBER number;
    BYTE buffer[1024];
    STORAGE_DEVICE_DESCRIPTOR *pDesc = (STORAGE_DEVICE_DESCRIPTOR *)buffer;
    DWORD dwSize = 0;
    BOOL bEjectable = FALSE;
    HANDLE hDevice;

    szDevice[4] = chDrive;
    hDevice = CreateFileW(szDevice, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE)
        return FALSE;

    ZeroMemory(buffer, sizeof(buffer));
    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        buffer, sizeof(buffer) - 1, &dwSize, NULL) &&
        dwSize >= FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) &&
        (pDesc->BusType == BusTypeUsb || pDesc->RemovableMedia) &&
        DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                        &number, sizeof(number), &dwSize, NULL))
    {
        *pdwNumber = number.DeviceNumber;
        pszName[0] = UNICODE_NULL;
        if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < sizeof(buffer))
            MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)buffer + pDesc->ProductIdOffset, -1, pszName, cchName);
        StrTrimW(pszName, L" ");
        bEjectable = TRUE;
    }
    CloseHandle(hDevice);
    return bEjectable;
}

static VOID EnumEjectableDisks()
{
    DWORD dwDrives = GetLogicalDrives();

    g_Disks.RemoveAll();
    for (WCHAR ch = L'A'; ch <= L'Z'; ch++)
    {
        WCHAR szName[128];
        DWORD dwNumber;
        int i;

        if (!(dwDrives & (1 << (ch - L'A'))) || !GetEjectableDisk(ch, &dwNumber, szName, _countof(szName)))
            continue;

        for (i = 0; i < g_Disks.GetSize(); i++)
        {
            if (g_Disks[i].dwNumber == dwNumber)
                break;
        }
        if (i == g_Disks.GetSize())
        {
            HOTPLUG_DISK disk;
            ZeroMemory(&disk, sizeof(disk));
            disk.dwNumber = dwNumber;
            if (szName[0])
                StringCchCopyW(disk.szName, _countof(disk.szName), szName);
            else
                LoadStringW(g_hInstance, IDS_HOTPLUG_MASS_STORAGE, disk.szName, _countof(disk.szName));
            g_Disks.Add(disk);
        }
        size_t len = wcslen(g_Disks[i].szDrives);
        g_Disks[i].szDrives[len] = ch;
        g_Disks[i].szDrives[len + 1] = UNICODE_NULL;
    }
}

static VOID UpdateHotplugIcon(CSysTray *pSysTray)
{
    EnumEjectableDisks();
    pSysTray->NotifyIcon(NIM_MODIFY, ID_ICON_HOTPLUG, g_hIconHotplug, g_strHotplugTooltip,
                         (g_Disks.GetSize() > 0 || g_bBalloonShown || g_bEjecting) ? 0 : NIS_HIDDEN);
}

HRESULT STDMETHODCALLTYPE Hotplug_Init(_In_ CSysTray * pSysTray)
{
    TRACE("Hotplug_Init\n");

    g_hIconHotplug = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_HOTPLUG_OK), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_strHotplugTooltip.LoadStringW(IDS_HOTPLUG_REMOVE_1);

    EnumEjectableDisks();

    return pSysTray->NotifyIcon(NIM_ADD, ID_ICON_HOTPLUG, g_hIconHotplug, g_strHotplugTooltip,
                                g_Disks.GetSize() > 0 ? 0 : NIS_HIDDEN);
}

HRESULT STDMETHODCALLTYPE Hotplug_Update(_In_ CSysTray * pSysTray)
{
    TRACE("Hotplug_Update\n");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Hotplug_Shutdown(_In_ CSysTray * pSysTray)
{
    TRACE("Hotplug_Shutdown\n");

    DestroyIcon(g_hIconHotplug);
    g_hIconHotplug = NULL;

    return pSysTray->NotifyIcon(NIM_DELETE, ID_ICON_HOTPLUG, NULL, NULL);
}

struct HOTPLUG_EJECT
{
    HWND hwnd;
    WCHAR chDrive;
};

/* the stack flushes, dismounts and stops the whole disk through any of its volumes */
static DWORD WINAPI EjectThread(LPVOID lpParameter)
{
    HOTPLUG_EJECT *pEject = (HOTPLUG_EJECT *)lpParameter;
    WCHAR szDevice[] = L"\\\\.\\?:";
    DWORD dwError = ERROR_SUCCESS, dwSize;
    HANDLE hDevice;

    szDevice[4] = pEject->chDrive;
    hDevice = CreateFileW(szDevice, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        dwError = GetLastError();
    }
    else
    {
        if (!DeviceIoControl(hDevice, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &dwSize, NULL))
            dwError = GetLastError();
        CloseHandle(hDevice);
    }

    PostMessageW(pEject->hwnd, WM_HOTPLUG_EJECTED, dwError, 0);
    HeapFree(GetProcessHeap(), 0, pEject);
    return 0;
}

static VOID EjectDisk(CSysTray *pSysTray, int index)
{
    HOTPLUG_EJECT *pEject;
    HANDLE hThread;

    if (g_bEjecting || index < 0 || index >= g_Disks.GetSize())
        return;
    pEject = (HOTPLUG_EJECT *)HeapAlloc(GetProcessHeap(), 0, sizeof(*pEject));
    if (!pEject)
        return;
    pEject->hwnd = pSysTray->GetHWnd();
    pEject->chDrive = g_Disks[index].szDrives[0];
    g_bEjecting = TRUE;
    hThread = CreateThread(NULL, 0, EjectThread, pEject, 0, NULL);
    if (!hThread)
    {
        g_bEjecting = FALSE;
        HeapFree(GetProcessHeap(), 0, pEject);
        return;
    }
    CloseHandle(hThread);
}

static VOID OnEjected(CSysTray *pSysTray, DWORD dwError)
{
    CString strDevice((LPCWSTR)IDS_HOTPLUG_MASS_STORAGE);
    CString strTitle, strText;

    g_bEjecting = FALSE;
    if (dwError == ERROR_SUCCESS)
    {
        NOTIFYICONDATAW nid = { sizeof(nid) };

        strTitle.LoadStringW(IDS_HOTPLUG_SAFE_TITLE);
        strText.Format(IDS_HOTPLUG_SAFE_TEXT, (LPCWSTR)strDevice);
        g_bBalloonShown = TRUE;
        SetTimer(pSysTray->GetHWnd(), HOTPLUG_BALLOON_TIMER_ID, HOTPLUG_BALLOON_TIME, NULL);

        /* the toast shows the notification icon, 16x16 as in Windows 10 */
        nid.hWnd = pSysTray->GetHWnd();
        nid.uID = ID_ICON_HOTPLUG;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_USER;
        nid.uTimeout = HOTPLUG_BALLOON_TIME;
        StringCchCopyW(nid.szInfoTitle, _countof(nid.szInfoTitle), strTitle);
        StringCchCopyW(nid.szInfo, _countof(nid.szInfo), strText);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        UpdateHotplugIcon(pSysTray);
    }
    else
    {
        UpdateHotplugIcon(pSysTray);
        strTitle.Format(IDS_HOTPLUG_PROBLEM, (LPCWSTR)strDevice);
        strText.LoadStringW(IDS_HOTPLUG_BUSY);
        MessageBoxW(NULL, strText, strTitle, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

static VOID ShowHotplugMenu(CSysTray *pSysTray)
{
    HWND hwnd = pSysTray->GetHWnd();
    HMENU hPopup;
    CString str;
    POINT pt;
    UINT id;

    EnumEjectableDisks();

    hPopup = CreatePopupMenu();
    str.LoadStringW(IDS_HOTPLUG_REMOVE_2);
    AppendMenuW(hPopup, MF_STRING, IDM_HOTPLUG_DEVICES, str);
    if (g_Disks.GetSize() > 0)
        AppendMenuW(hPopup, MF_SEPARATOR, 0, NULL);

    for (int i = 0; i < g_Disks.GetSize(); i++)
    {
        UINT flags = MF_STRING | (g_bEjecting ? MF_GRAYED : 0);

        str.Format(IDS_HOTPLUG_EJECT, g_Disks[i].szName);
        AppendMenuW(hPopup, flags, IDM_HOTPLUG_EJECT + i, str);
        for (LPCWSTR p = g_Disks[i].szDrives; *p; p++)
        {
            WCHAR szRoot[] = L"?:\\";
            SHFILEINFOW sfi;

            szRoot[0] = *p;
            ZeroMemory(&sfi, sizeof(sfi));
            if (!SHGetFileInfoW(szRoot, 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME))
                StringCchPrintfW(sfi.szDisplayName, _countof(sfi.szDisplayName), L"(%c:)", *p);
            str.Format(IDS_HOTPLUG_EJECT_VOLUME, sfi.szDisplayName);
            AppendMenuW(hPopup, flags, IDM_HOTPLUG_EJECT + i, str);
        }
    }

    SetForegroundWindow(hwnd);
    GetCursorPos(&pt);
    id = TrackPopupMenuEx(hPopup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                          pt.x, pt.y, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hPopup);

    if (id == IDM_HOTPLUG_DEVICES)
        ShellExecuteW(NULL, NULL, L"control.exe", L"/name Microsoft.DevicesAndPrinters", NULL, SW_SHOWNORMAL);
    else if (id >= IDM_HOTPLUG_EJECT)
        EjectDisk(pSysTray, id - IDM_HOTPLUG_EJECT);
}

HRESULT STDMETHODCALLTYPE Hotplug_Message(_In_ CSysTray * pSysTray, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &lResult)
{
    TRACE("Hotplug_Message uMsg=%d, wParam=%x, lParam=%x\n", uMsg, wParam, lParam);

    switch (uMsg)
    {
        case WM_USER + 220:
            TRACE("Hotplug_Message: WM_USER+220\n");
            if (wParam == HOTPLUG_SERVICE_FLAG)
            {
                if (lParam)
                {
                    pSysTray->EnableService(HOTPLUG_SERVICE_FLAG, TRUE);
                    return Hotplug_Init(pSysTray);
                }
                else
                {
                    pSysTray->EnableService(HOTPLUG_SERVICE_FLAG, FALSE);
                    return Hotplug_Shutdown(pSysTray);
                }
            }
            return S_FALSE;

        case WM_USER + 221:
            TRACE("Hotplug_Message: WM_USER+221\n");
            if (wParam == HOTPLUG_SERVICE_FLAG)
            {
                lResult = (LRESULT)pSysTray->IsServiceEnabled(HOTPLUG_SERVICE_FLAG);
                return S_OK;
            }
            return S_FALSE;

        case WM_HOTPLUG_EJECTED:
            OnEjected(pSysTray, (DWORD)wParam);
            return S_OK;

        case WM_TIMER:
            if (wParam == HOTPLUG_DEVICE_TIMER_ID)
            {
                KillTimer(pSysTray->GetHWnd(), HOTPLUG_DEVICE_TIMER_ID);
                UpdateHotplugIcon(pSysTray);
                return S_OK;
            }
            if (wParam == HOTPLUG_BALLOON_TIMER_ID)
            {
                KillTimer(pSysTray->GetHWnd(), HOTPLUG_BALLOON_TIMER_ID);
                g_bBalloonShown = FALSE;
                UpdateHotplugIcon(pSysTray);
                return S_OK;
            }
            break;

        case ID_ICON_HOTPLUG:
            switch (lParam)
            {
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                    ShowHotplugMenu(pSysTray);
                    break;
            }
            return S_OK;

        case WM_DEVICECHANGE:
            switch (wParam)
            {
                case DBT_DEVICEARRIVAL:
                case DBT_DEVICEREMOVECOMPLETE:
                case DBT_DEVNODES_CHANGED:
                    /* a volume came or went: a moment later, once all of a disk's volumes are there */
                    SetTimer(pSysTray->GetHWnd(), HOTPLUG_DEVICE_TIMER_ID, 300, NULL);
                    lResult = TRUE;
                    break;
            }
            return S_FALSE;

        default:
            return S_FALSE;
    }

    return S_FALSE;
}
