/*
 * fmifs.dll: exFAT, by Microsoft's published specification.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "format.h"

static int log2_of( uint32_t n )
{
    int i = 0;

    while (n > 1) { n >>= 1; i++; }
    return i;
}

static uint32_t boot_checksum( const uint8_t *data, uint32_t size )
{
    uint32_t sum = 0, i;

    for (i = 0; i < size; i++)
    {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + data[i];
    }
    return sum;
}

/* the up-case table in its compressed form: 0xffff, n skips n unchanged characters */
static uint32_t make_upcase( const struct volume *v, uint8_t *table )
{
    uint32_t c = 0, n = 0;

    while (c < 0x10000)
    {
        uint32_t up = v->upcase( c ) & 0xffff;

        if (up == c)
        {
            uint32_t run = 0;

            while (c + run < 0x10000 && (v->upcase( c + run ) & 0xffff) == c + run && run < 0xffff) run++;
            if (run >= 4)
            {
                put16( table + n, 0xffff ); n += 2;
                put16( table + n, (uint16_t)run ); n += 2;
                c += run;
                continue;
            }
        }
        put16( table + n, (uint16_t)up ); n += 2;
        c++;
    }
    return n;
}

static uint32_t table_checksum( const uint8_t *data, uint32_t size )
{
    uint32_t sum = 0, i;

    for (i = 0; i < size; i++) sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + data[i];
    return sum;
}

int format_exfat( struct volume *v )
{
    uint32_t sector = v->sector, cluster, fat_offset, fat_length, heap_offset, clusters, spc;
    uint32_t bitmap_bytes, bitmap_clusters, upcase_bytes, upcase_clusters, first_free, i;
    uint64_t total = v->size / sector;
    uint8_t *boot = NULL, *fat = NULL, *bitmap = NULL, *upcase = NULL, *root = NULL;
    uint32_t sum;
    int status = -1, n;

    cluster = v->cluster ? v->cluster :
              v->size <= 256ull << 20 ? 4096 : v->size <= 32ull << 30 ? 32768 : 131072;
    if (cluster < sector) cluster = sector;
    if (!is_power_of_2( cluster ) || cluster > 32u << 20) return -1;
    spc = cluster / sector;

    /* the FAT begins on a boundary a medium erases at, as Windows aligns it */
    fat_offset = (1u << 20) / sector;
    if (fat_offset < 24) fat_offset = 24;
    clusters = (uint32_t)((total - fat_offset) / spc);
    fat_length = (uint32_t)(((uint64_t)(clusters + 2) * 4 + sector - 1) / sector);
    heap_offset = fat_offset + fat_length;
    heap_offset = (heap_offset + spc - 1) / spc * spc;
    if (total <= heap_offset) return -1;
    clusters = (uint32_t)((total - heap_offset) / spc);
    if (clusters < 16) return -1;

    bitmap_bytes = (clusters + 7) / 8;
    bitmap_clusters = (bitmap_bytes + cluster - 1) / cluster;
    if (!(upcase = calloc( 2, 0x10000 ))) goto done;
    upcase_bytes = make_upcase( v, upcase );
    upcase_clusters = (upcase_bytes + cluster - 1) / cluster;
    first_free = 2 + bitmap_clusters + upcase_clusters + 1; /* the root directory takes one */

    if (!(boot = calloc( 12, sector )) || !(fat = calloc( 1, sector )) || !(root = calloc( 1, cluster )) ||
        !(bitmap = calloc( 1, bitmap_clusters * cluster )))
        goto done;

    boot[0] = 0xeb;
    boot[1] = 0x76;
    boot[2] = 0x90;
    memcpy( boot + 3, "EXFAT   ", 8 );
    put64( boot + 64, v->hidden );
    put64( boot + 72, total );
    put32( boot + 80, fat_offset );
    put32( boot + 84, fat_length );
    put32( boot + 88, heap_offset );
    put32( boot + 92, clusters );
    put32( boot + 96, 2 + bitmap_clusters + upcase_clusters );
    put32( boot + 100, v->serial );
    put16( boot + 104, 0x0100 );
    boot[108] = (uint8_t)log2_of( sector );
    boot[109] = (uint8_t)log2_of( spc );
    boot[110] = 1;
    boot[111] = 0x80;
    boot[112] = 0xff;
    boot[120] = 0xf4;
    boot[121] = 0xeb;
    boot[122] = 0xfd;
    boot[510] = 0x55;
    boot[511] = 0xaa;
    for (i = 1; i <= 8; i++) put32( boot + i * sector + sector - 4, 0xaa550000 );

    sum = boot_checksum( boot, 11 * sector );
    for (i = 0; i < sector / 4; i++) put32( boot + 11 * sector + i * 4, sum );

    for (i = 0; i < first_free - 2; i++) bitmap[i / 8] |= 1 << (i % 8);

    /* the FAT: the three system files are chains of consecutive clusters */
    {
        uint64_t fat_bytes = (uint64_t)fat_length * sector;
        uint8_t *table = calloc( 1, (first_free * 4 + sector - 1) / sector * sector );

        if (!table) goto done;
        put32( table, 0xfffffff8 );
        put32( table + 4, 0xffffffff );
        for (i = 2; i < first_free; i++)
        {
            uint32_t last = i == 1 + bitmap_clusters || i == 1 + bitmap_clusters + upcase_clusters || i == first_free - 1;

            put32( table + i * 4, last ? 0xffffffffu : i + 1 );
        }
        status = fmt_zero( v, 0, (uint64_t)heap_offset * sector, 0, v->quick ? 50 : 10 );
        if (!status && !v->quick)
            status = fmt_zero( v, (uint64_t)heap_offset * sector, v->size - (uint64_t)heap_offset * sector, 10, 85 );
        if (!status) status = v->write( v->context, (uint64_t)fat_offset * sector, table, (first_free * 4 + sector - 1) / sector * sector );
        free( table );
        (void)fat_bytes;
    }
    if (status) goto done;

    /* the root directory */
    n = 0;
    {
        uint8_t *e = root;
        int len = 0;

        while (len < 11 && v->label[len]) len++;
        e[0] = len ? 0x83 : 0x03;
        e[1] = (uint8_t)len;
        for (i = 0; i < (uint32_t)len; i++) put16( e + 2 + i * 2, v->label[i] );
        e += 32;
        e[0] = 0x81;
        put32( e + 20, 2 );
        put64( e + 24, bitmap_bytes );
        e += 32;
        e[0] = 0x82;
        put32( e + 4, table_checksum( upcase, upcase_bytes ) );
        put32( e + 20, 2 + bitmap_clusters );
        put64( e + 24, upcase_bytes );
        (void)n;
    }

    if ((status = v->write( v->context, (uint64_t)heap_offset * sector, bitmap, bitmap_clusters * cluster ))) goto done;
    {
        uint32_t size = upcase_clusters * cluster;
        uint8_t *copy = calloc( 1, size );

        if (!copy) { status = -1; goto done; }
        memcpy( copy, upcase, upcase_bytes );
        status = v->write( v->context, (uint64_t)heap_offset * sector + (uint64_t)bitmap_clusters * cluster, copy, size );
        free( copy );
        if (status) goto done;
    }
    if ((status = v->write( v->context, (uint64_t)heap_offset * sector + (uint64_t)(bitmap_clusters + upcase_clusters) * cluster,
                            root, cluster ))) goto done;
    if ((status = v->write( v->context, 12ull * sector, boot, 12 * sector ))) goto done;
    status = v->write( v->context, 0, boot, 12 * sector );

done:
    free( boot );
    free( fat );
    free( bitmap );
    free( upcase );
    free( root );
    return status;
}
