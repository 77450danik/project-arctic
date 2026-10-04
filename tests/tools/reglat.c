/* How long the system stalls: for some seconds (the first argument, 120 by
 * default) it sets a registry value every 20 ms, which goes through
 * wineserver as every NT call does, and every 2 s writes a small file and
 * flushes it (FlushFileBuffers), as browsers and editors do. A stick that is
 * slow to write shows as long waits: the longest, and how many passed 0.1,
 * 0.5 and 1 s. The result goes to stdout and C:\Users\Public\reglat.txt.
 *
 * x86_64-w64-mingw32-gcc -O2 -o out/tests/reglat.exe tests/tools/reglat.c -ladvapi32 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 120, calls = 0, over[3] = {0}, flushes = 0;
    double reg_max = 0, flush_max = 0, flush_total = 0, start = now_ms(), last_flush = start;
    char line[512], data[4096];
    HKEY key;
    FILE *out;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ArcticTest", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
        return 1;
    memset(data, 'a', sizeof(data));
    while (now_ms() - start < seconds * 1000.0) {
        double t0 = now_ms(), dt;
        DWORD value = calls;

        RegSetValueExA(key, "Latency", 0, REG_DWORD, (BYTE *)&value, sizeof(value));
        dt = now_ms() - t0;
        if (dt > reg_max)
            reg_max = dt;
        over[0] += dt > 100;
        over[1] += dt > 500;
        over[2] += dt > 1000;
        calls++;
        if (t0 - last_flush > 2000) {
            HANDLE file = CreateFileA("C:\\Users\\Public\\reglat.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            DWORD written;
            double f0 = now_ms();

            WriteFile(file, data, sizeof(data), &written, NULL);
            FlushFileBuffers(file);
            CloseHandle(file);
            dt = now_ms() - f0;
            if (dt > flush_max)
                flush_max = dt;
            flush_total += dt;
            flushes++;
            last_flush = t0;
        }
        Sleep(20);
    }
    RegCloseKey(key);
    DeleteFileA("C:\\Users\\Public\\reglat.dat");
    snprintf(line, sizeof(line),
             "%d s: %d registry calls, longest %.0f ms, over 0.1 s: %d, over 0.5 s: %d, over 1 s: %d\r\n"
             "%d flushed writes, longest %.0f ms, average %.0f ms\r\n",
             seconds, calls, reg_max, over[0], over[1], over[2], flushes, flush_max,
             flushes ? flush_total / flushes : 0);
    fputs(line, stdout);
    if ((out = fopen("C:\\Users\\Public\\reglat.txt", "wb"))) {
        fputs(line, out);
        fclose(out);
    }
    return 0;
}
