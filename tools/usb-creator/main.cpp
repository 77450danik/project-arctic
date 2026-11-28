// Arctic USB Creator: pick a USB drive, it is cleared (quick format: volumes
// dismounted, partition table wiped), the newest Arctic ISO is downloaded and
// written to it byte for byte (the ISO is a hybrid image: BIOS and UEFI), and
// the written drive is read back and checked against the image's SHA-256.
//
// Only standard Windows APIs, so it runs the same on Windows and on Arctic:
// Direct2D/DirectWrite (UI), WinHTTP (download), BCrypt (SHA-256), disk IOCTLs
// on \\.\PhysicalDriveN (write), TaskDialog and the common file dialog.
// Build: build.cmd (llvm-mingw).

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00  // Windows 10: per-monitor DPI
#define NTDDI_VERSION 0x0A000006
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <dbt.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define LATEST_URL L"https://77450danik.github.io/project-arctic/latest.json"
#define APP_NAME   L"Arctic USB Creator"

enum { WM_APP_LATEST = WM_APP + 1, WM_APP_TICK, WM_APP_FINISHED };
enum { TIMER_REFRESH = 1, TIMER_TICK = 2 };

// ---------------------------------------------------------------- strings
struct Strings {
    const wchar_t *heading, *drive, *refresh, *noDrive, *image, *imgLoading, *imgFailed,
        *retry, *otherFile, *cached, *warn, *write, *cancel, *stDownload, *stFormat, *stWrite,
        *stVerify, *done, *left, *confirmMain, *confirmContent, *confirmButton, *gb, *mbs,
        *minutes, *seconds, *errOpen, *errWrite, *errDownload, *errHash, *errVerify,
        *errCanceled, *errAdmin, *errChanged, *errRead, *localFile, *noLetter, *stopMain,
        *stopButton;
    const wchar_t *months[12];
};

static const Strings UK = {
    L"Записати Arctic на флешку", L"Флешка", L"Оновити", L"Вставте флешку", L"Образ",
    L"Шукаю останній образ…", L"Не вдалося отримати останній образ", L"Повторити",
    L"Інший файл…", L"уже завантажено", L"Усе на «%s» буде стерто.", L"Записати",
    L"Скасувати", L"Завантаження", L"Форматування", L"Запис", L"Перевірка",
    L"Готово. Флешку можна виймати.", L"залишилось", L"Стерти «%s»?",
    L"Усі файли на флешці (%s) буде видалено, на неї запишеться Arctic.",
    L"Стерти й записати", L"ГБ", L"МБ/с", L"хв", L"с",
    L"Не вдалося відкрити флешку (код %lu)", L"Не вдалося записати на флешку (код %lu)",
    L"Завантаження перервалося (код %lu)", L"Контрольна сума не збіглася: файл пошкоджено",
    L"Записане не збігається з образом: флешка, можливо, несправна", L"Скасовано",
    L"Потрібні права адміністратора", L"Флешку замінили або вийняли",
    L"Не вдалося прочитати образ (код %lu)", L"Свій файл", L"USB",
    L"Зупинити запис? Флешка лишиться незавантажувальною.", L"Зупинити",
    {L"січня", L"лютого", L"березня", L"квітня", L"травня", L"червня", L"липня",
     L"серпня", L"вересня", L"жовтня", L"листопада", L"грудня"}};

static const Strings EN = {
    L"Put Arctic on a USB drive", L"USB drive", L"Refresh", L"Plug in a USB drive", L"Image",
    L"Looking for the latest image…", L"Couldn't get the latest image", L"Retry",
    L"Other file…", L"already downloaded", L"Everything on “%s” will be erased.", L"Write",
    L"Cancel", L"Downloading", L"Formatting", L"Writing", L"Verifying",
    L"Done. You can remove the drive.", L"left", L"Erase “%s”?",
    L"All files on the drive (%s) will be deleted and Arctic written to it.",
    L"Erase and write", L"GB", L"MB/s", L"min", L"s",
    L"Couldn't open the drive (code %lu)", L"Couldn't write to the drive (code %lu)",
    L"The download stopped (code %lu)", L"Checksum mismatch: the file is damaged",
    L"What was written differs from the image: the drive may be faulty", L"Cancelled",
    L"Administrator rights are needed", L"The drive was replaced or removed",
    L"Couldn't read the image (code %lu)", L"Own file", L"USB",
    L"Stop writing? The drive will not boot.", L"Stop",
    {L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August",
     L"September", L"October", L"November", L"December"}};

static const Strings *T = &EN;
static bool g_uk = false;

// ---------------------------------------------------------------- state
struct Drive {
    int num;
    ULONGLONG size;
    wchar_t name[96];
    wchar_t letters[48];
};

struct ImageInfo {
    bool ready, failed, local, cached;
    wchar_t name[64], date[64], url[1024], path[MAX_PATH];
    ULONGLONG size;
    char sha[65];  // expected SHA-256 (hex), empty for a local file
};

enum Stage { ST_IDLE, ST_DOWNLOAD, ST_FORMAT, ST_WRITE, ST_VERIFY, ST_DONE, ST_ERROR };

struct Progress {
    CRITICAL_SECTION cs;
    int stage;
    ULONGLONG done, total;
    double speed;  // bytes per second, smoothed
    wchar_t error[256];
};

static HWND g_hwnd;
static Drive g_drives[16];
static int g_ndrives, g_sel = -1;
static ImageInfo g_img;
static Progress g_prog;
static volatile LONG g_busy, g_cancel;
static bool g_closeAfter;
static wchar_t g_cacheDir[MAX_PATH];

// ---------------------------------------------------------------- helpers
static void fmt_size(wchar_t *out, size_t n, ULONGLONG bytes) {
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    swprintf(out, n, gb >= 10 ? L"%.1f %s" : L"%.2f %s", gb, T->gb);
    if (g_uk)
        for (wchar_t *p = out; *p; p++)
            if (*p == L'.') *p = L',';
}

static void trim(wchar_t *s) {
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == L' ') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}

static void set_error(const wchar_t *fmt, DWORD code) {
    EnterCriticalSection(&g_prog.cs);
    swprintf(g_prog.error, 256, fmt, code);
    g_prog.stage = ST_ERROR;
    LeaveCriticalSection(&g_prog.cs);
}

static void set_stage(int st, ULONGLONG total) {
    EnterCriticalSection(&g_prog.cs);
    g_prog.stage = st;
    g_prog.done = 0;
    g_prog.total = total;
    g_prog.speed = 0;
    LeaveCriticalSection(&g_prog.cs);
    PostMessageW(g_hwnd, WM_APP_TICK, 0, 0);
}

// progress with a smoothed speed; posts a repaint at most ~10 times a second
static void add_progress(ULONGLONG n) {
    static ULONGLONG lastTick, lastDone;
    EnterCriticalSection(&g_prog.cs);
    g_prog.done += n;
    ULONGLONG now = GetTickCount64();
    if (lastTick == 0 || g_prog.done < lastDone) { lastTick = now; lastDone = g_prog.done; }
    if (now - lastTick >= 500) {
        double inst = (g_prog.done - lastDone) * 1000.0 / (now - lastTick);
        g_prog.speed = g_prog.speed == 0 ? inst : g_prog.speed * 0.7 + inst * 0.3;
        lastTick = now;
        lastDone = g_prog.done;
    }
    LeaveCriticalSection(&g_prog.cs);
}

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool init() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
        return BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    void reset() {
        if (h) BCryptDestroyHash(h), h = nullptr;
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    void add(const void *p, ULONG n) { BCryptHashData(h, (PUCHAR)p, n, 0); }
    void hex(char out[65]) {
        BYTE d[32];
        BCryptFinishHash(h, d, 32, 0);
        for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

// ---------------------------------------------------------------- HTTP
// GET url (from byte `from`), feeding the body to `sink`. Returns the HTTP
// status (0 on a transport error, *err set); *length = Content-Length if given.
typedef bool (*Sink)(const BYTE *p, DWORD n, void *ctx);

static DWORD http_get(const wchar_t *url, ULONGLONG from, Sink sink, void *ctx,
                      ULONGLONG *length, DWORD *err) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    wchar_t extra[1024] = L"";
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    *err = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { *err = GetLastError(); return 0; }
    wcsncat(path, extra, 2048 - wcslen(path) - 1);

    HINTERNET s = WinHttpOpen(L"ArcticUsbCreator/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ArcticUsbCreator/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    DWORD status = 0;
    if (r) {
        DWORD to = 60000;
        WinHttpSetOption(r, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
        wchar_t range[64] = L"";
        if (from) swprintf(range, 64, L"Range: bytes=%llu-\r\n", from);
        if (WinHttpSendRequest(r, from ? range : WINHTTP_NO_ADDITIONAL_HEADERS, from ? (DWORD)-1 : 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD sz = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            wchar_t len[32];
            sz = sizeof len;
            *length = 0;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                    len, &sz, WINHTTP_NO_HEADER_INDEX))
                *length = _wcstoui64(len, nullptr, 10);
            if (status == 200 || status == 206) {
                static BYTE buf[256 * 1024];
                for (;;) {
                    DWORD got = 0;
                    if (g_cancel) break;
                    if (!WinHttpReadData(r, buf, sizeof buf, &got)) { *err = GetLastError(); break; }
                    if (got == 0) break;
                    if (!sink(buf, got, ctx)) { *err = ERROR_WRITE_FAULT; break; }
                }
            }
        } else
            *err = GetLastError();
    } else
        *err = GetLastError();
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return *err ? 0 : status;
}

// ---------------------------------------------------------------- latest.json
struct MemBuf { char *p; size_t n, cap; };
static bool mem_sink(const BYTE *p, DWORD n, void *ctx) {
    MemBuf *m = (MemBuf *)ctx;
    if (m->n + n + 1 > m->cap) return false;
    memcpy(m->p + m->n, p, n);
    m->n += n;
    m->p[m->n] = 0;
    return true;
}

// the string or number value after "key": (after `from`, if given)
static bool json_value(const char *json, const char *key, char *out, size_t cap, const char *from = nullptr) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(from ? from : json, pat);
    if (!k) return false;
    k = strchr(k + strlen(pat), ':');
    if (!k) return false;
    k++;
    while (*k == ' ' || *k == '\n' || *k == '\r' || *k == '\t') k++;
    size_t i = 0;
    if (*k == '"') {
        k++;
        while (*k && *k != '"' && i + 1 < cap) out[i++] = *k++;
    } else
        while (*k && (*k >= '0' && *k <= '9') && i + 1 < cap) out[i++] = *k++;
    out[i] = 0;
    return i > 0;
}

static void cache_path(wchar_t *out, const char *sha, const wchar_t *ext) {
    swprintf(out, MAX_PATH, L"%s\\arctic-%.12S%s", g_cacheDir, sha, ext);
}

static DWORD WINAPI latest_thread(void *) {
    static char buf[16384];
    MemBuf m = {buf, 0, sizeof buf};
    ULONGLONG len;
    DWORD err;
    ImageInfo img = {};
    char v[1024];
    DWORD st = http_get(LATEST_URL, 0, mem_sink, &m, &len, &err);
    const char *iso = st == 200 ? strstr(buf, "\"iso\"") : nullptr;
    if (iso && json_value(buf, "url", v, sizeof v, iso)) {
        MultiByteToWideChar(CP_UTF8, 0, v, -1, img.url, 1024);
        if (json_value(buf, "size", v, sizeof v, iso)) img.size = _strtoui64(v, nullptr, 10);
        json_value(buf, "sha256", img.sha, sizeof img.sha, iso);
        if (json_value(buf, "name", v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, img.name, 64);
        int y, mo, d;
        if (json_value(buf, "date", v, sizeof v) && sscanf(v, "%d-%d-%d", &y, &mo, &d) == 3 && mo >= 1 && mo <= 12) {
            if (g_uk) swprintf(img.date, 64, L"%d %s %d", d, T->months[mo - 1], y);
            else swprintf(img.date, 64, L"%s %d, %d", T->months[mo - 1], d, y);
        }
        img.ready = img.size > 0 && strlen(img.sha) == 64;
        if (img.ready) {
            cache_path(img.path, img.sha, L".iso");
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(img.path, GetFileExInfoStandard, &fa) &&
                (((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == img.size)
                img.cached = true;
        }
    }
    img.failed = !img.ready;
    if (!g_img.local) {
        g_img = img;
        PostMessageW(g_hwnd, WM_APP_LATEST, 0, 0);
    }
    return 0;
}

// ---------------------------------------------------------------- drives
// The disk a volume lives on. Two independent answers must agree (the volume's
// extents and its storage device number): a volume we are not sure about is
// never shown on a drive and never dismounted.
static bool disk_of_volume(const wchar_t *vol, DWORD *disk) {
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE buf[1024];
    DWORD got;
    bool ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof buf, &got, nullptr);
    STORAGE_DEVICE_NUMBER dn = {};
    bool ok2 = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof dn, &got, nullptr);
    CloseHandle(h);
    VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)buf;
    if (!ok || e->NumberOfDiskExtents != 1) return false;
    if (ok2 && dn.DeviceNumber != e->Extents[0].DiskNumber) return false;
    *disk = e->Extents[0].DiskNumber;
    return true;
}

// "ADATA USB Flash Drive" from the device's friendly name (SetupAPI), by disk number
static bool friendly_name(int disk, wchar_t *out, size_t cap) {
    static const GUID DISK = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
    HDEVINFO di = SetupDiGetClassDevsW(&DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    bool found = false;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, nullptr, &DISK, i, &ifd); i++) {
        BYTE buf[2048];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof *det;
        SP_DEVINFO_DATA dd = {sizeof dd};
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, sizeof buf, nullptr, &dd)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER dn = {};
        DWORD got;
        bool ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof dn, &got, nullptr);
        CloseHandle(h);
        if (!ok || (int)dn.DeviceNumber != disk) continue;
        wchar_t fn[256] = L"";
        if (SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_FRIENDLYNAME, nullptr, (BYTE *)fn, sizeof fn - 2, nullptr) && *fn) {
            size_t n = wcslen(fn);
            if (n > 11 && wcscmp(fn + n - 11, L" USB Device") == 0) fn[n - 11] = 0;
            swprintf(out, cap, L"%s", fn);
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}

static int system_disk() {
    wchar_t win[MAX_PATH], vol[8];
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    swprintf(vol, 8, L"\\\\.\\%c:", win[0]);
    DWORD d;
    return disk_of_volume(vol, &d) ? (int)d : -1;
}

// A USB or SD drive with removable media: flash drives and card readers.
// USB SSDs and hard drives report fixed media and are never listed (Rufus
// hides them by default too): they usually hold someone's data.
static bool usb_disk_info(HANDLE h, ULONGLONG *size, wchar_t *name, size_t ncap) {
    STORAGE_PROPERTY_QUERY q = {StorageDeviceProperty, PropertyStandardQuery};
    BYTE buf[1024] = {};
    DWORD got;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf - 1, &got, nullptr))
        return false;
    STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    if (d->BusType != BusTypeUsb && d->BusType != BusTypeSd && d->BusType != BusTypeMmc) return false;
    if (!d->RemovableMedia) return false;
    got = sizeof buf - 1;  // some drivers report fewer bytes than they filled
    GET_LENGTH_INFORMATION li;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof li, &got, nullptr) ||
        li.Length.QuadPart == 0)
        return false;
    *size = li.Length.QuadPart;
    char ven[64] = "", prod[96] = "";
    if (d->VendorIdOffset && d->VendorIdOffset < got) snprintf(ven, sizeof ven, "%s", (char *)buf + d->VendorIdOffset);
    if (d->ProductIdOffset && d->ProductIdOffset < got) snprintf(prod, sizeof prod, "%s", (char *)buf + d->ProductIdOffset);
    wchar_t wv[64], wp[96];
    MultiByteToWideChar(CP_ACP, 0, ven, -1, wv, 64);
    MultiByteToWideChar(CP_ACP, 0, prod, -1, wp, 96);
    trim(wv);
    trim(wp);
    swprintf(name, ncap, L"%s%s%s", wv, *wv && *wp ? L" " : L"", wp);
    if (!*name) wcscpy(name, L"USB");
    return true;
}

static void enum_drives() {
    int prevNum = g_sel >= 0 ? g_drives[g_sel].num : -1;
    int sys = system_disk();
    g_ndrives = 0;
    for (int n = 0; n < 32 && g_ndrives < 16; n++) {
        if (n == sys) continue;
        wchar_t path[32];
        swprintf(path, 32, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        Drive d = {n};
        if (usb_disk_info(h, &d.size, d.name, 96)) {
            wchar_t fn[96];
            if (wcscmp(d.name, L"USB") == 0 && friendly_name(n, fn, 96)) wcscpy(d.name, fn);
            g_drives[g_ndrives++] = d;
        }
        CloseHandle(h);
    }
    for (wchar_t l = L'A'; l <= L'Z'; l++) {
        wchar_t root[4] = {l, L':', L'\\', 0}, vol[8];
        UINT t = GetDriveTypeW(root);
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) continue;
        swprintf(vol, 8, L"\\\\.\\%c:", l);
        DWORD disk;
        if (!disk_of_volume(vol, &disk)) continue;
        for (int i = 0; i < g_ndrives; i++)
            if (g_drives[i].num == (int)disk) {
                size_t k = wcslen(g_drives[i].letters);
                swprintf(g_drives[i].letters + k, 48 - k, L"%s%c:", k ? L", " : L"", l);
            }
    }
    g_sel = -1;
    for (int i = 0; i < g_ndrives; i++)
        if (g_drives[i].num == prevNum) g_sel = i;
    if (g_sel < 0 && g_ndrives == 1) g_sel = 0;
}

// ---------------------------------------------------------------- download
struct FileSink { HANDLE f; Sha256 *sha; };
static bool file_sink(const BYTE *p, DWORD n, void *ctx) {
    FileSink *s = (FileSink *)ctx;
    DWORD w;
    if (!WriteFile(s->f, p, n, &w, nullptr) || w != n) return false;
    s->sha->add(p, n);
    add_progress(n);
    return true;
}

static bool download_image() {
    wchar_t part[MAX_PATH];
    cache_path(part, g_img.sha, L".part");
    CreateDirectoryW(g_cacheDir, nullptr);
    set_stage(ST_DOWNLOAD, g_img.size);

    HANDLE f = CreateFileW(part, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { set_error(T->errDownload, GetLastError()); return false; }
    Sha256 sha;
    sha.init();
    LARGE_INTEGER have;
    GetFileSizeEx(f, &have);
    ULONGLONG from = have.QuadPart <= (LONGLONG)g_img.size ? have.QuadPart : 0;
    if (from) {  // resume: hash what is already there
        static BYTE buf[1 << 20];
        ULONGLONG left = from;
        while (left && !g_cancel) {
            DWORD n = (DWORD)(left < sizeof buf ? left : sizeof buf), got;
            if (!ReadFile(f, buf, n, &got, nullptr) || got != n) { from = 0; break; }
            sha.add(buf, n);
            add_progress(n);
            left -= n;
        }
    }
    if (!from) {
        sha.reset();
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        set_stage(ST_DOWNLOAD, g_img.size);
    }
    LARGE_INTEGER end = {};
    SetFilePointerEx(f, end, nullptr, FILE_END);

    FileSink fs = {f, &sha};
    ULONGLONG len;
    DWORD err;
    DWORD st = from < g_img.size ? http_get(g_img.url, from, file_sink, &fs, &len, &err) : 206;
    if (st == 200 && from) {  // the server ignored the range: start over
        CloseHandle(f);
        DeleteFileW(part);
        return download_image();
    }
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    if (g_cancel) { set_error(T->errCanceled, 0); return false; }
    if ((st != 200 && st != 206) || (ULONGLONG)size.QuadPart != g_img.size) {
        set_error(T->errDownload, err ? err : st);
        return false;
    }
    char hex[65];
    sha.hex(hex);
    if (_stricmp(hex, g_img.sha) != 0) {
        DeleteFileW(part);
        set_error(T->errHash, 0);
        return false;
    }
    DeleteFileW(g_img.path);
    if (!MoveFileW(part, g_img.path)) { set_error(T->errDownload, GetLastError()); return false; }
    // older images in the cache are not needed any more
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%s\\arctic-*.*", g_cacheDir);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%s\\%s", g_cacheDir, fd.cFileName);
            if (_wcsicmp(p, g_img.path) != 0) DeleteFileW(p);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    g_img.cached = true;
    return true;
}

// ---------------------------------------------------------------- write
struct Locked { HANDLE h[32]; int n; };

static void lock_volume(const wchar_t *vol, DWORD disk, Locked *lk) {
    DWORD d;
    if (!disk_of_volume(vol, &d) || d != disk || lk->n >= 32) return;
    HANDLE h = CreateFileW(vol, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got;
    for (int i = 0; i < 20 && !DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr); i++)
        Sleep(100);
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
    lk->h[lk->n++] = h;
}

static void lock_all_volumes(DWORD disk, Locked *lk) {
    wchar_t name[MAX_PATH];
    HANDLE fv = FindFirstVolumeW(name, MAX_PATH);
    if (fv != INVALID_HANDLE_VALUE) {
        do {
            size_t n = wcslen(name);
            if (n && name[n - 1] == L'\\') name[n - 1] = 0;  // \\?\Volume{...}
            lock_volume(name, disk, lk);
        } while (FindNextVolumeW(fv, name, MAX_PATH));
        FindVolumeClose(fv);
    }
    for (wchar_t l = L'A'; l <= L'Z'; l++) {
        wchar_t vol[8];
        swprintf(vol, 8, L"\\\\.\\%c:", l);
        lock_volume(vol, disk, lk);
    }
}

static bool write_image(const Drive &drv) {
    const ULONGLONG HEAD = 1 << 20;  // written last, so Windows never mounts a half-written image
    wchar_t path[32];
    swprintf(path, 32, L"\\\\.\\PhysicalDrive%d", drv.num);
    HANDLE disk = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (disk == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        set_error(e == ERROR_ACCESS_DENIED ? T->errAdmin : T->errOpen, e);
        return false;
    }
    // the same drive as chosen? (numbers move when drives come and go)
    ULONGLONG size = 0;
    wchar_t name[96];
    bool same = usb_disk_info(disk, &size, name, 96);
    if (same && wcscmp(name, L"USB") == 0) friendly_name(drv.num, name, 96);
    if (!same || size != drv.size || wcscmp(name, drv.name) != 0) {
        CloseHandle(disk);
        set_error(T->errChanged, 0);
        return false;
    }
    DWORD got, sector = 512;
    DISK_GEOMETRY_EX geo;
    if (DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &got, nullptr))
        sector = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : 512;

    HANDLE img = CreateFileW(g_img.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (img == INVALID_HANDLE_VALUE) { CloseHandle(disk); set_error(T->errRead, GetLastError()); return false; }
    LARGE_INTEGER isz;
    GetFileSizeEx(img, &isz);
    ULONGLONG total = isz.QuadPart;
    if (total > size) { CloseHandle(img); CloseHandle(disk); set_error(T->errWrite, ERROR_DISK_FULL); return false; }

    // quick format: no volumes, no partition table, nothing left at either end
    set_stage(ST_FORMAT, 4);
    Locked lk = {};
    lock_all_volumes(drv.num, &lk);
    add_progress(1);
    DeviceIoControl(disk, IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &got, nullptr);
    add_progress(1);
    const DWORD CHUNK = 4 << 20;
    BYTE *buf = (BYTE *)VirtualAlloc(nullptr, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE *head = (BYTE *)VirtualAlloc(nullptr, HEAD, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = buf && head;
    LARGE_INTEGER off;
    if (ok) {
        memset(buf, 0, HEAD);
        ULONGLONG ends[2] = {0, (size - HEAD) / sector * sector};
        for (ULONGLONG at : ends) {
            off.QuadPart = at;
            SetFilePointerEx(disk, off, nullptr, FILE_BEGIN);
            if (!WriteFile(disk, buf, (DWORD)HEAD, &got, nullptr)) { ok = false; break; }
        }
        DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
        if (!ok) set_error(T->errWrite, GetLastError());
    }
    add_progress(2);

    // write: everything after the first megabyte, then the first megabyte
    Sha256 sha;
    sha.init();
    if (ok) {
        set_stage(ST_WRITE, total);
        DWORD n = (DWORD)(total < HEAD ? total : HEAD);
        memset(head, 0, HEAD);
        if (!ReadFile(img, head, n, &got, nullptr) || got != n) { ok = false; set_error(T->errRead, GetLastError()); }
        else sha.add(head, n);
        add_progress(n);
        ULONGLONG pos = n, unflushed = 0;
        while (ok && pos < total) {
            if (g_cancel) { ok = false; set_error(T->errCanceled, 0); break; }
            DWORD want = (DWORD)(total - pos < CHUNK ? total - pos : CHUNK);
            if (!ReadFile(img, buf, want, &got, nullptr) || got != want) { ok = false; set_error(T->errRead, GetLastError()); break; }
            sha.add(buf, want);
            DWORD padded = (want + sector - 1) / sector * sector;
            if (padded > want) memset(buf + want, 0, padded - want);
            off.QuadPart = pos;
            SetFilePointerEx(disk, off, nullptr, FILE_BEGIN);
            if (!WriteFile(disk, buf, padded, &got, nullptr) || got != padded) {
                ok = false;
                set_error(T->errWrite, GetLastError());
                break;
            }
            pos += want;
            // Where unbuffered writes are not honoured (Arctic: the page cache
            // takes them), push every 32 MB to the drive, so progress and speed
            // show the real write and no long flush waits at the end.
            unflushed += want;
            if (unflushed >= (32u << 20)) {
                FlushFileBuffers(disk);
                add_progress(unflushed);
                unflushed = 0;
            }
        }
        add_progress(unflushed);
        if (ok) {
            off.QuadPart = 0;
            SetFilePointerEx(disk, off, nullptr, FILE_BEGIN);
            DWORD hn = (n + sector - 1) / sector * sector;
            if (!WriteFile(disk, head, hn, &got, nullptr) || got != hn) { ok = false; set_error(T->errWrite, GetLastError()); }
        }
        FlushFileBuffers(disk);
    }
    char written[65] = "";
    if (ok) sha.hex(written);

    // verify: read the drive back
    if (ok) {
        set_stage(ST_VERIFY, total);
        Sha256 back;
        back.init();
        ULONGLONG pos = 0;
        while (pos < total) {
            if (g_cancel) { ok = false; set_error(T->errCanceled, 0); break; }
            DWORD want = (DWORD)(total - pos < CHUNK ? total - pos : CHUNK);
            DWORD padded = (want + sector - 1) / sector * sector;
            off.QuadPart = pos;
            SetFilePointerEx(disk, off, nullptr, FILE_BEGIN);
            if (!ReadFile(disk, buf, padded, &got, nullptr) || got < want) { ok = false; set_error(T->errVerify, 0); break; }
            back.add(buf, want);
            pos += want;
            add_progress(want);
        }
        if (ok) {
            char hex[65];
            back.hex(hex);
            bool expected = !*g_img.sha || _stricmp(written, g_img.sha) == 0;
            if (!expected) { ok = false; set_error(T->errHash, 0); }
            else if (strcmp(hex, written) != 0) { ok = false; set_error(T->errVerify, 0); }
        }
    }

    DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
    for (int i = 0; i < lk.n; i++) CloseHandle(lk.h[i]);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    if (head) VirtualFree(head, 0, MEM_RELEASE);
    CloseHandle(img);
    CloseHandle(disk);
    return ok;
}

static DWORD WINAPI job_thread(void *p) {
    Drive drv = *(Drive *)p;
    delete (Drive *)p;
    bool ok = true;
    if (!g_img.local && !g_img.cached) ok = download_image();
    if (ok) ok = write_image(drv);
    if (ok && !g_img.local) {  // written and verified: the downloaded image is not needed any more
        DeleteFileW(g_img.path);
        g_img.cached = false;
    }
    if (ok) set_stage(ST_DONE, 0);
    InterlockedExchange(&g_busy, 0);
    PostMessageW(g_hwnd, WM_APP_FINISHED, ok, 0);
    return 0;
}

// ---------------------------------------------------------------- UI: theme
struct Theme {
    D2D1_COLOR_F bg, card, cardStroke, text, text2, accent, onAccent, button, buttonHover, buttonStroke,
        rowHover, rowSel, radio, caution, success, critical, track;
};
static Theme TH;
static bool g_dark;

static D2D1_COLOR_F rgb(UINT c, float a = 1) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a);
}

static UINT accent_from_system(bool dark) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &v, &sz) == ERROR_SUCCESS && v) {
        UINT r = v & 255, g = (v >> 8) & 255, b = (v >> 16) & 255;  // ABGR
        if (dark) {  // the light variant of the accent, as Windows 11 uses in dark mode
            r = r + (255 - r) * 55 / 100; g = g + (255 - g) * 55 / 100; b = b + (255 - b) * 55 / 100;
        }
        return (r << 16) | (g << 8) | b;
    }
    return dark ? 0x60CDFF : 0x005FB8;
}

static void load_theme() {
    DWORD light = 1, sz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz);
    g_dark = light == 0;
    if (g_dark)
        TH = {rgb(0x202020), rgb(0x2B2B2B), rgb(0x1D1D1D), rgb(0xFFFFFF), rgb(0xFFFFFF, 0.79f),
              rgb(accent_from_system(true)), rgb(0x000000), rgb(0x2D2D2D), rgb(0x323232), rgb(0x3A3A3A),
              rgb(0xFFFFFF, 0.06f), rgb(0xFFFFFF, 0.09f), rgb(0xFFFFFF, 0.6f), rgb(0xFCE100), rgb(0x6CCB5F),
              rgb(0xFF99A4), rgb(0xFFFFFF, 0.54f)};
    else
        TH = {rgb(0xF3F3F3), rgb(0xFFFFFF), rgb(0xE5E5E5), rgb(0x1B1B1B), rgb(0x000000, 0.62f),
              rgb(accent_from_system(false)), rgb(0xFFFFFF), rgb(0xFFFFFF), rgb(0xF9F9F9), rgb(0xE0E0E0),
              rgb(0x000000, 0.04f), rgb(0x000000, 0.06f), rgb(0x000000, 0.55f), rgb(0x9D5D00), rgb(0x0F7B0F),
              rgb(0xC42B1C), rgb(0x000000, 0.45f)};
    BOOL d = g_dark;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof d);
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
}

// ---------------------------------------------------------------- UI: Direct2D
static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static ID2D1HwndRenderTarget *g_rt;
static ID2D1SolidColorBrush *g_br;
static IDWriteTextFormat *fHeading, *fSection, *fBody, *fSmall, *fButton;
static float g_dpi = 96;

static const wchar_t *pick_font() {
    static const wchar_t *cands[] = {L"Segoe UI Variable Text", L"Segoe UI", L"Noto Sans", L"Tahoma", L"Arial"};
    IDWriteFontCollection *fc = nullptr;
    g_dw->GetSystemFontCollection(&fc, FALSE);
    const wchar_t *pick = L"Segoe UI";
    for (const wchar_t *c : cands) {
        UINT32 i;
        BOOL ex = FALSE;
        if (fc && SUCCEEDED(fc->FindFamilyName(c, &i, &ex)) && ex) { pick = c; break; }
    }
    if (fc) fc->Release();
    return pick;
}

static IDWriteTextFormat *mkfmt(const wchar_t *fam, float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat *f = nullptr;
    g_dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                           g_uk ? L"uk-UA" : L"en-US", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject *ell = nullptr;
    g_dw->CreateEllipsisTrimmingSign(f, &ell);
    f->SetTrimming(&tr, ell);
    if (ell) ell->Release();
    return f;
}

static void create_resources() {
    const wchar_t *fam = pick_font();
    const wchar_t *disp = wcscmp(fam, L"Segoe UI Variable Text") == 0 ? L"Segoe UI Variable Display" : fam;
    fHeading = mkfmt(disp, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fSection = mkfmt(fam, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fBody = mkfmt(fam, 14, DWRITE_FONT_WEIGHT_NORMAL);
    fSmall = mkfmt(fam, 12, DWRITE_FONT_WEIGHT_NORMAL);
    fButton = mkfmt(fam, 14, DWRITE_FONT_WEIGHT_NORMAL);
    fButton->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

static bool ensure_target() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
    p.dpiX = p.dpiY = g_dpi;
    if (FAILED(g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
        return false;
    g_rt->CreateSolidColorBrush(TH.text, &g_br);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

// Shapes are drawn into a layer at 4x the window's pixels and brought down in
// two exact halvings with linear filtering (each one a 2x2 average, together a
// 4x4 box filter): every edge is antialiased, also where Direct2D itself does
// not antialias geometry (Wine's d2d1, which Arctic uses). Text is drawn
// afterwards straight into the window, so it stays crisp.
static ID2D1BitmapRenderTarget *g_ss4, *g_ss2;
static ID2D1RenderTarget *g_cur;  // where the current pass draws
static int g_pass;                // 0: shapes (into the 4x layer), 1: text (into the window)

static void drop_layers() {
    if (g_ss4) g_ss4->Release(), g_ss4 = nullptr;
    if (g_ss2) g_ss2->Release(), g_ss2 = nullptr;
}

static void drop_target() {
    drop_layers();
    if (g_br) g_br->Release(), g_br = nullptr;
    if (g_rt) g_rt->Release(), g_rt = nullptr;
}

static void fill_rr(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    if (g_pass) return;
    g_br->SetColor(c);
    g_cur->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br);
}
static void stroke_rr(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) {
    if (g_pass) return;
    g_br->SetColor(c);
    D2D1_RECT_F i = {r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2};
    g_cur->DrawRoundedRectangle(D2D1::RoundedRect(i, rad, rad), g_br, w);
}
static void fill_circle(float cx, float cy, float r, D2D1_COLOR_F c) {
    if (g_pass) return;
    g_br->SetColor(c);
    g_cur->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br);
}
static void stroke_circle(float cx, float cy, float r, D2D1_COLOR_F c, float w) {
    if (g_pass) return;
    g_br->SetColor(c);
    g_cur->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, w);
}
static void text(const wchar_t *s, D2D1_RECT_F r, IDWriteTextFormat *f, D2D1_COLOR_F c,
                 DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (!g_pass) return;
    g_br->SetColor(c);
    f->SetTextAlignment(a);
    g_cur->DrawText(s, (UINT32)wcslen(s), f, r, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) {
    if (g_pass) return;
    g_br->SetColor(c);
    ID2D1StrokeStyle *ss = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &ss);
    g_cur->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_br, w, ss);
    if (ss) ss->Release();
}

// ---- icons: Fluent UI System Icons (Microsoft, MIT licence; the set Windows 11
// uses), 20x20 regular, embedded as their SVG paths and filled by Direct2D,
// so they are as smooth as in Windows at any DPI and need no icon font.
static const char *IC_REFRESH =  // arrow_clockwise_20_regular
    "M4 10C4 6.68629 6.68629 4 10 4C11.7766 4 13.3732 4.77191 14.4723 6H12.5C12.2239 6 12 6.22386 12 6.5C12 "
    "6.77614 12.2239 7 12.5 7H15.5C15.7761 7 16 6.77614 16 6.5V3.5C16 3.22386 15.7761 3 15.5 3C15.2239 3 15 "
    "3.22386 15 3.5V5.10109C13.7299 3.80499 11.9591 3 10 3C6.13401 3 3 6.13401 3 10C3 13.866 6.13401 17 10 "
    "17C13.866 17 17 13.866 17 10C17 9.8191 16.9931 9.6397 16.9796 9.46207C16.9587 9.18673 16.7185 8.98049 "
    "16.4431 9.00144C16.1678 9.02239 15.9615 9.26258 15.9825 9.53793C15.9941 9.69034 16 9.84443 16 10C16 "
    "13.3137 13.3137 16 10 16C6.68629 16 4 13.3137 4 10Z";
static const char *IC_USB =  // usb_stick_20_regular
    "M7.5 2H12.5C12.7761 2 13 2.22386 13 2.5V6C14.1046 6 15 6.89543 15 8V16C15 17.1046 14.1046 18 13 18H7C5.89543 "
    "18 5 17.1046 5 16V8C5 6.89543 5.89543 6 7 6V2.5C7 2.22386 7.22386 2 7.5 2ZM12 6V3H8V6H12ZM7 7C6.44772 7 6 "
    "7.44772 6 8V16C6 16.5523 6.44772 17 7 17H13C13.5523 17 14 16.5523 14 16V8C14 7.44772 13.5523 7 13 7H7Z";
static const char *IC_FOLDER =  // folder_open_20_regular
    "M3.00026 5.5V12.1041L4.50091 9.50488C5.03681 8.57668 6.02719 8.00488 7.09898 8.00488L15.0003 "
    "8.00488V7.5C15.0003 6.67157 14.3287 6 13.5003 6H9.50026C9.36765 6 9.24047 5.94732 9.1467 5.85355L7.4396 "
    "4.14645C7.34583 4.05268 7.21865 4 7.08604 4H4.50026C3.67183 4 3.00026 4.67157 3.00026 5.5ZM4.28361 "
    "15.9845C4.35498 15.9947 4.42844 16 4.50373 16H13.9012C14.6157 16 15.276 15.6188 15.6332 15L17.7955 "
    "11.2549C18.3728 10.2549 17.6511 9.00488 16.4964 9.00488L7.09898 9.00488C6.38445 9.00488 5.7242 9.38608 "
    "5.36694 10.0049L3.20469 13.75C2.78223 14.4817 3.0553 15.3473 3.68462 15.7591C3.86197 15.8742 4.06517 "
    "15.9529 4.28361 15.9845ZM2.00026 14.4607V5.5C2.00026 4.11929 3.11955 3 4.50026 3H7.08604C7.48387 3 7.8654 "
    "3.15804 8.1467 3.43934L9.70736 5H13.5003C14.881 5 16.0003 6.11929 16.0003 7.5V8.00488H16.4964C18.4209 "
    "8.00488 19.6237 10.0882 18.6615 11.7549L16.4992 15.5C15.9633 16.4282 14.973 17 13.9012 17H4.50026C4.4378 17 "
    "4.37588 16.9977 4.31457 16.9932C3.75344 16.9527 3.26124 16.7329 2.87665 16.4011C2.36561 15.9642 2.03283 "
    "15.3249 2.00252 14.6073C2.00031 14.5586 1.99955 14.5097 2.00026 14.4607Z";
// a disc in the same style (Fluent has none): 1 px rings at the same weight
static const char *IC_DISC =
    "M10 2C14.4183 2 18 5.58172 18 10C18 14.4183 14.4183 18 10 18C5.58172 18 2 14.4183 2 10C2 5.58172 5.58172 2 "
    "10 2ZM10 3C6.13401 3 3 6.13401 3 10C3 13.866 6.13401 17 10 17C13.866 17 17 13.866 17 10C17 6.13401 13.866 3 "
    "10 3ZM10 7.5C11.3807 7.5 12.5 8.61929 12.5 10C12.5 11.3807 11.3807 12.5 10 12.5C8.61929 12.5 7.5 11.3807 7.5 "
    "10C7.5 8.61929 8.61929 7.5 10 7.5ZM10 8.5C9.17157 8.5 8.5 9.17157 8.5 10C8.5 10.8284 9.17157 11.5 10 "
    "11.5C10.8284 11.5 11.5 10.8284 11.5 10C11.5 9.17157 10.8284 8.5 10 8.5Z";

// SVG path (M L H V C Z, absolute or relative) -> Direct2D geometry
static ID2D1PathGeometry *svg_path(const char *d) {
    ID2D1PathGeometry *g = nullptr;
    ID2D1GeometrySink *s = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return g;
    s->SetFillMode(D2D1_FILL_MODE_WINDING);
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;
    char cmd = 0;
    const char *p = d;
    auto num = [&](float &v) {
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        char *e;
        v = strtof(p, &e);
        bool ok = e != p;
        p = e;
        return ok;
    };
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
        bool rel = cmd >= 'a';
        float a, b, c2, d2, e2, f2;
        switch (cmd | 0x20) {
        case 'm':
            if (!num(a) || !num(b)) goto done;
            if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
            cx = rel ? cx + a : a; cy = rel ? cy + b : b;
            sx = cx; sy = cy;
            s->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            cmd = rel ? 'l' : 'L';
            break;
        case 'l':
            if (!num(a) || !num(b)) goto done;
            cx = rel ? cx + a : a; cy = rel ? cy + b : b;
            s->AddLine(D2D1::Point2F(cx, cy));
            break;
        case 'h':
            if (!num(a)) goto done;
            cx = rel ? cx + a : a;
            s->AddLine(D2D1::Point2F(cx, cy));
            break;
        case 'v':
            if (!num(a)) goto done;
            cy = rel ? cy + a : a;
            s->AddLine(D2D1::Point2F(cx, cy));
            break;
        case 'c':
            if (!num(a) || !num(b) || !num(c2) || !num(d2) || !num(e2) || !num(f2)) goto done;
            if (rel) { a += cx; b += cy; c2 += cx; d2 += cy; e2 += cx; f2 += cy; }
            s->AddBezier(D2D1::BezierSegment(D2D1::Point2F(a, b), D2D1::Point2F(c2, d2), D2D1::Point2F(e2, f2)));
            cx = e2; cy = f2;
            break;
        case 'z':
            if (open) s->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            cx = sx; cy = sy;
            break;
        default:
            goto done;
        }
    }
done:
    if (open) s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}

static ID2D1PathGeometry *gRefresh, *gUsb, *gFolder, *gDisc;

// a 20x20 icon at (x, y) top-left, `size` DIPs
static void icon(ID2D1PathGeometry *g, float x, float y, float size, D2D1_COLOR_F c) {
    if (!g || g_pass) return;
    D2D1_MATRIX_3X2_F old;
    g_cur->GetTransform(&old);
    g_cur->SetTransform(D2D1::Matrix3x2F::Scale(size / 20, size / 20) * D2D1::Matrix3x2F::Translation(x, y) * old);
    g_br->SetColor(c);
    g_cur->FillGeometry(g, g_br);
    g_cur->SetTransform(old);
}

static void icon_flake(float cx, float cy, float s) {  // site/assets/favicon.svg
    D2D1_COLOR_F c = rgb(0x38BDF8);
    float k = s / 32, ox = cx - 16 * k, oy = cy - 16 * k;
    auto L = [&](float a, float b, float x, float y, float w) { line(ox + a * k, oy + b * k, ox + x * k, oy + y * k, c, w * k); };
    L(16, 3, 16, 29, 2.6f); L(4.74f, 9.5f, 27.26f, 22.5f, 2.6f); L(27.26f, 9.5f, 4.74f, 22.5f, 2.6f);
    float v[6][6] = {{12.5f, 6.5f, 16, 10, 19.5f, 6.5f}, {12.5f, 25.5f, 16, 22, 19.5f, 25.5f},
                     {6.3f, 13.4f, 10.8f, 12.6f, 9.4f, 8.2f}, {22.6f, 23.8f, 21.2f, 19.4f, 25.7f, 18.6f},
                     {25.7f, 13.4f, 21.2f, 12.6f, 22.6f, 8.2f}, {9.4f, 23.8f, 10.8f, 19.4f, 6.3f, 18.6f}};
    for (auto &q : v) { L(q[0], q[1], q[2], q[3], 2); L(q[2], q[3], q[4], q[5], 2); }
}

// ---------------------------------------------------------------- UI: layout + paint
enum Hit { H_NONE, H_DRIVE0 = 100, H_REFRESH = 1, H_OTHER, H_RETRY, H_PRIMARY };
static D2D1_RECT_F rDrive[16], rRefresh, rOther, rRetry, rPrimary;
static int g_hover = H_NONE, g_down = H_NONE;
static const float W_DIP = 560, H_DIP = 600, PAD = 24;

static bool in(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static void button(D2D1_RECT_F r, const wchar_t *label, bool accent, int id, bool enabled = true,
                   ID2D1PathGeometry *glyph = nullptr) {
    bool hov = enabled && g_hover == id, down = enabled && g_down == id;
    if (accent) {
        D2D1_COLOR_F c = TH.accent;
        if (!enabled) c = g_dark ? rgb(0xFFFFFF, 0.16f) : rgb(0x000000, 0.22f);
        else if (down) c.a = 0.8f;
        else if (hov) c.a = 0.9f;
        fill_rr(r, 4, c);
        text(label, r, fButton, enabled ? TH.onAccent : (g_dark ? rgb(0xFFFFFF, 0.5f) : rgb(0xFFFFFF)),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        fill_rr(r, 4, hov ? TH.buttonHover : TH.button);
        stroke_rr(r, 4, TH.buttonStroke);
        D2D1_COLOR_F tc = TH.text;
        if (down || !enabled) tc = TH.text2;
        if (glyph) {  // icon + label, centred together
            IDWriteTextLayout *tl = nullptr;
            g_dw->CreateTextLayout(label, (UINT32)wcslen(label), fBody, 400, 32, &tl);
            DWRITE_TEXT_METRICS m = {};
            if (tl) tl->GetMetrics(&m), tl->Release();
            float w = 16 + 8 + m.width, x = (r.left + r.right - w) / 2, cy = (r.top + r.bottom) / 2;
            icon(glyph, x, cy - 8, 16, tc);
            text(label, {x + 24, r.top, r.right, r.bottom}, fBody, tc);
        } else
            text(label, r, fButton, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void draw_ui() {
    float x0 = PAD, x1 = W_DIP - PAD, y = PAD;
    bool busy = g_busy != 0;

    // heading
    icon_flake(x0 + 14, y + 14, 28);
    text(T->heading, {x0 + 40, y, x1, y + 28}, fHeading, TH.text);
    y += 28 + 24;

    // drives
    text(T->drive, {x0, y, x1, y + 20}, fSection, TH.text);
    float rw = 0;
    {
        IDWriteTextLayout *tl = nullptr;
        g_dw->CreateTextLayout(T->refresh, (UINT32)wcslen(T->refresh), fBody, 300, 20, &tl);
        DWRITE_TEXT_METRICS m;
        tl->GetMetrics(&m);
        rw = m.width;
        tl->Release();
    }
    rRefresh = {x1 - rw - 40, y - 6, x1, y + 26};
    if (!busy && g_hover == H_REFRESH) fill_rr(rRefresh, 4, TH.rowHover);
    icon(gRefresh, rRefresh.left + 8, y + 2, 16, busy ? TH.text2 : TH.text);
    text(T->refresh, {rRefresh.left + 32, y, x1, y + 20}, fBody, busy ? TH.text2 : TH.text);
    y += 20 + 8;

    int rows = g_ndrives ? (g_ndrives < 4 ? g_ndrives : 4) : 1;
    float rowH = 56;
    D2D1_RECT_F card = {x0, y, x1, y + rows * rowH + 8};
    fill_rr(card, 8, TH.card);
    stroke_rr(card, 8, TH.cardStroke);
    if (!g_ndrives) {
        icon(gUsb, x0 + 18, y + 4 + rowH / 2 - 10, 20, TH.text2);
        text(T->noDrive, {x0 + 52, y + 4, x1 - 16, y + 4 + rowH}, fBody, TH.text2);
    }
    for (int i = 0; i < g_ndrives && i < 4; i++) {
        D2D1_RECT_F r = {x0 + 4, y + 4 + i * rowH, x1 - 4, y + 4 + (i + 1) * rowH};
        rDrive[i] = r;
        bool sel = i == g_sel;
        if (sel) fill_rr(r, 4, TH.rowSel);
        else if (!busy && g_hover == H_DRIVE0 + i) fill_rr(r, 4, TH.rowHover);
        // radio, Windows 11 proportions: a 20 px ring, a 12 px dot when checked
        float cx = r.left + 22, cy = (r.top + r.bottom) / 2;
        bool hov = !busy && g_hover == H_DRIVE0 + i;
        if (sel) {
            fill_circle(cx, cy, 10, TH.accent);
            fill_circle(cx, cy, hov ? 7 : 6, TH.onAccent);
        } else {
            fill_circle(cx, cy, 10, g_dark ? rgb(0x000000, 0.10f) : rgb(0x000000, 0.024f));
            stroke_circle(cx, cy, 9.5f, TH.radio, 1);
        }
        icon(gUsb, r.left + 46, cy - 10, 20, TH.text);
        wchar_t sz[32], sub[96];
        fmt_size(sz, 32, g_drives[i].size);
        swprintf(sub, 96, L"%s", *g_drives[i].letters ? g_drives[i].letters : T->noLetter);
        text(g_drives[i].name, {r.left + 72, r.top + 8, r.right - 110, r.top + 28}, fBody, TH.text);
        text(sub, {r.left + 72, r.top + 28, r.right - 110, r.top + 46}, fSmall, TH.text2);
        text(sz, {r.right - 110, r.top, r.right - 14, r.bottom}, fBody, TH.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y = card.bottom + 24;

    // image
    text(T->image, {x0, y, x1, y + 20}, fSection, TH.text);
    y += 20 + 8;
    D2D1_RECT_F ic = {x0, y, x1, y + 68};
    fill_rr(ic, 8, TH.card);
    stroke_rr(ic, 8, TH.cardStroke);
    icon(gDisc, x0 + 18, y + 24, 20, TH.text);
    float bw = 150;
    rOther = {x1 - 16 - bw, y + 18, x1 - 16, y + 50};
    rRetry = {};
    wchar_t l1[160] = L"", l2[160] = L"";
    D2D1_COLOR_F c2 = TH.text2;
    if (g_img.local) {
        const wchar_t *fn = wcsrchr(g_img.path, L'\\');
        wcscpy(l1, fn ? fn + 1 : g_img.path);
        wchar_t sz[32];
        fmt_size(sz, 32, g_img.size);
        swprintf(l2, 160, L"%s · %s", T->localFile, sz);
    } else if (g_img.ready) {
        swprintf(l1, 160, L"%s%s%s", g_img.name, *g_img.date ? L" · " : L"", g_img.date);
        wchar_t sz[32];
        fmt_size(sz, 32, g_img.size);
        swprintf(l2, 160, g_img.cached ? L"%s · %s" : L"%s", sz, T->cached);
    } else if (g_img.failed) {
        wcscpy(l1, T->imgFailed);
        c2 = TH.critical;
        rRetry = {rOther.left - 8 - 100, y + 18, rOther.left - 8, y + 50};
    } else
        wcscpy(l1, T->imgLoading);
    float textR = (rRetry.right > 0 ? rRetry.left : rOther.left) - 12;
    if (*l2) {
        text(l1, {x0 + 52, y + 13, textR, y + 34}, fBody, TH.text);
        text(l2, {x0 + 52, y + 34, textR, y + 54}, fSmall, c2);
    } else
        text(l1, {x0 + 52, y, textR, y + 68}, fBody, g_img.failed ? TH.critical : TH.text2);
    if (rRetry.right > 0) button(rRetry, T->retry, false, H_RETRY, !busy);
    button(rOther, T->otherFile, false, H_OTHER, !busy, gFolder);

    // bottom: status line, progress, buttons
    float by = H_DIP - PAD - 32;
    rPrimary = {x1 - 132, by, x1, by + 32};
    EnterCriticalSection(&g_prog.cs);
    Progress pr = g_prog;
    LeaveCriticalSection(&g_prog.cs);
    if (busy && pr.stage >= ST_DOWNLOAD && pr.stage <= ST_VERIFY) {
        const wchar_t *names[] = {L"", T->stDownload, T->stFormat, T->stWrite, T->stVerify};
        double f = pr.total ? (double)pr.done / pr.total : 0;
        if (f > 1) f = 1;
        wchar_t st[160];
        int pct = (int)(f * 100);
        if (pr.stage != ST_FORMAT && pr.speed > 0) {
            double left = (pr.total - pr.done) / pr.speed;
            wchar_t eta[32];
            if (left >= 60) swprintf(eta, 32, L"%d %s", (int)(left / 60 + 0.5), T->minutes);
            else swprintf(eta, 32, L"%d %s", (int)left + 1, T->seconds);
            swprintf(st, 160, L"%s · %d %% · %.1f %s · %s %s", names[pr.stage], pct, pr.speed / 1048576, T->mbs, T->left, eta);
            if (g_uk) for (wchar_t *p = st; *p; p++) if (*p == L'.' ) *p = L',';
        } else
            swprintf(st, 160, L"%s · %d %%", names[pr.stage], pct);
        text(st, {x0, by - 40, x1, by - 20}, fBody, TH.text);
        // Windows 11 progress bar: a thin track, a thicker rounded fill
        float ty = by - 10;
        line(x0, ty, x1, ty, TH.track, 1);
        if (f > 0) fill_rr({x0, ty - 1.5f, x0 + (float)(x1 - x0) * (float)f, ty + 1.5f}, 1.5f, TH.accent);
        button(rPrimary, T->cancel, false, H_PRIMARY);
    } else {
        if (pr.stage == ST_DONE)
            text(T->done, {x0, by, rPrimary.left - 12, by + 32}, fBody, TH.success);
        else if (pr.stage == ST_ERROR)
            text(pr.error, {x0, by, rPrimary.left - 12, by + 32}, fBody, TH.critical);
        else if (g_sel >= 0) {
            wchar_t w[200];
            swprintf(w, 200, T->warn, g_drives[g_sel].name);
            text(w, {x0, by, rPrimary.left - 12, by + 32}, fBody, TH.caution);
        }
        bool can = g_sel >= 0 && (g_img.ready || g_img.local);
        button(rPrimary, T->write, true, H_PRIMARY, can);
    }
}

static void paint() {
    if (!ensure_target()) return;
    D2D1_SIZE_F dip = g_rt->GetSize();
    D2D1_SIZE_U px = g_rt->GetPixelSize();
    if (!g_ss4) {
        g_rt->CreateCompatibleRenderTarget(dip, D2D1::SizeU(px.width * 4, px.height * 4), &g_ss4);
        g_rt->CreateCompatibleRenderTarget(dip, D2D1::SizeU(px.width * 2, px.height * 2), &g_ss2);
    }
    D2D1_RECT_F all = {0, 0, dip.width, dip.height};
    bool layered = g_ss4 && g_ss2;
    if (layered) {
        g_cur = g_ss4;  // shapes at 4x
        g_pass = 0;
        g_ss4->BeginDraw();
        g_ss4->Clear(TH.bg);
        draw_ui();
        g_ss4->EndDraw();
        ID2D1Bitmap *b = nullptr;
        g_ss4->GetBitmap(&b);
        g_ss2->BeginDraw();  // 4x -> 2x
        g_ss2->DrawBitmap(b, all, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g_ss2->EndDraw();
        b->Release();
        g_ss2->GetBitmap(&b);
        g_rt->BeginDraw();  // 2x -> 1x
        g_rt->DrawBitmap(b, all, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        b->Release();
    } else {  // no memory for the layers: plain drawing
        g_cur = g_rt;
        g_pass = 0;
        g_rt->BeginDraw();
        g_rt->Clear(TH.bg);
        draw_ui();
    }
    g_cur = g_rt;  // text, crisp, on top
    g_pass = 1;
    draw_ui();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) drop_target();
}

static int hit_test(float x, float y) {
    if (in(rPrimary, x, y)) return H_PRIMARY;
    if (g_busy) return H_NONE;
    if (in(rRefresh, x, y)) return H_REFRESH;
    if (in(rOther, x, y)) return H_OTHER;
    if (rRetry.right > 0 && in(rRetry, x, y)) return H_RETRY;
    for (int i = 0; i < g_ndrives && i < 4; i++)
        if (in(rDrive[i], x, y)) return H_DRIVE0 + i;
    return H_NONE;
}

// ---------------------------------------------------------------- actions
static void start_latest() {
    if (g_img.local) return;
    memset(&g_img, 0, sizeof g_img);
    CloseHandle(CreateThread(nullptr, 0, latest_thread, nullptr, 0, nullptr));
}

static void pick_file() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"ISO\0*.iso;*.img\0\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&of)) return;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return;
    memset(&g_img, 0, sizeof g_img);
    g_img.local = true;
    wcscpy(g_img.path, path);
    g_img.size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool confirm_erase() {
    const Drive &d = g_drives[g_sel];
    wchar_t main[200], content[300], sz[32], what[160];
    fmt_size(sz, 32, d.size);
    swprintf(what, 160, L"%s, %s", d.name, sz);
    swprintf(main, 200, T->confirmMain, what);
    swprintf(content, 300, T->confirmContent, *d.letters ? d.letters : T->noLetter);
    TASKDIALOG_BUTTON btn[] = {{1001, T->confirmButton}};
    TASKDIALOGCONFIG tc = {sizeof tc};
    tc.hwndParent = g_hwnd;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = main;
    tc.pszContent = content;
    tc.cButtons = 1;
    tc.pButtons = btn;
    tc.nDefaultButton = IDCANCEL;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr)))
        return MessageBoxW(g_hwnd, content, main, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
    return pressed == 1001;
}

static void start_job() {
    if (g_sel < 0 || !(g_img.ready || g_img.local) || g_busy) return;
    if (!confirm_erase()) return;
    InterlockedExchange(&g_busy, 1);
    InterlockedExchange(&g_cancel, 0);
    EnterCriticalSection(&g_prog.cs);
    g_prog.stage = ST_IDLE;
    g_prog.error[0] = 0;
    LeaveCriticalSection(&g_prog.cs);
    SetTimer(g_hwnd, TIMER_TICK, 200, nullptr);
    CloseHandle(CreateThread(nullptr, 0, job_thread, new Drive(g_drives[g_sel]), 0, nullptr));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool ask_stop() {
    return MessageBoxW(g_hwnd, T->stopMain, APP_NAME, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

static void on_click(int id) {
    if (id == H_PRIMARY) {
        if (g_busy) { if (ask_stop()) InterlockedExchange(&g_cancel, 1); }
        else start_job();
    } else if (id == H_REFRESH) {
        enum_drives();
    } else if (id == H_OTHER) {
        pick_file();
    } else if (id == H_RETRY) {
        start_latest();
    } else if (id >= H_DRIVE0 && id < H_DRIVE0 + 16) {
        g_sel = id - H_DRIVE0;
        EnterCriticalSection(&g_prog.cs);
        if (g_prog.stage == ST_DONE || g_prog.stage == ST_ERROR) g_prog.stage = ST_IDLE;
        LeaveCriticalSection(&g_prog.cs);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- window
static void fit_window() {
    RECT r = {0, 0, (LONG)(W_DIP * g_dpi / 96), (LONG)(H_DIP * g_dpi / 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE, 0, (UINT)g_dpi);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        drop_layers();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        drop_target();
        RECT *r = (RECT *)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(l) * 96.f / g_dpi, y = GET_Y_LPARAM(l) * 96.f / g_dpi;
        int hv = hit_test(x, y);
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = H_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, g_hover != H_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(l) * 96.f / g_dpi, y = GET_Y_LPARAM(l) * 96.f / g_dpi;
        g_down = hit_test(x, y);
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x = GET_X_LPARAM(l) * 96.f / g_dpi, y = GET_Y_LPARAM(l) * 96.f / g_dpi;
        int id = hit_test(x, y), was = g_down;
        g_down = H_NONE;
        if (id == was && id != H_NONE) on_click(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (g_busy) break;
        if ((w == VK_DOWN || w == VK_UP) && g_ndrives) {
            int n = g_ndrives < 4 ? g_ndrives : 4;
            g_sel = g_sel < 0 ? 0 : (g_sel + (w == VK_DOWN ? 1 : n - 1)) % n;
            InvalidateRect(h, nullptr, FALSE);
        } else if (w == VK_RETURN)
            start_job();
        else if (w == VK_F5) {
            enum_drives();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_DEVICECHANGE:
        if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE || w == DBT_DEVNODES_CHANGED)
            SetTimer(h, TIMER_REFRESH, 600, nullptr);
        return TRUE;
    case WM_TIMER:
        if (w == TIMER_REFRESH) {
            KillTimer(h, TIMER_REFRESH);
            if (!g_busy) enum_drives();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t *)l, L"ImmersiveColorSet") == 0) {
            load_theme();
            drop_target();
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_APP_LATEST:
    case WM_APP_TICK:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_FINISHED:
        KillTimer(h, TIMER_TICK);
        if (g_closeAfter) { DestroyWindow(h); return 0; }
        enum_drives();
        InvalidateRect(h, nullptr, FALSE);
        if (w) FlashWindow(h, TRUE);
        return 0;
    case WM_CLOSE:
        if (g_busy) {
            if (ask_stop()) { g_closeAfter = true; InterlockedExchange(&g_cancel, 1); }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_uk = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN ||
           PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    T = g_uk ? &UK : &EN;
    InitializeCriticalSection(&g_prog.cs);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    PWSTR la = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) {
        swprintf(g_cacheDir, MAX_PATH, L"%s\\Arctic USB Creator", la);
        CoTaskMemFree(la);
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw))) {
        MessageBoxW(nullptr, L"Direct2D/DirectWrite unavailable", APP_NAME, MB_ICONERROR);
        return 1;
    }
    create_resources();
    gRefresh = svg_path(IC_REFRESH);
    gUsb = svg_path(IC_USB);
    gFolder = svg_path(IC_FOLDER);
    gDisc = svg_path(IC_DISC);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ArcticUsbCreator";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                             CW_USEDEFAULT, CW_USEDEFAULT, 600, 640, nullptr, nullptr, inst, nullptr);
    g_dpi = (float)GetDpiForWindow(g_hwnd);
    load_theme();
    fit_window();
    // centre on the work area
    RECT wr, wa;
    GetWindowRect(g_hwnd, &wr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_hwnd, nullptr, wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2,
                 wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    enum_drives();
    start_latest();
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
