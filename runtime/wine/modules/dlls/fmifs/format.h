/*
 * What fmifs.dll's formatters share: they write through a function, so that
 * they can be tried on an image file (tests/tools/format-test.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __FMIFS_FORMAT_H
#define __FMIFS_FORMAT_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint16_t utf16_t;

struct volume
{
    void    *context;
    int    (*write)( void *context, uint64_t offset, const void *data, uint32_t size );   /* 0 if it worked */
    void   (*progress)( void *context, unsigned int percent );
    uint32_t(*upcase)( uint32_t c );
    uint64_t size;          /* bytes */
    uint32_t sector;        /* bytes */
    uint64_t hidden;        /* sectors before the volume on its disk */
    uint32_t serial;        /* of the new volume */
    uint32_t cluster;       /* bytes; 0 for the default */
    int      quick;
    uint64_t nt_time;       /* now, in 100 ns units since 1601 */
    const utf16_t *label;   /* NUL-terminated; may be empty */
};

/* 0 on success */
extern int format_fat( struct volume *v, int bits );   /* bits: 32, or 0 for FAT12 or 16 by size */
extern int format_exfat( struct volume *v );
extern int format_ntfs( struct volume *v );

/* zeroes from offset, reporting progress between two percentages */
extern int fmt_zero( struct volume *v, uint64_t offset, uint64_t size, unsigned int from, unsigned int to );

static inline int is_power_of_2( uint32_t n ) { return n && !(n & (n - 1)); }

static inline void put16( void *p, uint16_t v ) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static inline void put32( void *p, uint32_t v ) { put16( p, v ); put16( (uint8_t *)p + 2, v >> 16 ); }
static inline void put64( void *p, uint64_t v ) { put32( p, (uint32_t)v ); put32( (uint8_t *)p + 4, (uint32_t)(v >> 32) ); }

#endif
