/* Tries the formatters of fmifs.dll on an image file.
 *   gcc -O1 -g -Iruntime/wine/modules/dlls/fmifs -o format-test tests/tools/format-test.c \
 *       runtime/wine/modules/dlls/fmifs/format_{fat,exfat,ntfs}.c
 *   truncate -s 512M x.img; ./format-test x.img ntfs LABEL [cluster]
 * and look at the result with the tools of the file system (fsck.vfat,
 * fsck.exfat, ntfsinfo, ntfsfix). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wctype.h>

#include "format.h"

static int do_write( void *context, uint64_t offset, const void *data, uint32_t size )
{
    return pwrite( *(int *)context, data, size, offset ) == size ? 0 : -1;
}

static void progress( void *context, unsigned int percent )
{
    (void)context;
    (void)percent;
}

static uint32_t upcase( uint32_t c )
{
    return c < 0x80 ? (uint32_t)towupper( c ) : c >= 0x430 && c <= 0x44f ? c - 0x20 : c;
}

int main( int argc, char **argv )
{
    struct volume v;
    utf16_t label[64] = { 0 };
    struct stat st;
    int fd, i, status;

    if (argc < 4) return 2;
    if ((fd = open( argv[1], O_RDWR )) < 0 || fstat( fd, &st )) return 2;
    for (i = 0; argv[3][i] && i < 32; i++) label[i] = (unsigned char)argv[3][i];
    memset( &v, 0, sizeof(v) );
    v.context = &fd;
    v.write = do_write;
    v.progress = progress;
    v.upcase = upcase;
    v.size = st.st_size;
    v.sector = 512;
    v.serial = 0x12345678;
    v.cluster = argc > 4 ? atoi( argv[4] ) : 0;
    v.quick = 1;
    v.nt_time = 134000000000000000ull;
    v.label = label;

    if (!strcmp( argv[2], "fat32" )) status = format_fat( &v, 32 );
    else if (!strcmp( argv[2], "fat" )) status = format_fat( &v, 0 );
    else if (!strcmp( argv[2], "exfat" )) status = format_exfat( &v );
    else status = format_ntfs( &v );
    printf( "%s: %d\n", argv[2], status );
    close( fd );
    return status ? 1 : 0;
}
