/* What Safely Remove Hardware sees of each drive (bus, removable medium,
 * product, disk number), then ejects the drive named on the command line.
 *
 *   ejecttest.exe [LETTER] */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

int main( int argc, char **argv )
{
    DWORD drives = GetLogicalDrives();
    char device[] = "\\\\.\\?:";

    for (char c = 'A'; c <= 'Z'; c++)
    {
        STORAGE_PROPERTY_QUERY query = { StorageDeviceProperty, PropertyStandardQuery };
        BYTE buffer[1024] = { 0 };
        STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buffer;
        STORAGE_DEVICE_NUMBER number = { 0 };
        DWORD size;
        HANDLE h;

        if (!(drives & (1 << (c - 'A')))) continue;
        device[4] = c;
        h = CreateFileA( device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
        if (h == INVALID_HANDLE_VALUE)
        {
            printf( "ejecttest: %c: open %lu\n", c, GetLastError() );
            continue;
        }
        BOOL prop = DeviceIoControl( h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                                     sizeof(buffer) - 1, &size, NULL );
        BOOL num = DeviceIoControl( h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &number, sizeof(number),
                                    &size, NULL );
        printf( "ejecttest: %c: type %u bus %d removable %d product '%s' disk %lu (%d %d)\n", c,
                GetDriveTypeA( (char[]){ c, ':', '\\', 0 } ), prop ? desc->BusType : -1,
                prop ? desc->RemovableMedia : -1,
                prop && desc->ProductIdOffset ? (char *)buffer + desc->ProductIdOffset : "",
                number.DeviceNumber, prop, num );
        CloseHandle( h );
    }

    if (argc > 1)
    {
        HANDLE h;
        DWORD size, start = GetTickCount();

        device[4] = argv[1][0];
        h = CreateFileA( device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
        if (h == INVALID_HANDLE_VALUE)
        {
            printf( "ejecttest: open %s for eject: %lu\n", device, GetLastError() );
            return 1;
        }
        if (DeviceIoControl( h, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &size, NULL ))
            printf( "ejecttest: ejected %c: in %lu ms\n", argv[1][0], GetTickCount() - start );
        else
            printf( "ejecttest: eject %c: error %lu\n", argv[1][0], GetLastError() );
        CloseHandle( h );
    }
    return 0;
}
