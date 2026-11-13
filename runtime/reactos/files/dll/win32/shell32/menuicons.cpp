/*
 * PROJECT:     Arctic's shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The pictures of menu items, the way Windows 11's context menus
 *              have them
 *
 * Windows 11 gives each of the system's commands in its context menus a
 * small picture: grey lines, blue where the command acts, white inside. Ours
 * are drawn by tools/menu-icons.py into res/menuicons.bin; this file paints
 * them in the colours of the menu (light or dark) at the size of its small
 * icons, and gives them to menu items as 32-bit bitmaps with alpha, which
 * win32u blends (Wine patch 0125). Commands that run a program (Open, Edit)
 * show that program's icon, as in Windows 11, and a verb with an Icon value
 * in the registry shows that, as in every Windows since 7.
 *
 * Only with the visual style's size "Windows 11": Windows 10's menus have
 * no pictures.
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* the roles of a picture's parts, as tools/menu-icons.py numbers them */
enum { ROLE_LINES, ROLE_ACCENT, ROLE_INSIDE, ROLE_WHITE, ROLE_DARK, ROLE_GREY, ROLE_COUNT };

/* light menu, dark menu; CLR_NONE: not painted */
static const COLORREF s_Colours[2][ROLE_COUNT] =
{
    { RGB(0x42, 0x42, 0x42), RGB(0x00, 0x78, 0xD4), RGB(0xFF, 0xFF, 0xFF),
      RGB(0xFF, 0xFF, 0xFF), RGB(0x2B, 0x2B, 0x2B), RGB(0x6E, 0x6E, 0x6E) },
    { RGB(0xDE, 0xDE, 0xDE), RGB(0x4C, 0xC2, 0xFF), CLR_NONE,
      RGB(0xFF, 0xFF, 0xFF), RGB(0x2B, 0x2B, 0x2B), RGB(0x6E, 0x6E, 0x6E) },
};

#define MAX_SIZES  8
#define MAX_ICONS  128
#define MAX_CACHE  160

struct MENU_PICTURE
{
    char name[32];
    BYTE roles;                       /* bit mask */
    const BYTE *data[MAX_SIZES];      /* run-length coverage, the roles in order */
};

struct CACHED_BITMAP
{
    WCHAR key[MAX_PATH + 48];
    HBITMAP hbm;
};

static CRITICAL_SECTION s_cs;
static LONG s_csState;               /* 0 none, 1 being made, 2 ready */
static BOOL s_bParsed;
static UINT s_nSizes;
static BYTE s_Sizes[MAX_SIZES];
static UINT s_nPictures;
static MENU_PICTURE s_Pictures[MAX_ICONS];
static CACHED_BITMAP s_Cache[MAX_CACHE];
static UINT s_nCache, s_iCacheNext;

static void MenuIconsLock()
{
    if (InterlockedCompareExchange(&s_csState, 1, 0) == 0)
    {
        InitializeCriticalSection(&s_cs);
        InterlockedExchange(&s_csState, 2);
    }
    while (s_csState != 2)
        Sleep(0);
    EnterCriticalSection(&s_cs);
}

static void MenuIconsUnlock()
{
    LeaveCriticalSection(&s_cs);
}

/* the pictures are for menus of the visual style's size "Windows 11" */
EXTERN_C BOOL SHELL_MenuIconsWanted(void)
{
    static DWORD s_dwChecked;
    static BOOL s_bWanted;
    DWORD dwNow = GetTickCount();

    if (s_dwChecked && dwNow - s_dwChecked < 1000)
        return s_bWanted;
    WCHAR szSize[64] = L"";
    DWORD cb = sizeof(szSize);
    BOOL bWanted = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",
                                L"SizeName", RRF_RT_REG_SZ, NULL, szSize, &cb) == ERROR_SUCCESS &&
                   !wcscmp(szSize, L"Windows 11");
    s_bWanted = bWanted;
    s_dwChecked = dwNow ? dwNow : 1;
    return bWanted;
}

/* the size of a picture: the menu's small icons, at most the largest drawn */
static UINT WantedSize()
{
    int cx = GetSystemMetrics(SM_CXSMICON);
    return cx > 0 ? cx : 16;
}

static BOOL MenuIsDark()
{
    COLORREF c = GetSysColor(COLOR_MENU);
    return GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114 < 128 * 1000;
}

/* the length of one run-length stream of size * size bytes */
static const BYTE *SkipStream(const BYTE *p, const BYTE *end, UINT cPixels)
{
    UINT n = 0;
    while (n < cPixels && p + 2 <= end)
    {
        UINT cLiteral = p[1];
        n += p[0] + cLiteral;
        p += 2 + cLiteral;
    }
    return (n == cPixels && p <= end) ? p : NULL;
}

static void DecodeStream(const BYTE *p, BYTE *out, UINT cPixels)
{
    UINT n = 0;
    while (n < cPixels)
    {
        UINT cZeros = p[0], cLiteral = p[1];
        p += 2;
        for (UINT i = 0; i < cZeros && n < cPixels; i++)
            out[n++] = 0;
        for (UINT i = 0; i < cLiteral && n < cPixels; i++)
            out[n++] = *p++;
    }
}

static BOOL Parse()
{
    if (s_bParsed)
        return s_nPictures != 0;
    s_bParsed = TRUE;

    HRSRC hRes = FindResourceW(shell32_hInstance, L"MENUICONS", (LPCWSTR)RT_RCDATA);
    HGLOBAL hData = hRes ? LoadResource(shell32_hInstance, hRes) : NULL;
    const BYTE *p = hData ? (const BYTE *)LockResource(hData) : NULL;
    DWORD cb = hRes ? SizeofResource(shell32_hInstance, hRes) : 0;
    if (!p || cb < 8 || memcmp(p, "AMI1", 4))
    {
        ERR("no pictures for menus\n");
        return FALSE;
    }
    const BYTE *end = p + cb;
    UINT cIcons = p[4] | (p[5] << 8);
    s_nSizes = min(p[6], MAX_SIZES);
    p += 7;
    for (UINT i = 0; i < s_nSizes; i++)
        s_Sizes[i] = p[i];
    p += p[-1];

    for (UINT i = 0; i < cIcons && s_nPictures < MAX_ICONS && p < end; i++)
    {
        MENU_PICTURE *pic = &s_Pictures[s_nPictures];
        UINT cchName = *p++;
        if (p + cchName + 1 > end)
            break;
        memcpy(pic->name, p, min(cchName, sizeof(pic->name) - 1));
        pic->name[min(cchName, sizeof(pic->name) - 1)] = 0;
        p += cchName;
        pic->roles = *p++;
        for (UINT s = 0; s < s_nSizes && p; s++)
        {
            pic->data[s] = p;
            for (UINT r = 0; r < ROLE_COUNT && p; r++)
            {
                if (pic->roles & (1 << r))
                    p = SkipStream(p, end, s_Sizes[s] * s_Sizes[s]);
            }
        }
        if (!p)
            break;
        s_nPictures++;
    }
    TRACE("%u pictures for menus\n", s_nPictures);
    return s_nPictures != 0;
}

static HBITMAP CreateBitmap32(UINT cx, UINT cy, DWORD **ppBits)
{
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), (LONG)cx, -(LONG)cy, 1, 32, BI_RGB } };
    return CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)ppBits, NULL, 0);
}

static HBITMAP FindCached(LPCWSTR pszKey)
{
    for (UINT i = 0; i < s_nCache; i++)
    {
        if (!wcscmp(s_Cache[i].key, pszKey))
            return s_Cache[i].hbm;
    }
    return NULL;
}

/* kept for the process: a menu may still show a bitmap given out long ago,
   so only the oldest of many goes */
static void AddCached(LPCWSTR pszKey, HBITMAP hbm)
{
    UINT i;
    if (s_nCache < MAX_CACHE)
        i = s_nCache++;
    else
    {
        i = s_iCacheNext;
        s_iCacheNext = (s_iCacheNext + 1) % MAX_CACHE;
        DeleteObject(s_Cache[i].hbm);
    }
    StringCchCopyW(s_Cache[i].key, _countof(s_Cache[i].key), pszKey);
    s_Cache[i].hbm = hbm;
}

static HBITMAP PaintPicture(const MENU_PICTURE *pic, UINT iSize, BOOL bDark)
{
    UINT size = s_Sizes[iSize], cPixels = size * size;
    DWORD *pBits;
    HBITMAP hbm = CreateBitmap32(size, size, &pBits);
    if (!hbm)
        return NULL;

    UINT *sum = (UINT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cPixels * 4 * sizeof(UINT));
    BYTE *cov = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cPixels);
    if (!sum || !cov)
    {
        HeapFree(GetProcessHeap(), 0, sum);
        HeapFree(GetProcessHeap(), 0, cov);
        DeleteObject(hbm);
        return NULL;
    }

    const BYTE *p = pic->data[iSize];
    for (UINT r = 0; r < ROLE_COUNT; r++)
    {
        if (!(pic->roles & (1 << r)))
            continue;
        const BYTE *next = SkipStream(p, p + 65536, cPixels);
        COLORREF c = s_Colours[bDark][r];
        if (c != CLR_NONE)
        {
            DecodeStream(p, cov, cPixels);
            for (UINT i = 0; i < cPixels; i++)
            {
                UINT a = cov[i];
                sum[i * 4 + 0] += a * GetBValue(c);
                sum[i * 4 + 1] += a * GetGValue(c);
                sum[i * 4 + 2] += a * GetRValue(c);
                sum[i * 4 + 3] += a * 255;
            }
        }
        p = next;
    }
    /* premultiplied, as AlphaBlend takes it */
    for (UINT i = 0; i < cPixels; i++)
    {
        UINT b = min(sum[i * 4 + 0] / 255, 255u), g = min(sum[i * 4 + 1] / 255, 255u);
        UINT rr = min(sum[i * 4 + 2] / 255, 255u), a = min(sum[i * 4 + 3] / 255, 255u);
        pBits[i] = (a << 24) | (rr << 16) | (g << 8) | b;
    }
    HeapFree(GetProcessHeap(), 0, sum);
    HeapFree(GetProcessHeap(), 0, cov);
    return hbm;
}

/* one of our pictures, for the menus of now (size, light or dark) */
EXTERN_C HBITMAP SHELL_GetMenuPicture(LPCSTR pszName)
{
    HBITMAP hbm = NULL;
    BOOL bDark = MenuIsDark();
    UINT wanted = WantedSize();

    MenuIconsLock();
    if (Parse())
    {
        UINT iSize = 0;
        for (UINT i = 0; i < s_nSizes; i++)
        {
            if (s_Sizes[i] <= wanted)
                iSize = i;
        }
        WCHAR szKey[64];
        StringCchPrintfW(szKey, _countof(szKey), L"*%S/%u/%d", pszName, s_Sizes[iSize], bDark);
        if (!(hbm = FindCached(szKey)))
        {
            for (UINT i = 0; i < s_nPictures; i++)
            {
                if (!strcmp(s_Pictures[i].name, pszName))
                {
                    if ((hbm = PaintPicture(&s_Pictures[i], iSize, bDark)))
                        AddCached(szKey, hbm);
                    break;
                }
            }
            if (!hbm)
                WARN("no picture %s\n", debugstr_a(pszName));
        }
    }
    MenuIconsUnlock();
    return hbm;
}

/* an icon as a bitmap with alpha: drawn on black and on white, the
   difference gives its coverage, whatever kind of icon it is */
static HBITMAP BitmapFromIcon(HICON hIcon, UINT size)
{
    DWORD *pBlack, *pWhite, *pOut;
    HBITMAP hbmBlack = CreateBitmap32(size, size, &pBlack);
    HBITMAP hbmWhite = CreateBitmap32(size, size, &pWhite);
    HBITMAP hbm = CreateBitmap32(size, size, &pOut);
    HDC hdc = CreateCompatibleDC(NULL);
    BOOL bOk = hbmBlack && hbmWhite && hbm && hdc;

    if (bOk)
    {
        UINT cPixels = size * size;
        for (UINT i = 0; i < cPixels; i++)
        {
            pBlack[i] = 0;
            pWhite[i] = 0x00FFFFFF;
        }
        HGDIOBJ hOld = SelectObject(hdc, hbmBlack);
        DrawIconEx(hdc, 0, 0, hIcon, size, size, 0, NULL, DI_NORMAL);
        SelectObject(hdc, hbmWhite);
        DrawIconEx(hdc, 0, 0, hIcon, size, size, 0, NULL, DI_NORMAL);
        SelectObject(hdc, hOld);
        GdiFlush();
        BOOL bAny = FALSE;
        for (UINT i = 0; i < cPixels; i++)
        {
            DWORD b = pBlack[i], w = pWhite[i];
            int dMax = 0;
            for (int sh = 0; sh < 24; sh += 8)
            {
                int d = (int)((w >> sh) & 0xff) - (int)((b >> sh) & 0xff);
                dMax = max(dMax, d);
            }
            UINT a = 255 - dMax;
            UINT bb = min(b & 0xff, a), gg = min((b >> 8) & 0xff, a), rr = min((b >> 16) & 0xff, a);
            pOut[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            bAny |= (a != 0);
        }
        bOk = bAny;
    }
    if (hdc)
        DeleteDC(hdc);
    if (hbmBlack)
        DeleteObject(hbmBlack);
    if (hbmWhite)
        DeleteObject(hbmWhite);
    if (!bOk && hbm)
    {
        DeleteObject(hbm);
        hbm = NULL;
    }
    return hbm;
}

/* the icon of a file (a program's, mostly) for a menu item */
EXTERN_C HBITMAP SHELL_GetMenuPictureOfFile(LPCWSTR pszPath, INT iIndex)
{
    WCHAR szPath[MAX_PATH], szKey[MAX_PATH + 48];
    UINT size = WantedSize();
    HBITMAP hbm;

    if (!pszPath || !*pszPath || !ExpandEnvironmentStringsW(pszPath, szPath, _countof(szPath)))
        return NULL;
    StringCchPrintfW(szKey, _countof(szKey), L"%s,%d/%u", szPath, iIndex, size);
    CharLowerW(szKey);

    MenuIconsLock();
    hbm = FindCached(szKey);
    MenuIconsUnlock();
    if (hbm)
        return hbm;

    HICON hIcon = NULL;
    if (PrivateExtractIconsW(szPath, iIndex, size, size, &hIcon, NULL, 1, 0) != 1 || !hIcon)
    {
        hIcon = NULL;
        ExtractIconExW(szPath, iIndex, NULL, &hIcon, 1);
    }
    if (!hIcon)
        return NULL;
    hbm = BitmapFromIcon(hIcon, size);
    DestroyIcon(hIcon);
    if (!hbm)
        return NULL;

    MenuIconsLock();
    HBITMAP hbmOther = FindCached(szKey);
    if (hbmOther)
    {
        DeleteObject(hbm);
        hbm = hbmOther;
    }
    else
        AddCached(szKey, hbm);
    MenuIconsUnlock();
    return hbm;
}

static BOOL SetItemBitmap(HMENU hMenu, UINT uItem, BOOL fByPos, HBITMAP hbm)
{
    MENUITEMINFOW mii = { sizeof(mii), MIIM_BITMAP };
    if (!hbm || !GetMenuItemInfoW(hMenu, uItem, fByPos, &mii))
        return FALSE;
    if (mii.hbmpItem)
        return TRUE; /* the item has its own picture */
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = hbm;
    return SetMenuItemInfoW(hMenu, uItem, fByPos, &mii);
}

EXTERN_C BOOL SHELL_SetMenuIcon(HMENU hMenu, UINT uItem, BOOL fByPos, LPCSTR pszName)
{
    if (!hMenu || !SHELL_MenuIconsWanted())
        return FALSE;
    return SetItemBitmap(hMenu, uItem, fByPos, SHELL_GetMenuPicture(pszName));
}

EXTERN_C BOOL SHELL_SetMenuIconFile(HMENU hMenu, UINT uItem, BOOL fByPos, LPCWSTR pszPath, INT iIndex)
{
    if (!hMenu || !SHELL_MenuIconsWanted())
        return FALSE;
    return SetItemBitmap(hMenu, uItem, fByPos, SHELL_GetMenuPictureOfFile(pszPath, iIndex));
}

/* "path,index", as Icon values and DefaultIcon have it */
EXTERN_C BOOL SHELL_SetMenuIconLocation(HMENU hMenu, UINT uItem, BOOL fByPos, LPCWSTR pszLocation)
{
    WCHAR szPath[MAX_PATH];
    INT iIndex = 0;

    if (!pszLocation || !*pszLocation)
        return FALSE;
    StringCchCopyW(szPath, _countof(szPath), pszLocation);
    PathUnquoteSpacesW(szPath);
    LPWSTR pszComma = wcsrchr(szPath, L',');
    if (pszComma && pszComma > wcsrchr(szPath, L'\\'))
    {
        *pszComma = 0;
        iIndex = _wtoi(pszComma + 1);
        PathUnquoteSpacesW(szPath);
    }
    return SHELL_SetMenuIconFile(hMenu, uItem, fByPos, szPath, iIndex);
}

/* the system's verbs and their pictures */
static const struct
{
    LPCWSTR pszVerb;
    LPCSTR pszPicture;
} s_VerbPictures[] =
{
    { L"cut",                    "cut" },
    { L"copy",                   "copy" },
    { L"paste",                  "paste" },
    { L"pastelink",              "pastelink" },
    { L"delete",                 "delete" },
    { L"rename",                 "rename" },
    { L"properties",             "properties" },
    { L"link",                   "shortcut" },
    { L"copyaspath",             "copypath" },
    { L"windows.copyaspath",     "copypath" },
    { L"copyto",                 "copyto" },
    { L"moveto",                 "moveto" },
    { L"find",                   "search" },
    { L"explore",                "explore" },
    { L"openas",                 "openwith" },
    { L"runas",                  "runas" },
    { L"runasuser",              "runas" },
    { L"print",                  "print" },
    { L"printto",                "print" },
    { L"powershell",             "terminal" },
    { L"cmd",                    "terminal" },
    { L"connectNetworkDrive",    "mapdrive" },
    { L"disconnectNetworkDrive", "unmapdrive" },
    { L"Manage",                 "shield" },
    { L"empty",                  "delete" },
    { L"pintohome",              "pin" },
    { L"pintostartscreen",       "pin" },
    { L"taskbarpin",             "pin" },
    { L"windows.pintotaskbar",   "pin" },
    { L"unpinfromhome",          "unpin" },
    { L"taskbarunpin",           "unpin" },
    { L"windows.unpinfromtaskbar", "unpin" },
    { L"eject",                  "eject" },
    { L"format",                 "format" },
    { L"opennewwindow",          "open" },
    { L"opennewtab",             "open" },
    { L"undelete",               "undo" },
    { L"restore",                "undo" },
    { L"undo",                   "undo" },
    { L"redo",                   "redo" },
    { L"newfolder",              "folder" },
    { L"share",                  "share" },
    { L"sendto",                 "sendto" },
};

EXTERN_C LPCSTR SHELL_MenuPictureOfVerb(LPCWSTR pszVerb)
{
    if (!pszVerb || !*pszVerb)
        return NULL;
    for (UINT i = 0; i < _countof(s_VerbPictures); i++)
    {
        if (!_wcsicmp(s_VerbPictures[i].pszVerb, pszVerb))
            return s_VerbPictures[i].pszPicture;
    }
    return NULL;
}

/* the program a command line runs: quoted, or up to ".exe", or the first
   word; rundll32's is the library it loads */
EXTERN_C BOOL SHELL_ProgramOfCommand(LPCWSTR pszCommand, LPWSTR pszExe, UINT cchExe)
{
    WCHAR szCmd[MAX_PATH * 2], szExe[MAX_PATH];
    LPCWSTR p, pszArgs;

    if (!pszCommand || !ExpandEnvironmentStringsW(pszCommand, szCmd, _countof(szCmd)))
        return FALSE;
    p = szCmd;
    while (*p == L' ')
        p++;
    if (*p == L'"')
    {
        LPCWSTR q = wcschr(p + 1, L'"');
        if (!q)
            return FALSE;
        StringCchCopyNW(szExe, _countof(szExe), p + 1, q - p - 1);
        pszArgs = q + 1;
    }
    else
    {
        LPCWSTR q = StrStrIW(p, L".exe");
        if (q && (q[4] == 0 || q[4] == L' '))
            q += 4;
        else if (!(q = wcschr(p, L' ')))
            q = p + wcslen(p);
        StringCchCopyNW(szExe, _countof(szExe), p, q - p);
        pszArgs = q;
    }
    if (!szExe[0] || szExe[0] == L'%')
        return FALSE;

    if (!_wcsicmp(PathFindFileNameW(szExe), L"rundll32.exe") ||
        !_wcsicmp(PathFindFileNameW(szExe), L"rundll32"))
    {
        while (*pszArgs == L' ')
            pszArgs++;
        WCHAR szDll[MAX_PATH];
        StringCchCopyW(szDll, _countof(szDll), pszArgs);
        LPWSTR pszComma = wcschr(szDll, L',');
        if (pszComma)
            *pszComma = 0;
        PathUnquoteSpacesW(szDll);
        if (szDll[0])
            StringCchCopyW(szExe, _countof(szExe), szDll);
    }

    if (PathIsRelativeW(szExe))
    {
        WCHAR szFound[MAX_PATH];
        if (SearchPathW(NULL, szExe, L".exe", _countof(szFound), szFound, NULL))
            StringCchCopyW(szExe, _countof(szExe), szFound);
    }
    return SUCCEEDED(StringCchCopyW(pszExe, cchExe, szExe));
}

/* for other parts of the system (ntshrui, explorer): one of our pictures on
   a menu item, with the style of Windows 11 only */
EXTERN_C BOOL WINAPI ArcticSetMenuIcon(HMENU hMenu, UINT uItem, BOOL fByPos, LPCSTR pszName)
{
    return SHELL_SetMenuIcon(hMenu, uItem, fByPos, pszName);
}
