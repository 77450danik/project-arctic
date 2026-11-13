/* The drive letters as a program sees them, and what MountedDevices keeps of
 * them (docs/disks.md).
 *   x86_64-w64-mingw32-gcc -O1 -s -o out/tests/letters.exe tests/tools/letters.c */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    DWORD drives = GetLogicalDrives(), index;
    HKEY key;

    for (int i = 0; i < 26; i++)
    {
        WCHAR root[] = L"A:\\", volume[64] = L"", label[64] = L"", fs[16] = L"";

        if (!(drives & (1u << i))) continue;
        root[0] += i;
        GetVolumeNameForVolumeMountPointW(root, volume, 64);
        GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, fs, 16);
        printf("%c: type %u  fs '%ls'  label '%ls'  %ls\n", 'A' + i, GetDriveTypeW(root), fs, label, volume + (volume[0] ? 10 : 0));
    }

    printf("kept in MountedDevices:\n");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\MountedDevices", 0, KEY_QUERY_VALUE, &key)) return 1;
    for (index = 0;; index++)
    {
        WCHAR name[128];
        BYTE data[1024];
        DWORD name_len = ARRAYSIZE(name), size = sizeof(data), type;

        if (RegEnumValueW(key, index, name, &name_len, NULL, &type, data, &size)) break;
        if (wcsncmp(name, L"\\DosDevices\\", 12)) continue;
        if (size == 24 && !memcmp(data, "DMIO:ID:", 8)) printf("  %ls  GPT\n", name + 12);
        else if (size == 12) printf("  %ls  MBR\n", name + 12);
        else if (size > 8 && !data[1]) printf("  %ls  %.46ls\n", name + 12, (WCHAR *)data);
        else printf("  %ls  %.40s\n", name + 12, data);
    }
    RegCloseKey(key);
    return 0;
}
