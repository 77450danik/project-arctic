/* What a program that looks for volumes by their GUID names gets (Rufus's
 * GetLogicalName): the root opened as a directory, its device information,
 * the device name and the disk extents.
 *   x86_64-w64-mingw32-gcc -O1 -o voldiag.exe tests/tools/voldiag.c -lntdll */
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <stdio.h>

typedef struct { ULONG DeviceType; ULONG Characteristics; } DEVINFO;

int main(void)
{
    WCHAR name[MAX_PATH], device[MAX_PATH];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);

    if (find == INVALID_HANDLE_VALUE) { printf("FindFirstVolume: %lu\n", GetLastError()); return 1; }
    do
    {
        UNICODE_STRING nt;
        OBJECT_ATTRIBUTES attr;
        IO_STATUS_BLOCK io;
        HANDLE root, vol;
        NTSTATUS status;
        DEVINFO info = { 0 };
        VOLUME_DISK_EXTENTS ext = { 0 };
        DWORD size, len = lstrlenW(name);

        printf("%ls  type %u\n", name, GetDriveTypeW(name));
        RtlInitUnicodeString(&nt, L"");
        if (RtlDosPathNameToNtPathName_U(name, &nt, NULL, NULL))
        {
            InitializeObjectAttributes(&attr, &nt, OBJ_CASE_INSENSITIVE, 0, NULL);
            status = NtOpenFile(&root, SYNCHRONIZE, &attr, &io, 0, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
            printf("  open root %ls: %08lx\n", nt.Buffer, status);
            if (!status)
            {
                status = NtQueryVolumeInformationFile(root, &io, &info, sizeof(info), 4 /* FileFsDeviceInformation */);
                printf("  device info: %08lx type %lx chars %lx\n", status, info.DeviceType, info.Characteristics);
                CloseHandle(root);
            }
        }
        name[len - 1] = 0;
        if (QueryDosDeviceW(name + 4, device, MAX_PATH)) printf("  device %ls\n", device);
        else printf("  QueryDosDevice: %lu\n", GetLastError());
        vol = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (vol == INVALID_HANDLE_VALUE) printf("  open: %lu\n", GetLastError());
        else
        {
            if (DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof(ext), &size, NULL))
                printf("  extents %lu: disk %lu at %llu, %llu bytes\n", ext.NumberOfDiskExtents, ext.Extents[0].DiskNumber,
                       ext.Extents[0].StartingOffset.QuadPart, ext.Extents[0].ExtentLength.QuadPart);
            else printf("  extents: %lu\n", GetLastError());
            CloseHandle(vol);
        }
        name[len - 1] = '\\';
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return 0;
}
