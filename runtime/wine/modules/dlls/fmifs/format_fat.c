/*
 * fmifs.dll: FAT12, FAT16 and FAT32, laid out as Windows' format does.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "format.h"

/* halts where a boot loader would be: the medium is not a boot disk until
 * something writes one (Rufus writes its own) */
static const uint8_t stub[] = { 0xf4, 0xeb, 0xfd };

int fmt_zero( struct volume *v, uint64_t offset, uint64_t size, unsigned int from, unsigned int to )
{
    enum { CHUNK = 1 << 20 };
    uint64_t done = 0;
    uint8_t *zeros = calloc( 1, CHUNK );
    int status = 0;

    if (!zeros) return -1;
    while (done < size && !status)
    {
        uint32_t n = size - done > CHUNK ? CHUNK : (uint32_t)(size - done);

        status = v->write( v->context, offset + done, zeros, n );
        done += n;
        if (v->progress) v->progress( v->context, from + (unsigned int)((to - from) * (done * 100 / size) / 100) );
    }
    free( zeros );
    return status;
}

static void fat_label( const struct volume *v, char out[11] )
{
    int i;

    memset( out, ' ', 11 );
    for (i = 0; i < 11 && v->label[i]; i++)
    {
        uint32_t c = v->upcase( v->label[i] );

        out[i] = c < 0x80 ? (char)c : '_';
    }
}

static void common_bpb( const struct volume *v, uint8_t *b, uint32_t reserved, uint32_t spc, uint32_t root_entries,
                        uint64_t total, uint8_t media )
{
    b[0] = 0xeb;
    b[2] = 0x90;
    memcpy( b + 3, "MSDOS5.0", 8 );
    put16( b + 11, v->sector );
    b[13] = spc;
    put16( b + 14, reserved );
    b[16] = 2;
    put16( b + 17, root_entries );
    if (total < 0x10000) put16( b + 19, (uint16_t)total );
    b[21] = media;
    put16( b + 24, 63 );
    put16( b + 26, 255 );
    put32( b + 28, (uint32_t)v->hidden );
    if (total >= 0x10000) put32( b + 32, (uint32_t)total );
    b[510] = 0x55;
    b[511] = 0xaa;
}

/* sectors per cluster as Windows chooses them for FAT32 */
static uint32_t fat32_spc( uint64_t size, uint32_t sector )
{
    uint32_t cluster = size <= 260ull << 20 ? 512 : size <= 8ull << 30 ? 4096 : size <= 16ull << 30 ? 8192 :
                       size <= 32ull << 30 ? 16384 : 32768;

    return cluster <= sector ? 1 : cluster / sector;
}

static int format_fat32( struct volume *v )
{
    uint32_t sector = v->sector, reserved = 32, spc, fat_sectors = 1;
    uint64_t total = v->size / sector, clusters, data_start;
    char label[11];
    uint8_t *buf;
    int status = -1, i;

    spc = v->cluster ? v->cluster / sector : fat32_spc( v->size, sector );
    if (!is_power_of_2( spc ) || spc > 128) return -1;

    /* FAT32 has 65525 clusters at least: boot loaders and firmware tell the
     * FAT type by the count (Syslinux takes fewer for FAT16). A cluster too
     * large for the volume (Rufus asks for 64 KB on 2 GB) is made smaller
     * until the volume is a FAT32 one. */
    for (;;)
    {
        fat_sectors = 1;
        /* the FAT holds a number for each cluster the rest of the volume can have */
        for (i = 0; i < 8; i++)
        {
            clusters = (total - reserved - 2ull * fat_sectors) / spc;
            fat_sectors = (uint32_t)(((clusters + 2) * 4 + sector - 1) / sector);
        }
        clusters = (total - reserved - 2ull * fat_sectors) / spc;
        if (clusters >= 65525 || spc == 1) break;
        spc /= 2;
    }
    if (clusters < 65525 || clusters >= 0x0ffffff5) return -1;
    data_start = reserved + 2ull * fat_sectors;

    if (!(buf = calloc( 3, sector ))) return -1;
    fat_label( v, label );
    common_bpb( v, buf, reserved, spc, 0, total, 0xf8 );
    put32( buf + 36, fat_sectors );
    put32( buf + 44, 2 );              /* the root directory is cluster 2 */
    put16( buf + 48, 1 );              /* FSInfo */
    put16( buf + 50, 6 );              /* the copy of the boot sectors */
    buf[64] = 0x80;
    buf[66] = 0x29;
    put32( buf + 67, v->serial );
    memcpy( buf + 71, label, 11 );
    memcpy( buf + 82, "FAT32   ", 8 );
    buf[1] = 0x58;
    memcpy( buf + 90, stub, sizeof(stub) );

    /* FSInfo */
    put32( buf + sector, 0x41615252 );
    put32( buf + sector + 484, 0x61417272 );
    put32( buf + sector + 488, (uint32_t)clusters - 1 );
    put32( buf + sector + 492, 3 );
    put16( buf + sector + 510, 0xaa55 );
    /* the third sector of the boot record */
    put16( buf + 2 * sector + 510, 0xaa55 );

    if ((status = fmt_zero( v, 0, data_start * sector, 0, v->quick ? 60 : 10 ))) goto done;
    if (!v->quick && (status = fmt_zero( v, data_start * sector, v->size - data_start * sector, 10, 90 ))) goto done;
    if ((status = v->write( v->context, 0, buf, 3 * sector ))) goto done;
    if ((status = v->write( v->context, 6ull * sector, buf, 3 * sector ))) goto done;

    {
        uint8_t *fat = calloc( 1, sector );
        uint8_t *root = calloc( spc, sector );

        if (!fat || !root) status = -1;
        else
        {
            put32( fat, 0x0ffffff8 );
            put32( fat + 4, 0x0fffffff );
            put32( fat + 8, 0x0fffffff );   /* the root directory: one cluster */
            status = v->write( v->context, (uint64_t)reserved * sector, fat, sector );
            if (!status) status = v->write( v->context, ((uint64_t)reserved + fat_sectors) * sector, fat, sector );
            if (v->label[0])
            {
                memcpy( root, label, 11 );
                root[11] = 0x08;
            }
            if (!status) status = v->write( v->context, data_start * sector, root, spc * sector );
        }
        free( fat );
        free( root );
    }
done:
    free( buf );
    return status;
}

static int format_fat16( struct volume *v )
{
    uint32_t sector = v->sector, reserved = 1, root_entries = 512, spc, fat_sectors = 1, root_sectors, bits;
    uint64_t total = v->size / sector, clusters = 0, data_start;
    char label[11];
    uint8_t *buf;
    int status, i;

    root_sectors = (root_entries * 32 + sector - 1) / sector;
    spc = v->cluster ? v->cluster / sector : 1;
    if (!is_power_of_2( spc )) return -1;
    for (;; spc *= 2)
    {
        if (spc > 128) return -1;
        for (i = 0; i < 8; i++)
        {
            clusters = (total - reserved - root_sectors - 2ull * fat_sectors) / spc;
            bits = clusters < 4085 ? 12 : 16;
            fat_sectors = (uint32_t)(((clusters + 2) * bits / 8 + sector) / sector);
        }
        clusters = (total - reserved - root_sectors - 2ull * fat_sectors) / spc;
        /* a cluster too small for FAT16 on this volume grows, as FAT32's shrinks */
        if (clusters < 65525) break;
        fat_sectors = 1;
    }
    if (clusters >= 65525 || !clusters) return -1;
    bits = clusters < 4085 ? 12 : 16;
    data_start = reserved + 2ull * fat_sectors + root_sectors;

    if (!(buf = calloc( 1, sector ))) return -1;
    fat_label( v, label );
    common_bpb( v, buf, reserved, spc, root_entries, total, 0xf8 );
    put16( buf + 22, fat_sectors );
    buf[36] = 0x80;
    buf[38] = 0x29;
    put32( buf + 39, v->serial );
    memcpy( buf + 43, label, 11 );
    memcpy( buf + 54, bits == 12 ? "FAT12   " : "FAT16   ", 8 );
    buf[1] = 0x3c;
    memcpy( buf + 62, stub, sizeof(stub) );

    if ((status = fmt_zero( v, 0, data_start * sector, 0, v->quick ? 60 : 10 ))) goto done;
    if (!v->quick && (status = fmt_zero( v, data_start * sector, v->size - data_start * sector, 10, 90 ))) goto done;
    if ((status = v->write( v->context, 0, buf, sector ))) goto done;
    {
        uint8_t *fat = calloc( 1, sector );

        if (!fat) status = -1;
        else
        {
            fat[0] = 0xf8;
            fat[1] = 0xff;
            fat[2] = 0xff;
            if (bits == 16) fat[3] = 0xff;
            status = v->write( v->context, (uint64_t)reserved * sector, fat, sector );
            if (!status) status = v->write( v->context, ((uint64_t)reserved + fat_sectors) * sector, fat, sector );
            if (!status && v->label[0])
            {
                memset( fat, 0, sector );
                memcpy( fat, label, 11 );
                fat[11] = 0x08;
                status = v->write( v->context, ((uint64_t)reserved + 2ull * fat_sectors) * sector, fat, sector );
            }
        }
        free( fat );
    }
done:
    free( buf );
    return status;
}

int format_fat( struct volume *v, int bits )
{
    return bits == 32 ? format_fat32( v ) : format_fat16( v );
}
