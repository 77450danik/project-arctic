/*
 * fmifs.dll: NTFS 3.1, the structures mkntfs and Windows' format leave on a new volume.
 *
 * The first 27 records of the MFT are the system files, the root directory
 * holds them in one index block, and $LogFile is all 0xff, which is what a
 * log that has never been written looks like to both Windows and Linux.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "format.h"

#define RECORD_SIZE 1024u
#define INDEX_SIZE  4096u
#define RECORDS     27u

enum { AT_STD = 0x10, AT_NAME = 0x30, AT_SD = 0x50, AT_VOLNAME = 0x60, AT_VOLINFO = 0x70, AT_DATA = 0x80,
       AT_IROOT = 0x90, AT_IALLOC = 0xa0, AT_BITMAP = 0xb0, AT_END = 0xffffffffu };

enum { F_MFT, F_MFTMIRR, F_LOGFILE, F_VOLUME, F_ATTRDEF, F_ROOT, F_BITMAP, F_BOOT, F_BADCLUS, F_SECURE, F_UPCASE,
       F_EXTEND, F_QUOTA = 24, F_OBJID = 25, F_REPARSE = 26 };

#define FA_HIDDEN  0x02
#define FA_SYSTEM  0x04
#define FA_ARCHIVE 0x20
#define FA_DIR     0x10000000u

struct ntfs
{
    struct volume *v;
    uint32_t cluster;
    uint64_t clusters;
    uint64_t next;          /* the next free cluster, handing out from the start */
    uint8_t *bitmap;
    uint32_t bitmap_bytes;
    uint8_t *mft;
};

struct run { uint64_t lcn, count; };

static uint32_t align8( uint32_t n ) { return (n + 7) & ~7u; }

static void mark( struct ntfs *n, uint64_t lcn, uint64_t count )
{
    for (; count; count--, lcn++) n->bitmap[lcn / 8] |= 1 << (lcn % 8);
}

static uint64_t bytes_to_clusters( const struct ntfs *n, uint64_t bytes ) { return (bytes + n->cluster - 1) / n->cluster; }

static struct run take( struct ntfs *n, uint64_t bytes )
{
    struct run r;

    r.count = bytes_to_clusters( n, bytes );
    r.lcn = n->next;
    n->next += r.count;
    mark( n, r.lcn, r.count );
    return r;
}

static uint8_t *record( struct ntfs *n, unsigned int i ) { return n->mft + i * RECORD_SIZE; }

static uint16_t seq_of( unsigned int i ) { return i ? (uint16_t)i : 1; }

static uint64_t mref( unsigned int i ) { return (uint64_t)i | ((uint64_t)seq_of( i ) << 48); }

static void begin_record( struct ntfs *n, unsigned int i, uint16_t flags )
{
    uint8_t *m = record( n, i );

    memcpy( m, "FILE", 4 );
    put16( m + 4, 48 );                 /* update sequence */
    put16( m + 6, RECORD_SIZE / 512 + 1 );
    put16( m + 16, seq_of( i ) );
    put16( m + 18, 1 );                 /* links */
    put16( m + 20, 56 );                /* the first attribute */
    put16( m + 22, flags );
    put32( m + 24, 56 + 8 );
    put32( m + 28, RECORD_SIZE );
    put32( m + 44, i );
    put32( m + 56, AT_END );
}

static uint32_t used_of( const uint8_t *m ) { return m[24] | m[25] << 8 | m[26] << 16 | (uint32_t)m[27] << 24; }

/* makes room for an attribute of this size at the end, returns it */
static uint8_t *new_attr( uint8_t *m, uint32_t type, uint32_t size, const utf16_t *name, uint32_t name_len )
{
    uint32_t used = used_of( m ) - 8;   /* the end marker */
    uint16_t instance = (uint16_t)(m[40] | m[41] << 8);
    uint8_t *a = m + used;

    memset( a, 0, size + 8 );
    put32( a, type );
    put32( a + 4, size );
    a[9] = (uint8_t)name_len;
    put16( a + 14, instance );
    put16( m + 40, instance + 1 );
    put32( a + size, AT_END );
    put32( m + 24, used + size + 8 );
    (void)name;
    return a;
}

static void add_resident( uint8_t *m, uint32_t type, const utf16_t *name, uint32_t name_len, const void *value,
                          uint32_t size, uint8_t flags )
{
    uint32_t name_off = 24, value_off = align8( name_off + name_len * 2 );
    uint8_t *a = new_attr( m, type, align8( value_off + size ), name, name_len );
    uint32_t i;

    a[8] = 0;
    put16( a + 10, name_off );
    put32( a + 16, size );
    put16( a + 20, value_off );
    a[22] = flags;
    for (i = 0; i < name_len; i++) put16( a + name_off + i * 2, name[i] );
    if (size) memcpy( a + value_off, value, size );
}

static uint32_t encode_run( uint8_t *p, const struct run *r, int sparse )
{
    uint8_t *start = p;
    uint64_t v = r->count;
    int lb = 0, ob = 0;
    uint8_t *head = p++;
    int64_t off = (int64_t)r->lcn;

    do { *p++ = (uint8_t)v; v >>= 8; lb++; } while (v);
    /* the count is a signed value too */
    if (p[-1] & 0x80) { *p++ = 0; lb++; }
    if (!sparse)
    {
        do
        {
            *p++ = (uint8_t)off;
            off >>= 8;
            ob++;
        } while (off != 0 && off != -1);
        if ((off == 0 && (p[-1] & 0x80)) || (off == -1 && !(p[-1] & 0x80))) { *p++ = (uint8_t)off; ob++; }
    }
    *head = (uint8_t)(ob << 4 | lb);
    return (uint32_t)(p - start);
}

static void add_nonresident( uint8_t *m, uint32_t type, const utf16_t *name, uint32_t name_len, const struct run *r,
                             uint64_t size, int sparse )
{
    uint8_t pairs[32];
    uint32_t len = encode_run( pairs, r, sparse ), name_off = 64, map_off = align8( name_off + name_len * 2 ), i;
    uint8_t *a = new_attr( m, type, align8( map_off + len + 1 ), name, name_len );

    a[8] = 1;
    put16( a + 10, name_off );
    put64( a + 16, 0 );
    put64( a + 24, r->count - 1 );
    put16( a + 32, map_off );
    put64( a + 40, sparse ? r->count * 4096 : r->count * 1ull );
    put64( a + 48, size );
    put64( a + 56, size );
    for (i = 0; i < name_len; i++) put16( a + name_off + i * 2, name[i] );
    memcpy( a + map_off, pairs, len );
}

static void fix_allocated( uint8_t *m, uint32_t type, uint64_t bytes )
{
    uint32_t pos = 56;

    while (pos < used_of( m ) - 8)
    {
        uint32_t t = m[pos] | m[pos + 1] << 8 | m[pos + 2] << 16 | (uint32_t)m[pos + 3] << 24;
        uint32_t len = m[pos + 4] | m[pos + 5] << 8 | m[pos + 6] << 16;

        if (t == type && m[pos + 8]) { put64( m + pos + 40, bytes ); return; }
        pos += len;
    }
}

static void add_std( const struct ntfs *n, uint8_t *m, uint32_t attrs )
{
    uint8_t v[72];
    uint64_t t = n->v->nt_time;

    memset( v, 0, sizeof(v) );
    put64( v, t ); put64( v + 8, t ); put64( v + 16, t ); put64( v + 24, t );
    put32( v + 32, attrs );
    put32( v + 52, 0x100 );             /* the descriptor in $Secure */
    add_resident( m, AT_STD, NULL, 0, v, sizeof(v), 0 );
}

/* $FILE_NAME's value, returns its size */
static uint32_t file_name( const struct ntfs *n, uint8_t *v, unsigned int parent, const char *name, uint64_t alloc,
                           uint64_t size, uint32_t attrs )
{
    uint32_t len = (uint32_t)strlen( name ), i;
    uint64_t t = n->v->nt_time;

    memset( v, 0, 66 + len * 2 );
    put64( v, mref( parent ) );
    put64( v + 8, t ); put64( v + 16, t ); put64( v + 24, t ); put64( v + 32, t );
    put64( v + 40, alloc );
    put64( v + 48, size );
    put32( v + 56, attrs );
    v[64] = (uint8_t)len;
    v[65] = 3;                          /* the Windows and DOS name in one */
    for (i = 0; i < len; i++) put16( v + 66 + i * 2, (utf16_t)name[i] );
    return 66 + len * 2;
}

static void add_name( const struct ntfs *n, uint8_t *m, unsigned int parent, const char *name, uint64_t alloc,
                      uint64_t size, uint32_t attrs )
{
    uint8_t v[128];
    uint32_t len = file_name( n, v, parent, name, alloc, size, attrs );

    add_resident( m, AT_NAME, NULL, 0, v, len, 1 );
}

static void fixup( uint8_t *block, uint32_t size, uint32_t usa_offset )
{
    uint32_t count = size / 512, i;
    uint16_t usn = 1;

    put16( block + usa_offset, usn );
    for (i = 0; i < count; i++)
    {
        uint8_t *end = block + (i + 1) * 512 - 2;

        block[usa_offset + 2 + i * 2] = end[0];
        block[usa_offset + 3 + i * 2] = end[1];
        put16( end, usn );
    }
}

/* the index of a directory or a system table, with no entries of its own */
static uint32_t empty_index_root( uint8_t *v, uint32_t type, uint32_t collation )
{
    memset( v, 0, 64 );
    put32( v, type );
    put32( v + 4, collation );
    put32( v + 8, INDEX_SIZE );
    v[12] = INDEX_SIZE >= 4096 ? 1 : 0xf4;
    /* the header of the entries */
    put32( v + 16, 16 );
    put32( v + 20, 16 + 16 );
    put32( v + 24, 16 + 16 );
    /* the last entry */
    put16( v + 16 + 16 + 8, 16 );
    put16( v + 16 + 16 + 12, 2 );
    return 16 + 16 + 16;
}

/* the default descriptor: everyone may do everything (the volume is a removable one's) */
static uint32_t default_sd( uint8_t *out )
{
    static const uint8_t sd[] = {
        1, 0, 0x04, 0x80,  0x14, 0, 0, 0,  0x20, 0, 0, 0,  0, 0, 0, 0,  0x2c, 0, 0, 0,
        /* owner: S-1-5-32-544 */
        1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0,
        /* group: the same */
        1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 2, 0, 0,
        /* the list: Everyone, all rights */
        2, 0, 0x1c, 0, 1, 0, 0, 0,
        0, 3, 0x14, 0, 0xff, 1, 0x1f, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    };

    memcpy( out, sd, sizeof(sd) );
    return sizeof(sd);
}

static uint32_t sd_hash( const uint8_t *sd, uint32_t size )
{
    uint32_t hash = 0, i;

    for (i = 0; i + 4 <= size; i += 4)
    {
        uint32_t word = sd[i] | sd[i + 1] << 8 | sd[i + 2] << 16 | (uint32_t)sd[i + 3] << 24;

        hash = word + ((hash >> 29) | (hash << 3));
    }
    return hash;
}

static const struct { const char *name; uint32_t type, collation, flags; int64_t min, max; } attr_defs[] =
{
    { "$STANDARD_INFORMATION", 0x10, 0, 0x40, 0x30, 0x48 },
    { "$ATTRIBUTE_LIST", 0x20, 0, 0x80, 0, -1 },
    { "$FILE_NAME", 0x30, 1, 0x42, 0x44, 0x242 },
    { "$OBJECT_ID", 0x40, 0, 0x40, 0, 0x100 },
    { "$SECURITY_DESCRIPTOR", 0x50, 0, 0x80, 0, -1 },
    { "$VOLUME_NAME", 0x60, 0, 0x40, 2, 0x100 },
    { "$VOLUME_INFORMATION", 0x70, 0, 0x40, 0xc, 0xc },
    { "$DATA", 0x80, 0, 0, 0, -1 },
    { "$INDEX_ROOT", 0x90, 0, 0x40, 0, -1 },
    { "$INDEX_ALLOCATION", 0xa0, 0, 0x80, 0, -1 },
    { "$BITMAP", 0xb0, 0, 0x80, 0, -1 },
    { "$REPARSE_POINT", 0xc0, 0, 0x80, 0, 0x4000 },
    { "$EA_INFORMATION", 0xd0, 0, 0x40, 8, 8 },
    { "$EA", 0xe0, 0, 0, 0, 0x10000 },
    { "$LOGGED_UTILITY_STREAM", 0x100, 0, 0x80, 0, 0x10000 },
};

static const struct { const char *name; unsigned int rec; } root_files[] =
{
    /* in the order of the index: by upper-case name */
    { "$AttrDef", F_ATTRDEF }, { "$BadClus", F_BADCLUS }, { "$Bitmap", F_BITMAP }, { "$Boot", F_BOOT },
    { "$Extend", F_EXTEND }, { "$LogFile", F_LOGFILE }, { "$MFT", F_MFT }, { "$MFTMirr", F_MFTMIRR },
    { "$Secure", F_SECURE }, { "$UpCase", F_UPCASE }, { "$Volume", F_VOLUME }, { ".", F_ROOT },
};

static const utf16_t I30[] = { '$', 'I', '3', '0' };

static uint64_t data_size_of( const uint8_t *m )
{
    uint32_t pos = 56;

    if (used_of( m ) < 64) return 0; /* a record not made yet */
    while (pos < used_of( m ) - 8)
    {
        uint32_t t = m[pos] | m[pos + 1] << 8 | m[pos + 2] << 16 | (uint32_t)m[pos + 3] << 24;
        uint32_t len = m[pos + 4] | m[pos + 5] << 8 | m[pos + 6] << 16;

        if (t == AT_DATA && !m[pos + 9])
        {
            if (m[pos + 8])
            {
                uint64_t v = 0;
                int i;

                for (i = 7; i >= 0; i--) v = v << 8 | m[pos + 48 + i];
                return v;
            }
            return (uint32_t)(m[pos + 16] | m[pos + 17] << 8 | m[pos + 18] << 16);
        }
        pos += len;
    }
    return 0;
}

int format_ntfs( struct volume *v )
{
    struct ntfs n;
    uint8_t *boot = NULL, *block = NULL, *attrdef = NULL, *upcase = NULL, *sds = NULL, *logfile = NULL, *mirror = NULL;
    struct run mft_run, mirr_run, log_run, bitmap_run, boot_run, attrdef_run, upcase_run, root_run, sds_run, mftbmp_run, bad_run;
    uint64_t log_size, sds_size, mft_bytes, mirr_bytes;
    uint32_t sd_len, i, spc, records;
    uint8_t sd[256], val[512], extra[512];
    int status = -1;

    memset( &n, 0, sizeof(n) );
    n.v = v;
    n.cluster = v->cluster ? v->cluster : 4096;
    if (n.cluster < v->sector || !is_power_of_2( n.cluster ) || n.cluster > 65536 || v->sector > 4096) return -1;
    spc = n.cluster / v->sector;
    /* the last sector of the volume keeps the copy of the boot sector */
    n.clusters = (v->size / v->sector - 1) / spc;
    if (n.clusters < 4096) return -1;
    n.bitmap_bytes = (uint32_t)(((n.clusters + 63) / 64) * 8);

    /* the MFT and its mirror are at least a cluster: with large clusters
     * the records that do not exist yet are there, marked unused */
    mft_bytes = bytes_to_clusters( &n, (uint64_t)RECORDS * RECORD_SIZE ) * n.cluster;
    records = (uint32_t)(mft_bytes / RECORD_SIZE);
    mirr_bytes = 4ull * RECORD_SIZE > n.cluster ? 4ull * RECORD_SIZE : n.cluster;

    if (!(n.bitmap = calloc( 1, n.bitmap_bytes )) || !(n.mft = calloc( records, RECORD_SIZE ))) goto done;
    /* what lies past the last cluster counts as used */
    for (i = (uint32_t)n.clusters; i < n.bitmap_bytes * 8; i++) n.bitmap[i / 8] |= 1 << (i % 8);

    /* where everything goes: $Boot at the start, the MFT 4 clusters on, the
     * mirror in the middle, as mkntfs places them */
    boot_run = take( &n, 8192 );
    n.next = n.clusters > (1u << 16) ? (1u << 20) / n.cluster * 16 / 16 : n.next;
    if (n.next < boot_run.count + 4) n.next = boot_run.count + 4;
    mft_run = take( &n, mft_bytes );
    mftbmp_run = take( &n, 8 );
    attrdef_run = take( &n, 0xa00 );
    upcase_run = take( &n, 0x20000 );
    root_run = take( &n, INDEX_SIZE );
    bitmap_run = take( &n, n.bitmap_bytes );
    sd_len = default_sd( sd );
    /* every descriptor is written twice, 256 KB apart */
    sds_size = 0x40000 + align8( 20 + sd_len );
    sds_run = take( &n, sds_size );
    {
        uint64_t saved = n.next;

        n.next = n.clusters / 2;
        mirr_run = take( &n, mirr_bytes );
        log_size = v->size / 100;
        if (log_size < 2 << 20) log_size = 2 << 20;
        if (log_size > 64 << 20) log_size = 64 << 20;
        log_size = (log_size + n.cluster - 1) / n.cluster * n.cluster;
        if (n.next + log_size / n.cluster >= n.clusters) goto done;
        log_run = take( &n, log_size );
        n.next = saved;
    }
    if (n.next >= n.clusters / 2) goto done;

    /* the system files */
    begin_record( &n, F_MFT, 1 );
    add_std( &n, record( &n, F_MFT ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_MFT ), F_ROOT, "$MFT", mft_run.count * n.cluster, mft_bytes, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_MFT ), AT_DATA, NULL, 0, &mft_run, mft_bytes, 0 );
    fix_allocated( record( &n, F_MFT ), AT_DATA, mft_run.count * n.cluster );
    {
        struct run r = mftbmp_run;

        add_nonresident( record( &n, F_MFT ), AT_BITMAP, NULL, 0, &r, 8, 0 );
        fix_allocated( record( &n, F_MFT ), AT_BITMAP, r.count * n.cluster );
    }

    begin_record( &n, F_MFTMIRR, 1 );
    add_std( &n, record( &n, F_MFTMIRR ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_MFTMIRR ), F_ROOT, "$MFTMirr", mirr_run.count * n.cluster, mirr_bytes, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_MFTMIRR ), AT_DATA, NULL, 0, &mirr_run, mirr_bytes, 0 );
    fix_allocated( record( &n, F_MFTMIRR ), AT_DATA, mirr_run.count * n.cluster );

    begin_record( &n, F_LOGFILE, 1 );
    add_std( &n, record( &n, F_LOGFILE ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_LOGFILE ), F_ROOT, "$LogFile", log_run.count * n.cluster, log_size, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_LOGFILE ), AT_DATA, NULL, 0, &log_run, log_size, 0 );
    fix_allocated( record( &n, F_LOGFILE ), AT_DATA, log_run.count * n.cluster );

    begin_record( &n, F_VOLUME, 1 );
    add_std( &n, record( &n, F_VOLUME ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_VOLUME ), F_ROOT, "$Volume", 0, 0, FA_HIDDEN | FA_SYSTEM );
    {
        uint32_t len = 0;

        while (v->label[len] && len < 32) len++;
        for (i = 0; i < len; i++) put16( val + i * 2, v->label[i] );
        add_resident( record( &n, F_VOLUME ), AT_VOLNAME, NULL, 0, val, len * 2, 0 );
        memset( val, 0, 12 );
        val[8] = 3;
        val[9] = 1;
        add_resident( record( &n, F_VOLUME ), AT_VOLINFO, NULL, 0, val, 12, 0 );
        add_resident( record( &n, F_VOLUME ), AT_DATA, NULL, 0, NULL, 0, 0 );
    }

    if (!(attrdef = calloc( 1, attrdef_run.count * n.cluster ))) goto done;
    for (i = 0; i < sizeof(attr_defs) / sizeof(attr_defs[0]); i++)
    {
        uint8_t *e = attrdef + i * 160;
        const char *s = attr_defs[i].name;
        int c;

        for (c = 0; s[c]; c++) put16( e + c * 2, (utf16_t)s[c] );
        put32( e + 128, attr_defs[i].type );
        put32( e + 136, attr_defs[i].collation );
        put32( e + 140, attr_defs[i].flags );
        put64( e + 144, (uint64_t)attr_defs[i].min );
        put64( e + 152, (uint64_t)attr_defs[i].max );
    }
    begin_record( &n, F_ATTRDEF, 1 );
    add_std( &n, record( &n, F_ATTRDEF ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_ATTRDEF ), F_ROOT, "$AttrDef", attrdef_run.count * n.cluster, 0xa00, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_ATTRDEF ), AT_DATA, NULL, 0, &attrdef_run, 0xa00, 0 );
    fix_allocated( record( &n, F_ATTRDEF ), AT_DATA, attrdef_run.count * n.cluster );

    begin_record( &n, F_BITMAP, 1 );
    add_std( &n, record( &n, F_BITMAP ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_BITMAP ), F_ROOT, "$Bitmap", bitmap_run.count * n.cluster, n.bitmap_bytes, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_BITMAP ), AT_DATA, NULL, 0, &bitmap_run, n.bitmap_bytes, 0 );
    fix_allocated( record( &n, F_BITMAP ), AT_DATA, bitmap_run.count * n.cluster );

    begin_record( &n, F_BOOT, 1 );
    add_std( &n, record( &n, F_BOOT ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_BOOT ), F_ROOT, "$Boot", boot_run.count * n.cluster, 8192, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_BOOT ), AT_DATA, NULL, 0, &boot_run, 8192, 0 );
    fix_allocated( record( &n, F_BOOT ), AT_DATA, boot_run.count * n.cluster );

    begin_record( &n, F_BADCLUS, 1 );
    add_std( &n, record( &n, F_BADCLUS ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_BADCLUS ), F_ROOT, "$BadClus", 0, 0, FA_HIDDEN | FA_SYSTEM );
    add_resident( record( &n, F_BADCLUS ), AT_DATA, NULL, 0, NULL, 0, 0 );
    {
        static const utf16_t bad[] = { '$', 'B', 'a', 'd' };

        bad_run.lcn = 0;
        bad_run.count = n.clusters;
        add_nonresident( record( &n, F_BADCLUS ), AT_DATA, bad, 4, &bad_run, n.clusters * n.cluster, 1 );
        fix_allocated( record( &n, F_BADCLUS ), AT_DATA, n.clusters * n.cluster );
    }

    /* $Secure: one descriptor, for everything on the new volume */
    if (!(sds = calloc( 1, sds_run.count * n.cluster ))) goto done;
    {
        uint32_t hash = sd_hash( sd, sd_len );

        put32( sds, hash );
        put32( sds + 4, 0x100 );
        put64( sds + 8, 0 );
        put32( sds + 16, 20 + sd_len );
        memcpy( sds + 20, sd, sd_len );
        memcpy( sds + 0x40000, sds, 20 + sd_len );
        put64( sds + 0x40000 + 8, 0x40000 );

        begin_record( &n, F_SECURE, 1 );
        add_std( &n, record( &n, F_SECURE ), FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        add_name( &n, record( &n, F_SECURE ), F_ROOT, "$Secure", 0, 0, FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        {
            static const utf16_t name[] = { '$', 'S', 'D', 'S' };

            add_nonresident( record( &n, F_SECURE ), AT_DATA, name, 4, &sds_run, sds_size, 0 );
            fix_allocated( record( &n, F_SECURE ), AT_DATA, sds_run.count * n.cluster );
        }
        {
            /* $SDH: by hash, then id */
            static const utf16_t name[] = { '$', 'S', 'D', 'H' };
            uint32_t len = empty_index_root( val, 0, 0x12 ), key = 8, entry;

            entry = align8( 16 + key + 20 + 4 );
            put32( val + 20, 16 + entry + 16 );
            put32( val + 24, 16 + entry + 16 );
            memset( val + 32, 0, entry + 16 );
            put16( val + 32, 16 + key );        /* where the data lies */
            put16( val + 34, 20 );
            put16( val + 40, entry );
            put16( val + 42, key );
            put32( val + 48, hash );
            put32( val + 52, 0x100 );
            /* the data: the record's header in $SDS */
            put32( val + 56, hash );
            put32( val + 60, 0x100 );
            put64( val + 64, 0 );
            put32( val + 72, 20 + sd_len );
            memcpy( val + 76, "II", 2 ); /* padding, as Windows leaves it */
            put16( val + 32 + entry + 8, 16 );
            put16( val + 32 + entry + 12, 2 );
            len = 32 + entry + 16;
            add_resident( record( &n, F_SECURE ), AT_IROOT, name, 4, val, len, 0 );
        }
        {
            /* $SII: by id */
            static const utf16_t name[] = { '$', 'S', 'I', 'I' };
            uint32_t len = empty_index_root( extra, 0, 0x10 ), key = 4, entry;

            entry = align8( 16 + key + 20 );
            put32( extra + 20, 16 + entry + 16 );
            put32( extra + 24, 16 + entry + 16 );
            memset( extra + 32, 0, entry + 16 );
            put16( extra + 32, 16 + key );
            put16( extra + 34, 20 );
            put16( extra + 40, entry );
            put16( extra + 42, key );
            put32( extra + 48, 0x100 );
            put32( extra + 52, hash );
            put32( extra + 56, 0x100 );
            put64( extra + 60, 0 );
            put32( extra + 68, 20 + sd_len );
            put16( extra + 32 + entry + 8, 16 );
            put16( extra + 32 + entry + 12, 2 );
            len = 32 + entry + 16;
            add_resident( record( &n, F_SECURE ), AT_IROOT, name, 4, extra, len, 0 );
        }
    }

    if (!(upcase = calloc( 2, 0x10000 ))) goto done;
    for (i = 0; i < 0x10000; i++) put16( upcase + i * 2, (utf16_t)v->upcase( i ) );
    begin_record( &n, F_UPCASE, 1 );
    add_std( &n, record( &n, F_UPCASE ), FA_HIDDEN | FA_SYSTEM );
    add_name( &n, record( &n, F_UPCASE ), F_ROOT, "$UpCase", upcase_run.count * n.cluster, 0x20000, FA_HIDDEN | FA_SYSTEM );
    add_nonresident( record( &n, F_UPCASE ), AT_DATA, NULL, 0, &upcase_run, 0x20000, 0 );
    fix_allocated( record( &n, F_UPCASE ), AT_DATA, upcase_run.count * n.cluster );

    /* the reserved records and $Extend's tables */
    for (i = 12; i < 24; i++)
    {
        /* 12 to 15 are reserved and in use, 16 to 23 are free, as mkntfs and Windows leave them */
        begin_record( &n, i, i < 16 ? 1 : 0 );
        if (i < 16)
        {
            add_std( &n, record( &n, i ), FA_HIDDEN | FA_SYSTEM );
            add_resident( record( &n, i ), AT_DATA, NULL, 0, NULL, 0, 0 );
        }
    }
    for (i = RECORDS; i < records; i++) begin_record( &n, i, 0 );
    {
        static const utf16_t Q[] = { '$', 'Q' }, O[] = { '$', 'O' }, R[] = { '$', 'R' };
        uint32_t len;

        begin_record( &n, F_QUOTA, 1 );
        add_std( &n, record( &n, F_QUOTA ), FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        add_name( &n, record( &n, F_QUOTA ), F_EXTEND, "$Quota", 0, 0, FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        len = empty_index_root( val, 0, 0x11 );
        add_resident( record( &n, F_QUOTA ), AT_IROOT, O, 2, val, len, 0 );
        len = empty_index_root( val, 0, 0x10 );
        add_resident( record( &n, F_QUOTA ), AT_IROOT, Q, 2, val, len, 0 );

        begin_record( &n, F_OBJID, 1 );
        add_std( &n, record( &n, F_OBJID ), FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        add_name( &n, record( &n, F_OBJID ), F_EXTEND, "$ObjId", 0, 0, FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        len = empty_index_root( val, 0, 0x13 );
        add_resident( record( &n, F_OBJID ), AT_IROOT, O, 2, val, len, 0 );

        begin_record( &n, F_REPARSE, 1 );
        add_std( &n, record( &n, F_REPARSE ), FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        add_name( &n, record( &n, F_REPARSE ), F_EXTEND, "$Reparse", 0, 0, FA_HIDDEN | FA_SYSTEM | 0x20000000 );
        len = empty_index_root( val, 0, 0x13 );
        add_resident( record( &n, F_REPARSE ), AT_IROOT, R, 2, val, len, 0 );
    }

    /* $Extend: its index holds the three tables, in the order of their names */
    begin_record( &n, F_EXTEND, 3 );
    add_std( &n, record( &n, F_EXTEND ), FA_HIDDEN | FA_SYSTEM | FA_DIR );
    add_name( &n, record( &n, F_EXTEND ), F_ROOT, "$Extend", 0, 0, FA_HIDDEN | FA_SYSTEM | FA_DIR );
    {
        static const struct { const char *name; unsigned int rec; } children[] =
            { { "$ObjId", F_OBJID }, { "$Quota", F_QUOTA }, { "$Reparse", F_REPARSE } };
        uint32_t pos = 32;

        empty_index_root( val, AT_NAME, 1 );
        for (i = 0; i < 3; i++)
        {
            uint32_t key = file_name( &n, val + pos + 16, F_EXTEND, children[i].name, 0, 0, FA_HIDDEN | FA_SYSTEM | 0x20000000 );
            uint32_t entry = align8( 16 + key );

            memset( val + pos, 0, 16 );
            put64( val + pos, mref( children[i].rec ) );
            put16( val + pos + 8, entry );
            put16( val + pos + 10, key );
            pos += entry;
        }
        memset( val + pos, 0, 16 );
        put16( val + pos + 8, 16 );
        put16( val + pos + 12, 2 );
        pos += 16;
        put32( val + 20, pos - 16 );
        put32( val + 24, pos - 16 );
        add_resident( record( &n, F_EXTEND ), AT_IROOT, I30, 4, val, pos, 0 );
    }

    /* the root directory: every system file, in one index block */
    if (!(block = calloc( 1, INDEX_SIZE ))) goto done;
    memcpy( block, "INDX", 4 );
    put16( block + 4, 40 );
    put16( block + 6, INDEX_SIZE / 512 + 1 );
    {
        uint32_t entries = 64, pos = entries, usa_size = (INDEX_SIZE / 512 + 1) * 2;

        (void)usa_size;
        for (i = 0; i < sizeof(root_files) / sizeof(root_files[0]); i++)
        {
            const uint8_t *m = record( &n, root_files[i].rec );
            uint64_t size = data_size_of( m ), alloc = bytes_to_clusters( &n, size ) * n.cluster;
            uint32_t attrs = root_files[i].rec == F_EXTEND || root_files[i].rec == F_ROOT ? FA_DIR | FA_HIDDEN | FA_SYSTEM : FA_HIDDEN | FA_SYSTEM;
            uint32_t key, entry;

            if (root_files[i].rec == F_SECURE) attrs |= 0x20000000;
            if (root_files[i].rec == F_MFT) { size = mft_bytes; alloc = mft_run.count * n.cluster; }
            key = file_name( &n, block + pos + 16, F_ROOT, root_files[i].name, alloc, size, attrs );
            entry = align8( 16 + key );
            put64( block + pos, mref( root_files[i].rec ) );
            put16( block + pos + 8, entry );
            put16( block + pos + 10, key );
            pos += entry;
        }
        put16( block + pos + 8, 16 );
        put16( block + pos + 12, 2 );
        pos += 16;
        /* the header of the entries, at 24 in the block */
        put32( block + 24, entries - 24 );
        put32( block + 28, pos - 24 );
        put32( block + 32, INDEX_SIZE - 24 );
    }
    fixup( block, INDEX_SIZE, 40 );

    begin_record( &n, F_ROOT, 3 );
    add_std( &n, record( &n, F_ROOT ), FA_HIDDEN | FA_SYSTEM | FA_DIR );
    add_name( &n, record( &n, F_ROOT ), F_ROOT, ".", 0, 0, FA_HIDDEN | FA_SYSTEM | FA_DIR );
    {
        uint32_t len = empty_index_root( val, AT_NAME, 1 );

        /* the last entry points at the block */
        put32( val + 20, 16 + 24 );
        put32( val + 24, 16 + 24 );
        put16( val + 32 + 8, 24 );
        put16( val + 32 + 12, 3 );
        put64( val + 32 + 16, 0 );
        val[28] = 1;
        len = 32 + 24;
        add_resident( record( &n, F_ROOT ), AT_IROOT, I30, 4, val, len, 0 );
        add_nonresident( record( &n, F_ROOT ), AT_IALLOC, I30, 4, &root_run, INDEX_SIZE, 0 );
        fix_allocated( record( &n, F_ROOT ), AT_IALLOC, root_run.count * n.cluster );
        memset( val, 0, 8 );
        val[0] = 1;
        add_resident( record( &n, F_ROOT ), AT_BITMAP, I30, 4, val, 8, 0 );
    }

    /* the MFT's bitmap: records 16 to 23 are not in use */
    {
        uint8_t bitmap[8] = { 0xff, 0xff, 0x00, 0x07 };
        uint8_t *cluster = calloc( 1, mftbmp_run.count * n.cluster );

        if (!cluster) goto done;
        memcpy( cluster, bitmap, 8 );
        status = fmt_zero( v, 0, n.next * (uint64_t)n.cluster, 0, v->quick ? 30 : 5 );
        if (!status && !v->quick)
            status = fmt_zero( v, n.next * (uint64_t)n.cluster, v->size - n.next * (uint64_t)n.cluster, 5, 80 );
        if (!status) status = v->write( v->context, mftbmp_run.lcn * n.cluster, cluster, mftbmp_run.count * n.cluster );
        free( cluster );
        if (status) goto done;
    }

    for (i = 0; i < records; i++) fixup( record( &n, i ), RECORD_SIZE, 48 );

    if ((status = v->write( v->context, mft_run.lcn * n.cluster, n.mft, (uint32_t)mft_bytes ))) goto done;
    if (!(mirror = calloc( 1, mirr_run.count * n.cluster ))) { status = -1; goto done; }
    memcpy( mirror, n.mft, (size_t)(mirr_bytes < mft_bytes ? mirr_bytes : mft_bytes) );
    if ((status = v->write( v->context, mirr_run.lcn * n.cluster, mirror, mirr_run.count * n.cluster ))) goto done;
    if (v->progress) v->progress( v->context, 85 );

    /* a log that was never written is all ones */
    if (!(logfile = malloc( 1 << 20 ))) { status = -1; goto done; }
    memset( logfile, 0xff, 1 << 20 );
    {
        uint64_t done_bytes = 0, bytes = log_run.count * n.cluster;

        while (done_bytes < bytes && !status)
        {
            uint32_t c = bytes - done_bytes > (1 << 20) ? (1 << 20) : (uint32_t)(bytes - done_bytes);

            status = v->write( v->context, log_run.lcn * n.cluster + done_bytes, logfile, c );
            done_bytes += c;
        }
        if (status) goto done;
    }

    if ((status = v->write( v->context, attrdef_run.lcn * n.cluster, attrdef, attrdef_run.count * n.cluster ))) goto done;
    {
        uint8_t *copy = calloc( 1, upcase_run.count * n.cluster );

        if (!copy) { status = -1; goto done; }
        memcpy( copy, upcase, 0x20000 );
        status = v->write( v->context, upcase_run.lcn * n.cluster, copy, upcase_run.count * n.cluster );
        free( copy );
        if (status) goto done;
    }
    if ((status = v->write( v->context, root_run.lcn * n.cluster, block, INDEX_SIZE ))) goto done;
    if ((status = v->write( v->context, sds_run.lcn * n.cluster, sds, sds_run.count * n.cluster ))) goto done;
    if (v->progress) v->progress( v->context, 92 );
    {
        uint8_t *copy = calloc( 1, bitmap_run.count * n.cluster );

        if (!copy) { status = -1; goto done; }
        memcpy( copy, n.bitmap, n.bitmap_bytes );
        status = v->write( v->context, bitmap_run.lcn * n.cluster, copy, bitmap_run.count * n.cluster );
        free( copy );
        if (status) goto done;
    }

    if (!(boot = calloc( 1, boot_run.count * n.cluster ))) { status = -1; goto done; }
    boot[0] = 0xeb;
    boot[1] = 0x52;
    boot[2] = 0x90;
    memcpy( boot + 3, "NTFS    ", 8 );
    put16( boot + 11, v->sector );
    boot[13] = (uint8_t)spc;
    boot[21] = 0xf8;
    put16( boot + 24, 63 );
    put16( boot + 26, 255 );
    put32( boot + 28, (uint32_t)v->hidden );
    put64( boot + 40, n.clusters * spc );
    put64( boot + 48, mft_run.lcn );
    put64( boot + 56, mirr_run.lcn );
    boot[64] = RECORD_SIZE >= n.cluster ? (uint8_t)(RECORD_SIZE / n.cluster) : 0xf6;
    boot[68] = INDEX_SIZE >= n.cluster ? (uint8_t)(INDEX_SIZE / n.cluster) : 0xf4;
    put32( boot + 72, v->serial );
    put32( boot + 76, v->serial ^ 0x5a5a5a5au );
    boot[510] = 0x55;
    boot[511] = 0xaa;
    memcpy( boot + 0x54, "\xf4\xeb\xfd", 3 );
    /* the copy at the very end comes first: the volume only counts once its first sector says so */
    status = v->write( v->context, v->size / v->sector * v->sector - v->sector, boot, v->sector );
    if (!status) status = v->write( v->context, 0, boot, (uint32_t)(boot_run.count * n.cluster) );

done:
    free( n.bitmap );
    free( n.mft );
    free( boot );
    free( block );
    free( attrdef );
    free( upcase );
    free( sds );
    free( logfile );
    free( mirror );
    return status;
}
