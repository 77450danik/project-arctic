/* How fast the NT calls a program makes most are: the same binary runs on
 * Arctic and on Windows, on the same PC, and the two tables are compared.
 * Every test runs its call for about a second (or a set number of times for
 * slow ones) and gives the median and the 99th percentile of one call in µs.
 *
 * ntbench [name]   name goes into the file: C:\Users\Public\ntbench-<name>.txt
 *                  (default: "arctic" when ntdll exports wine_get_version,
 *                  otherwise "windows")
 *
 * zig cc -target x86_64-windows-gnu -O2 -o out/tests/ntbench.exe tests/tools/ntbench.c -luser32 -ladvapi32 -lgdi32 -lwinmm */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER freq;
static double now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)freq.QuadPart;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

#define MAX_SAMPLES 200000
static double samples[MAX_SAMPLES];
static FILE *out;

static void report(const char *name, int n, double total_us)
{
    qsort(samples, n, sizeof(double), cmp_double);
    char line[256];
    snprintf(line, sizeof(line), "%-34s %10.2f %10.2f %10.2f %8d\r\n", name, samples[n / 2],
             samples[(int)(n * 0.99)], total_us / n, n);
    fputs(line, stdout);
    fflush(stdout);
    if (out)
        fputs(line, out);
}

/* "ntbench name filter seconds": only the tests whose name starts with
 * filter, each for that many seconds (to profile one of them) */
static const char *filter;
static double scale = 1;

/* a request a +server trace shows by name (open_event of a missing event):
 * the requests between "begin" and "end" are the timed calls' own */
static void mark(const char *what)
{
    OpenEventA(SYNCHRONIZE, FALSE, what);
}

/* one call per sample, for about budget_us or max_n calls */
#define BENCH(name, budget_us, max_n, setup, call, teardown)              \
    if (!filter || !strncmp(name, filter, strlen(filter))) do {           \
        int n_ = 0;                                                       \
        mark("ntbench-begin");                                            \
        double start_ = now_us(), sum_ = 0;                               \
        while (n_ < (int)((max_n) * scale < MAX_SAMPLES ? (max_n) * scale : MAX_SAMPLES) && \
               now_us() - start_ < (budget_us) * scale) {                 \
            setup;                                                        \
            double t0_ = now_us();                                        \
            call;                                                         \
            double dt_ = now_us() - t0_;                                  \
            teardown;                                                     \
            samples[n_++] = dt_;                                          \
            sum_ += dt_;                                                  \
        }                                                                 \
        mark("ntbench-end");                                              \
        report(name, n_, sum_);                                           \
    } while (0)

/* --- threads that answer ------------------------------------------------ */

static HANDLE ping, pong;
static volatile LONG stop_flag;

static DWORD WINAPI ponger(void *arg)
{
    for (;;) {
        WaitForSingleObject(ping, INFINITE);
        if (stop_flag)
            return 0;
        SetEvent(pong);
    }
}

static DWORD WINAPI nothing(void *arg)
{
    return 0;
}

static HWND target;
static HANDLE window_ready;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_USER)
        return wp + 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI window_thread(void *arg)
{
    MSG msg;
    target = CreateWindowExW(0, L"ntbench", L"", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    SetEvent(window_ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

/* --- files -------------------------------------------------------------- */

static const char dir[] = "C:\\Users\\Public\\ntbench.tmp";

static void make_files(void)
{
    char path[MAX_PATH];
    CreateDirectoryA(dir, NULL);
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), "%s\\File%04d.txt", dir, i);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w;
        WriteFile(f, path, (DWORD)strlen(path), &w, NULL);
        CloseHandle(f);
    }
}

static void remove_files(void)
{
    char path[MAX_PATH];
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), "%s\\File%04d.txt", dir, i);
        DeleteFileA(path);
    }
    RemoveDirectoryA(dir);
}

typedef NTSTATUS(WINAPI *query_thread_fn)(HANDLE, THREADINFOCLASS, void *, ULONG, ULONG *);

int main(int argc, char **argv)
{
    char self[MAX_PATH], name[64], path[MAX_PATH], cmd[MAX_PATH + 16];
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char *(CDECL *wine_version)(void) = (void *)GetProcAddress(ntdll, "wine_get_version");
    query_thread_fn query_thread = (query_thread_fn)GetProcAddress(ntdll, "NtQueryInformationThread");

    if (argc > 1 && !strcmp(argv[1], "--noop"))
        return 0;
    QueryPerformanceFrequency(&freq);
    GetModuleFileNameA(NULL, self, sizeof(self));
    {
        HKEY key; /* Arctic hides wine_get_version; its runtime has this key */
        int arctic = !RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Arctic\\Runtime", 0, KEY_READ, &key);
        if (arctic)
            RegCloseKey(key);
        snprintf(name, sizeof(name), "%s", argc > 1 ? argv[1] : arctic || wine_version ? "arctic" : "windows");
    }
    if (argc > 2)
        filter = argv[2];
    if (argc > 3)
        scale = atof(argv[3]);
    snprintf(path, sizeof(path), "C:\\Users\\Public\\ntbench-%s.txt", name);
    out = fopen(path, "wb");

    {
        OSVERSIONINFOW v = {sizeof(v)};
        SYSTEM_INFO si;
        char head[512], cpu[128] = "?";
        DWORD size = sizeof(cpu);
        HKEY key;
        LONG (WINAPI *rtl_version)(OSVERSIONINFOW *) = (void *)GetProcAddress(ntdll, "RtlGetVersion");
        rtl_version(&v);
        GetSystemInfo(&si);
        if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key)) {
            RegQueryValueExA(key, "ProcessorNameString", NULL, NULL, (BYTE *)cpu, &size);
            RegCloseKey(key);
        }
        snprintf(head, sizeof(head),
                 "ntbench %s: NT %lu.%lu.%lu%s%s, %lu CPUs, %s\r\n\r\n%-34s %10s %10s %10s %8s\r\n", name,
                 v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, wine_version ? ", runtime " : "",
                 wine_version ? wine_version() : "", si.dwNumberOfProcessors, cpu, "test (us per call)", "median",
                 "p99", "mean", "calls");
        fputs(head, stdout);
        if (out)
            fputs(head, out);
    }

    /* kernel calls with nothing behind them */
    {
        struct { NTSTATUS status; void *teb; CLIENT_ID id; ULONG_PTR affinity; LONG prio, base; } tbi; /* ThreadBasicInformation */
        BENCH("NtQueryInformationThread", 1e6, MAX_SAMPLES, , query_thread(GetCurrentThread(), 0, &tbi, sizeof(tbi), NULL), );
        FILETIME a, b, c, d;
        BENCH("GetProcessTimes", 1e6, MAX_SAMPLES, , GetProcessTimes(GetCurrentProcess(), &a, &b, &c, &d), );
        BENCH("SwitchToThread", 1e6, MAX_SAMPLES, , SwitchToThread(), );
    }

    /* synchronisation (ntsync on Arctic) */
    {
        HANDLE ev = CreateEventW(NULL, TRUE, TRUE, NULL), mx = CreateMutexW(NULL, FALSE, NULL);
        HANDLE sem = CreateSemaphoreW(NULL, 0, 1000000, NULL);
        BENCH("Wait on signaled event", 1e6, MAX_SAMPLES, , WaitForSingleObject(ev, 0), );
        BENCH("SetEvent", 1e6, MAX_SAMPLES, , SetEvent(ev), );
        BENCH("Mutex wait + release", 1e6, MAX_SAMPLES, , (WaitForSingleObject(mx, 0), ReleaseMutex(mx)), );
        BENCH("ReleaseSemaphore", 1e6, MAX_SAMPLES, , ReleaseSemaphore(sem, 1, NULL), );
        HANDLE two[2] = {ev, mx};
        BENCH("WaitForMultipleObjects(2, all)", 1e6, MAX_SAMPLES, , (WaitForMultipleObjects(2, two, TRUE, 0), ReleaseMutex(mx)), );
        HANDLE h;
        BENCH("CreateEvent + CloseHandle", 1e6, MAX_SAMPLES, , h = CreateEventW(NULL, FALSE, FALSE, NULL), CloseHandle(h));
        BENCH("DuplicateHandle + CloseHandle", 1e6, MAX_SAMPLES, ,
              DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS),
              CloseHandle(h));
        CloseHandle(ev);
        CloseHandle(mx);
        CloseHandle(sem);

        ping = CreateEventW(NULL, FALSE, FALSE, NULL);
        pong = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, ponger, NULL, 0, NULL);
        BENCH("Event ping-pong between threads", 1e6, MAX_SAMPLES, , (SetEvent(ping), WaitForSingleObject(pong, INFINITE)), );
        stop_flag = 1;
        SetEvent(ping);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        CloseHandle(ping);
        CloseHandle(pong);

        CRITICAL_SECTION cs;
        InitializeCriticalSection(&cs);
        BENCH("EnterCriticalSection + Leave", 3e5, MAX_SAMPLES, , (EnterCriticalSection(&cs), LeaveCriticalSection(&cs)), );
        DeleteCriticalSection(&cs);

        HANDLE timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        LARGE_INTEGER due = {.QuadPart = -10000}; /* 1 ms */
        timeBeginPeriod(1);
        BENCH("Waitable timer 1 ms (real time)", 1e6, 2000, SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE),
              WaitForSingleObject(timer, INFINITE), );
        BENCH("Sleep(1) (real time)", 1e6, 2000, , Sleep(1), );
        timeEndPeriod(1);
        CloseHandle(timer);
    }

    /* threads and processes */
    {
        HANDLE t;
        BENCH("CreateThread + wait + close", 2e6, 20000, , (t = CreateThread(NULL, 0, nothing, NULL, 0, NULL), WaitForSingleObject(t, INFINITE)),
              CloseHandle(t));
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        snprintf(cmd, sizeof(cmd), "\"%s\" --noop", self);
        BENCH("CreateProcess + wait (console)", 5e6, 200, ,
              (CreateProcessA(self, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi),
               WaitForSingleObject(pi.hProcess, INFINITE)),
              (CloseHandle(pi.hThread), CloseHandle(pi.hProcess)));
    }

    /* memory */
    {
        void *p;
        BENCH("VirtualAlloc+touch+free 64 KB", 1e6, MAX_SAMPLES, ,
              (p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), memset(p, 1, 65536),
               VirtualFree(p, 0, MEM_RELEASE)), );
        DWORD old;
        p = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        BENCH("VirtualProtect", 1e6, MAX_SAMPLES, , VirtualProtect(p, 4096, PAGE_READONLY, &old), VirtualProtect(p, 4096, PAGE_READWRITE, &old));
        VirtualFree(p, 0, MEM_RELEASE);
        HANDLE sec;
        BENCH("CreateFileMapping+Map+Unmap 1 MB", 1e6, MAX_SAMPLES, ,
              (sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, NULL),
               p = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 0), UnmapViewOfFile(p)),
              CloseHandle(sec));
    }

    /* files */
    {
        char file[MAX_PATH], upper[MAX_PATH], missing[MAX_PATH], buf[4096];
        HANDLE f;
        DWORD rw;
        int i = 0;
        make_files();
        snprintf(file, sizeof(file), "%s\\File0500.txt", dir);
        snprintf(upper, sizeof(upper), "%s\\FILE0500.TXT", dir);
        snprintf(missing, sizeof(missing), "%s\\NoSuchFile.txt", dir);
        BENCH("CreateFile(open) + CloseHandle", 1e6, MAX_SAMPLES, ,
              f = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), CloseHandle(f));
        BENCH("GetFileAttributes existing", 1e6, MAX_SAMPLES, , GetFileAttributesA(file), );
        BENCH("GetFileAttributes other case", 1e6, MAX_SAMPLES, , GetFileAttributesA(upper), );
        BENCH("GetFileAttributes missing", 1e6, MAX_SAMPLES, , GetFileAttributesA(missing), );
        CreateDirectoryA("C:\\Users\\Public\\ntbench.small", NULL);
        BENCH("GetFileAttributes missing, empty dir", 1e6, MAX_SAMPLES, ,
              GetFileAttributesA("C:\\Users\\Public\\ntbench.small\\NoSuchFile.txt"), );
        RemoveDirectoryA("C:\\Users\\Public\\ntbench.small");
        HMODULE lib;
        BENCH("LoadLibrary of a missing DLL", 1e6, MAX_SAMPLES, , lib = LoadLibraryA("nosuchlib42.dll"), );
        BENCH("LoadLibrary+Free (not loaded yet)", 1e6, MAX_SAMPLES, , lib = LoadLibraryA("msimg32.dll"), FreeLibrary(lib));
        f = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        BENCH("ReadFile 4 KB (cached)", 1e6, MAX_SAMPLES, SetFilePointer(f, 0, NULL, FILE_BEGIN), ReadFile(f, buf, sizeof(buf), &rw, NULL), );
        BENCH("GetFileSizeEx", 1e6, MAX_SAMPLES, , {LARGE_INTEGER s; GetFileSizeEx(f, &s);}, );
        CloseHandle(f);
        WIN32_FIND_DATAA fd;
        BENCH("FindFirst/Next over 1000 files", 3e6, 2000, , {
            HANDLE fh = FindFirstFileA("C:\\Users\\Public\\ntbench.tmp\\*", &fd);
            while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }, );
        memset(buf, 'x', sizeof(buf));
        BENCH("Create+write 4 KB+close+delete", 3e6, 20000, snprintf(file, sizeof(file), "%s\\new%d.tmp", dir, i++),
              (f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL), WriteFile(f, buf, sizeof(buf), &rw, NULL),
               CloseHandle(f), DeleteFileA(file)), );
        remove_files();
    }

    /* registry */
    {
        HKEY key, k2;
        char value[256];
        DWORD size;
        const char *nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        RegOpenKeyExA(HKEY_LOCAL_MACHINE, nt, 0, KEY_READ, &key);
        BENCH("RegQueryValueEx (open key)", 1e6, MAX_SAMPLES, size = sizeof(value),
              RegQueryValueExA(key, "ProductName", NULL, NULL, (BYTE *)value, &size), );
        BENCH("RegOpenKeyEx + Query + Close", 1e6, MAX_SAMPLES, size = sizeof(value),
              (RegOpenKeyExA(HKEY_LOCAL_MACHINE, nt, 0, KEY_READ, &k2),
               RegQueryValueExA(k2, "ProductName", NULL, NULL, (BYTE *)value, &size)),
              RegCloseKey(k2));
        RegCloseKey(key);
        RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ArcticTest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL);
        DWORD v = 0;
        BENCH("RegSetValueEx DWORD", 1e6, MAX_SAMPLES, v++, RegSetValueExA(key, "Bench", 0, REG_DWORD, (BYTE *)&v, sizeof(v)), );
        RegDeleteValueA(key, "Bench");
        RegCloseKey(key);
    }

    /* windows and messages */
    {
        WNDCLASSW wc = {0};
        MSG msg;
        HWND w;
        wc.lpfnWndProc = wndproc;
        wc.lpszClassName = L"ntbench";
        RegisterClassW(&wc);
        BENCH("CreateWindowEx + DestroyWindow", 2e6, 20000, ,
              (w = CreateWindowExW(0, L"ntbench", L"", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, NULL, NULL), DestroyWindow(w)), );
        w = CreateWindowExW(0, L"ntbench", L"", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
        BENCH("PeekMessage (empty queue)", 1e6, MAX_SAMPLES, , PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE), );
        BENCH("PostMessage + PeekMessage", 1e6, MAX_SAMPLES, ,
              (PostMessageW(w, WM_USER, 0, 0), PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)), );
        BENCH("SendMessage same thread", 1e6, MAX_SAMPLES, , SendMessageW(w, WM_USER, 1, 0), );
        BENCH("GetWindowRect", 1e6, MAX_SAMPLES, , {RECT r; GetWindowRect(w, &r);}, );
        BENCH("SetWindowPos (move)", 1e6, 50000, , SetWindowPos(w, NULL, 1 + (int)(now_us()) % 50, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE), );
        BENCH("GetDC + ReleaseDC", 1e6, MAX_SAMPLES, , {HDC dc = GetDC(w); ReleaseDC(w, dc);}, );
        DestroyWindow(w);

        window_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
        WaitForSingleObject(window_ready, INFINITE);
        BENCH("SendMessage to another thread", 1e6, MAX_SAMPLES, , SendMessageW(target, WM_USER, 1, 0), );
        PostMessageW(target, WM_QUIT, 0, 0);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }

    if (out) {
        fclose(out);
        printf("\r\n-> %s\r\n", path);
    }
    return 0;
}
