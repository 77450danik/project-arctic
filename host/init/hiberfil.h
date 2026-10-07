#ifndef ARCTIC_HIBERFIL_H
#define ARCTIC_HIBERFIL_H

/* C:\hiberfil.sys, as Windows keeps its sleeping kernel (docs/hibernation.md).
 * arctic-init writes the kernel's snapshot (/dev/snapshot) into the file's
 * own sectors, found with FIEMAP, without touching the file system; the
 * initrd reads it back from the disk before C: is mounted. Where the file
 * starts on the disk is kept in an EFI variable.
 *
 * The file: the first MiB is the header area (the header, the file's
 * extents, a CRC-32C for every 4 MiB of the image), the image follows. */

#include <stddef.h>
#include <stdint.h>

#define HIB_MAGIC "ARCTICHB"         /* an image to wake from */
#define HIB_USED "ARCTICUS"          /* woken from (or given up): never again */
#define HIB_VERSION 1
#define HIB_HEADER_AREA (1u << 20)
#define HIB_CHUNK (4u << 20)
#define HIB_MAX_EXTENTS 8192
#define HIB_EXTENTS_AT 4096
#define HIB_CRCS_AT (HIB_EXTENTS_AT + HIB_MAX_EXTENTS * 24)
#define HIB_MAX_CHUNKS ((HIB_HEADER_AREA - HIB_CRCS_AT) / 4)

#define HIB_FAST_STARTUP 1           /* the session was closed: start a new one */
#define HIB_HIBERNATE 2              /* everything as it was */

struct hib_extent {
    uint64_t file_offset, disk_offset, length; /* disk_offset: bytes from the start of the whole disk */
};

struct hib_header {
    char magic[8];
    uint32_t version, mode;
    char kernel[200];   /* uname -r and -v: only this very kernel can take the image */
    char partuuid[40];  /* C:'s */
    uint64_t image_bytes, created;
    uint32_t extent_count, chunk_count;
    uint32_t area_crc;  /* CRC-32C of the whole header area, with this field 0 */
    uint32_t reserved;
};

/* The EFI variable ArcticHiberfil: which partition, where the header area is */
struct hib_where {
    char partuuid[40];
    uint64_t header_disk_offset;
    char disk[32]; /* the disk's name when it was written ("sdb"), only a hint */
};

uint32_t hib_crc32c(uint32_t crc, const void *data, size_t len);
uint32_t hib_crc32(uint32_t crc, const void *data, size_t len); /* GPT's */

/* Windows' "basic data" type, and Arctic's for a C: asleep: Windows gives a
 * partition of an unknown type no letter and does not mount it */
extern const uint8_t hib_type_basic_data[16], hib_type_asleep[16];

/* The type GUID of partition num (1-based) on the disk open as fd */
int hib_gpt_get_type(int fd, int num, uint8_t type[16]);
/* Sets it in the primary and the backup table, CRCs included */
int hib_gpt_set_type(int fd, int num, const uint8_t type[16]);

/* The whole disk under a partition ("sdb3" -> "sdb", 3, its start in bytes) */
int hib_partition_disk(const char *part, char *disk, size_t len, int *num, uint64_t *start);

int hib_efi_mount(void);
int hib_efi_read(struct hib_where *w);
int hib_efi_write(const struct hib_where *w); /* only when it differs */

#endif
