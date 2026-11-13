/* What Node.js does first in a program started without a console (an Electron
 * program from its shortcut): the standard handles it lacks become NUL. If
 * GetFileType cannot name NUL, _open fails and Node stops with _exit(134),
 * with no window, message or dump.
 *   x86_64-w64-mingw32-gcc -O1 -o nultest.exe tests/tools/nultest.c */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

int main(void)
{
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    DWORD type, done = 0;
    char buffer[16];
    int fd, ok, bad = 0;

    if (nul == INVALID_HANDLE_VALUE)
    {
        printf("CreateFile NUL: error %lu\n", GetLastError());
        return 1;
    }
    SetLastError(0);
    type = GetFileType(nul);
    printf("GetFileType: %lu, error %lu (a character device is 2)\n", type, GetLastError());
    bad |= type != FILE_TYPE_CHAR;
    done = 1;
    ok = ReadFile(nul, buffer, sizeof(buffer), &done, NULL);
    printf("ReadFile: %d, %lu bytes (none expected)\n", ok, done);
    bad |= !ok || done != 0;
    done = 0;
    ok = WriteFile(nul, "hello", 5, &done, NULL);
    printf("WriteFile: %d, %lu bytes (5 expected)\n", ok, done);
    bad |= !ok || done != 5;
    CloseHandle(nul);

    fd = _open("nul", _O_RDWR);
    printf("_open nul: %d\n", fd);
    bad |= fd == -1;
    if (fd != -1) _close(fd);

    printf(bad ? "NUL IS BROKEN\n" : "NUL IS FINE\n");
    return bad;
}
