/*
 * fmifs.dll: formatting volumes, for Rufus, Explorer's "Format..." and the
 * rest (docs/disks.md). The file systems are made here, in Windows code,
 * through the volume's raw handle; there is no format tool on the host.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "wine/debug.h"

#include "format.h"

WINE_DEFAULT_DEBUG_CHANNEL(fmifs);

/* what the callback is told */
enum { FCC_PROGRESS = 0, FCC_DONE = 11 };

typedef BOOLEAN (WINAPI *FMIFS_CALLBACK)( int command, DWORD modifier, void *argument );

struct target
{
    HANDLE         handle;
    FMIFS_CALLBACK callback;
};

static int write_volume( void *context, uint64_t offset, const void *data, uint32_t size )
{
    struct target *target = context;
    LARGE_INTEGER position;
    DWORD written;

    position.QuadPart = offset;
    if (!SetFilePointerEx( target->handle, position, NULL, FILE_BEGIN )) return -1;
    if (!WriteFile( target->handle, data, size, &written, NULL ) || written != size)
    {
        WARN( "writing %u bytes at %s: %lu\n", size, wine_dbgstr_longlong( offset ), GetLastError() );
        return -1;
    }
    return 0;
}

static void report_progress( void *context, unsigned int percent )
{
    struct target *target = context;
    DWORD value = percent;

    if (target->callback) target->callback( FCC_PROGRESS, 0, &value );
}

static uint32_t upcase_char( uint32_t c )
{
    return RtlUpcaseUnicodeChar( (WCHAR)c );
}

/* "\\?\Volume{...}", "\\?\Volume{...}\", "E:", "E:\" or "\\.\E:" */
static HANDLE open_volume( const WCHAR *root )
{
    WCHAR path[MAX_PATH + 8];
    size_t len;

    if (root[0] && root[1] == ':') swprintf( path, ARRAY_SIZE(path), L"\\\\.\\%c:", root[0] );
    else lstrcpynW( path, root, ARRAY_SIZE(path) );
    len = lstrlenW( path );
    if (len && path[len - 1] == '\\') path[len - 1] = 0;
    return CreateFileW( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
}

/* FormatEx: what it finds wrong it says through the callback, as it returns nothing */
VOID WINAPI FormatEx( WCHAR *root, DWORD media, WCHAR *file_system, WCHAR *label, BOOLEAN quick,
                      ULONG cluster, FMIFS_CALLBACK callback )
{
    struct target target = { INVALID_HANDLE_VALUE, callback };
    static const WCHAR no_label[1];
    GET_LENGTH_INFORMATION length;
    DISK_GEOMETRY geometry;
    PARTITION_INFORMATION_EX partition;
    struct volume volume;
    BOOLEAN success = FALSE;
    LARGE_INTEGER now;
    DWORD size;
    int status = -1;

    TRACE( "%s %s label %s quick %u cluster %lu\n", debugstr_w(root), debugstr_w(file_system), debugstr_w(label), quick, cluster );

    target.handle = open_volume( root );
    if (target.handle == INVALID_HANDLE_VALUE)
    {
        WARN( "cannot open %s: %lu\n", debugstr_w(root), GetLastError() );
        goto done;
    }
    /* the file system goes first, and stays away until this handle closes;
     * files still open on it are forced off, as format does once it is told
     * to dismount */
    if ((!DeviceIoControl( target.handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &size, NULL ) &&
         (!DeviceIoControl( target.handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &size, NULL ) ||
          !DeviceIoControl( target.handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &size, NULL ))) ||
        !DeviceIoControl( target.handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &size, NULL ))
    {
        WARN( "cannot lock %s: %lu\n", debugstr_w(root), GetLastError() );
        goto done;
    }
    if (!DeviceIoControl( target.handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &size, NULL ) ||
        !DeviceIoControl( target.handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geometry, sizeof(geometry), &size, NULL ))
    {
        WARN( "no size for %s: %lu\n", debugstr_w(root), GetLastError() );
        goto done;
    }

    memset( &volume, 0, sizeof(volume) );
    volume.context = &target;
    volume.write = write_volume;
    volume.progress = report_progress;
    volume.upcase = upcase_char;
    volume.size = length.Length.QuadPart;
    volume.sector = geometry.BytesPerSector;
    volume.cluster = cluster;
    volume.quick = quick;
    volume.label = label ? label : no_label;
    NtQuerySystemTime( &now );
    volume.nt_time = now.QuadPart;
    volume.serial = (uint32_t)(now.QuadPart >> 16) ^ GetTickCount();
    if (DeviceIoControl( target.handle, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &partition, sizeof(partition), &size, NULL ))
        volume.hidden = partition.StartingOffset.QuadPart / geometry.BytesPerSector;

    if (!wcsicmp( file_system, L"FAT32" )) status = format_fat( &volume, 32 );
    else if (!wcsicmp( file_system, L"FAT" )) status = format_fat( &volume, 0 );
    else if (!wcsicmp( file_system, L"exFAT" )) status = format_exfat( &volume );
    else if (!wcsicmp( file_system, L"NTFS" )) status = format_ntfs( &volume );
    else FIXME( "no %s\n", debugstr_w(file_system) );

    if (!status)
    {
        DWORD percent = 100;

        FlushFileBuffers( target.handle );
        if (callback) callback( FCC_PROGRESS, 0, &percent );
        success = TRUE;
    }

done:
    /* closing the handle gives the volume back to the system, which mounts what is on it now */
    if (target.handle != INVALID_HANDLE_VALUE) CloseHandle( target.handle );
    if (!success) WARN( "%s: failed\n", debugstr_w(root) );
    if (callback) callback( FCC_DONE, 0, &success );
}

/* No checker here: a volume that was just made has nothing to fix, and the
 * others get their check when the host mounts them (docs/persistence.md). */
VOID WINAPI Chkdsk( WCHAR *root, WCHAR *file_system, BOOLEAN correct, BOOLEAN verbose, BOOLEAN only_if_dirty,
                    BOOLEAN scan, void *reserved1, void *reserved2, FMIFS_CALLBACK callback )
{
    BOOLEAN success = TRUE;

    TRACE( "%s %s\n", debugstr_w(root), debugstr_w(file_system) );
    if (callback) callback( FCC_DONE, 0, &success );
}

BOOLEAN WINAPI EnableVolumeCompression( WCHAR *root, USHORT format )
{
    HANDLE handle = CreateFileW( root, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_READ_DATA | FILE_WRITE_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL );
    DWORD size;
    BOOL ret;

    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    ret = DeviceIoControl( handle, FSCTL_SET_COMPRESSION, &format, sizeof(format), NULL, 0, &size, NULL );
    CloseHandle( handle );
    return ret;
}

BOOLEAN WINAPI QueryAvailableFileSystemFormat( DWORD index, WCHAR *name, UCHAR *major, UCHAR *minor, BOOLEAN *latest )
{
    static const struct { const WCHAR *name; UCHAR major, minor; } formats[] =
    {
        { L"NTFS", 3, 1 }, { L"FAT32", 0, 0 }, { L"exFAT", 1, 0 }, { L"FAT", 0, 0 },
    };

    if (index >= ARRAY_SIZE(formats)) return FALSE;
    if (name) lstrcpyW( name, formats[index].name );
    if (major) *major = formats[index].major;
    if (minor) *minor = formats[index].minor;
    if (latest) *latest = TRUE;
    return TRUE;
}
