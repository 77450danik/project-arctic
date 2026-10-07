#define _GNU_SOURCE
#include "hiberfil.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <nmmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/* {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7} and {7A5E5B3C-1D2E-4F60-8A9B-41524354484E}
 * as a GPT stores them: the first three fields little-endian */
const uint8_t hib_type_basic_data[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                         0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
const uint8_t hib_type_asleep[16] = {0x3c, 0x5b, 0x5e, 0x7a, 0x2e, 0x1d, 0x60, 0x4f,
                                     0x8a, 0x9b, 0x41, 0x52, 0x43, 0x54, 0x48, 0x4e};

static uint32_t table32[256], table32c[256];

static void make_tables(void)
{
    if (table32[1])
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t a = i, b = i;
        for (int k = 0; k < 8; k++) {
            a = a & 1 ? a >> 1 ^ 0xedb88320u : a >> 1;
            b = b & 1 ? b >> 1 ^ 0x82f63b78u : b >> 1;
        }
        table32[i] = a;
        table32c[i] = b;
    }
}

uint32_t hib_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;

    make_tables();
    crc = ~crc;
    while (len--)
        crc = table32[(crc ^ *p++) & 0xff] ^ crc >> 8;
    return ~crc;
}

/* The image is checked at the speed it is read: SSE 4.2's CRC instruction
 * where the processor has it (every x86-64 since 2008), a table otherwise */
__attribute__((target("sse4.2"))) static uint32_t crc32c_hw(uint32_t crc, const uint8_t *p, size_t len)
{
    uint64_t c = crc;

    for (; len >= 8; p += 8, len -= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
    }
    crc = (uint32_t)c;
    while (len--)
        crc = _mm_crc32_u8(crc, *p++);
    return crc;
}

uint32_t hib_crc32c(uint32_t crc, const void *data, size_t len)
{
    static int hw = -1;
    const uint8_t *p = data;

    if (hw < 0) {
        __builtin_cpu_init();
        hw = __builtin_cpu_supports("sse4.2");
    }
    crc = ~crc;
    if (hw) {
        crc = crc32c_hw(crc, p, len);
    } else {
        make_tables();
        while (len--)
            crc = table32c[(crc ^ *p++) & 0xff] ^ crc >> 8;
    }
    return ~crc;
}

/* --- GPT --- */

struct gpt {
    int sector;
    uint8_t hdr[512];
    uint64_t entries_lba;
    uint32_t count, size;
    uint8_t *entries;
};

static int gpt_read(int fd, uint64_t lba, struct gpt *g)
{
    uint32_t hsize;

    g->sector = 512;
    ioctl(fd, BLKSSZGET, &g->sector); /* the table is in logical sectors: 4096 on some disks */
    if (pread(fd, g->hdr, sizeof(g->hdr), (off_t)(lba * (uint64_t)g->sector)) != (ssize_t)sizeof(g->hdr) ||
        memcmp(g->hdr, "EFI PART", 8))
        return -1;
    memcpy(&hsize, g->hdr + 12, 4);
    memcpy(&g->entries_lba, g->hdr + 72, 8);
    memcpy(&g->count, g->hdr + 80, 4);
    memcpy(&g->size, g->hdr + 84, 4);
    if (hsize < 92 || hsize > sizeof(g->hdr) || g->size < 128 || g->count == 0 || g->count > 4096)
        return -1;
    /* the header's own CRC: a table someone half wrote is not touched */
    uint32_t crc, want;
    memcpy(&want, g->hdr + 16, 4);
    memset(g->hdr + 16, 0, 4);
    crc = hib_crc32(0, g->hdr, hsize);
    memcpy(g->hdr + 16, &want, 4);
    if (crc != want)
        return -1;
    size_t bytes = (size_t)g->count * g->size;
    if (!(g->entries = malloc(bytes)))
        return -1;
    if (pread(fd, g->entries, bytes, (off_t)(g->entries_lba * (uint64_t)g->sector)) != (ssize_t)bytes) {
        free(g->entries);
        return -1;
    }
    memcpy(&want, g->hdr + 88, 4);
    if (hib_crc32(0, g->entries, bytes) != want) {
        free(g->entries);
        return -1;
    }
    return 0;
}

static int gpt_write(int fd, struct gpt *g)
{
    uint32_t hsize, crc;
    size_t bytes = (size_t)g->count * g->size;
    uint64_t lba;

    memcpy(&hsize, g->hdr + 12, 4);
    memcpy(&lba, g->hdr + 24, 8); /* this header's own place */
    crc = hib_crc32(0, g->entries, bytes);
    memcpy(g->hdr + 88, &crc, 4);
    memset(g->hdr + 16, 0, 4);
    crc = hib_crc32(0, g->hdr, hsize);
    memcpy(g->hdr + 16, &crc, 4);
    if (pwrite(fd, g->entries, bytes, (off_t)(g->entries_lba * (uint64_t)g->sector)) != (ssize_t)bytes ||
        pwrite(fd, g->hdr, (size_t)g->sector < sizeof(g->hdr) ? (size_t)g->sector : sizeof(g->hdr),
               (off_t)(lba * (uint64_t)g->sector)) < 0)
        return -1;
    return 0;
}

int hib_gpt_get_type(int fd, int num, uint8_t type[16])
{
    struct gpt g;

    if (gpt_read(fd, 1, &g))
        return -1;
    int ok = num >= 1 && (uint32_t)num <= g.count;
    if (ok)
        memcpy(type, g.entries + (size_t)(num - 1) * g.size, 16);
    free(g.entries);
    return ok ? 0 : -1;
}

int hib_gpt_set_type(int fd, int num, const uint8_t type[16])
{
    struct gpt primary, backup;
    uint64_t backup_lba;
    int rc = -1;

    if (gpt_read(fd, 1, &primary))
        return -1;
    memcpy(&backup_lba, primary.hdr + 32, 8);
    if (num < 1 || (uint32_t)num > primary.count) {
        free(primary.entries);
        return -1;
    }
    /* the backup first: should the power go in between, the primary still
     * holds the old table whole, and so does the one read at the next start */
    if (!gpt_read(fd, backup_lba, &backup)) {
        if ((uint32_t)num <= backup.count) {
            memcpy(backup.entries + (size_t)(num - 1) * backup.size, type, 16);
            if (gpt_write(fd, &backup) || fdatasync(fd))
                goto out;
        }
        free(backup.entries);
    }
    memcpy(primary.entries + (size_t)(num - 1) * primary.size, type, 16);
    if (!gpt_write(fd, &primary) && !fdatasync(fd))
        rc = 0;
    free(primary.entries);
    return rc;
out:
    free(backup.entries);
    free(primary.entries);
    return -1;
}

static int read_text(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

int hib_partition_disk(const char *part, char *disk, size_t len, int *num, uint64_t *start)
{
    char path[PATH_MAX], real[PATH_MAX], value[32];

    snprintf(path, sizeof(path), "/sys/class/block/%s/partition", part);
    if (read_text(path, value, sizeof(value)))
        return -1;
    *num = atoi(value);
    snprintf(path, sizeof(path), "/sys/class/block/%s/start", part);
    if (read_text(path, value, sizeof(value)))
        return -1;
    *start = strtoull(value, NULL, 10) * 512; /* sysfs counts 512-byte sectors */
    snprintf(path, sizeof(path), "/sys/class/block/%s", part);
    if (!realpath(path, real) || !strrchr(real, '/'))
        return -1;
    *strrchr(real, '/') = 0;
    snprintf(disk, len, "%s", strrchr(real, '/') + 1);
    return 0;
}

/* --- the EFI variable --- */

#define EFIVARS "/sys/firmware/efi/efivars"
#define HIB_VAR EFIVARS "/ArcticHiberfil-7a5e5b3c-1d2e-4f60-8a9b-415243544856"

int hib_efi_mount(void)
{
    char line[512];
    FILE *f;

    if (access("/sys/firmware/efi", F_OK))
        return -1; /* BIOS: no hibernation */
    if ((f = fopen("/proc/mounts", "re"))) {
        while (fgets(line, sizeof(line), f))
            if (strstr(line, " " EFIVARS " efivarfs ")) {
                fclose(f);
                return 0;
            }
        fclose(f);
    }
    return mount("efivarfs", EFIVARS, "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

int hib_efi_read(struct hib_where *w)
{
    uint8_t buf[4 + sizeof(*w)];
    int fd = open(HIB_VAR, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != (ssize_t)sizeof(buf))
        return -1;
    memcpy(w, buf + 4, sizeof(*w));
    w->partuuid[sizeof(w->partuuid) - 1] = 0;
    w->disk[sizeof(w->disk) - 1] = 0;
    return 0;
}

int hib_efi_write(const struct hib_where *w)
{
    struct hib_where old;
    uint8_t buf[4 + sizeof(*w)];
    uint32_t attrs = 0x7; /* non-volatile, boot and runtime access */
    int fd, flags;

    if (!hib_efi_read(&old) && !memcmp(&old, w, sizeof(old)))
        return 0; /* NVRAM is written as seldom as can be */
    /* efivarfs makes its files immutable: that goes first */
    if ((fd = open(HIB_VAR, O_RDONLY | O_CLOEXEC)) >= 0) {
        if (!ioctl(fd, FS_IOC_GETFLAGS, &flags)) {
            flags &= ~FS_IMMUTABLE_FL;
            ioctl(fd, FS_IOC_SETFLAGS, &flags);
        }
        close(fd);
    }
    memcpy(buf, &attrs, 4);
    memcpy(buf + 4, w, sizeof(*w));
    if ((fd = open(HIB_VAR, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        return -1;
    int ok = write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
    close(fd);
    return ok ? 0 : -1;
}
