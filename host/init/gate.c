/* host/init/gate.c: the Linux gate's password (docs/linux-gate.md, "Де живе
 * пароль"). gate.conf holds password=on|off, the yescrypt hash (libxcrypt)
 * of the password, salted (the password itself is never stored), and
 * distro=, the WSL distribution that is the gate to the host. */
#define _GNU_SOURCE
#include "gate.h"
#include <crypt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define C_DRIVE "/mnt/c"
#define HOST_STATE C_DRIVE "/Windows/System32/config/Host"
#define GATE_REAL_DIR HOST_STATE "/Gate"
#define GATE_CONF GATE_STORE "/gate.conf"
#define GATE_DEFAULT_PASSWORD "0000"
#define GATE_DEFAULT_DISTRO "arctic-build"

int gate_prepare_state_dir(void)
{
    struct stat st;

    mkdir(HOST_STATE, 0755);
    mkdir(GATE_REAL_DIR, 0700);
    mkdir("/run/arctic", 0755);
    mkdir(GATE_RUN_DIR, 0700);
    /* root's folder on tmpfs, whatever was there before: the one thing
     * between the Windows side and the store */
    if (lstat(GATE_RUN_DIR, &st) || !S_ISDIR(st.st_mode) || chown(GATE_RUN_DIR, 0, 0) || chmod(GATE_RUN_DIR, 0700))
        return -1;
    mkdir(GATE_STORE, 0700);
    if (mount(GATE_REAL_DIR, GATE_STORE, NULL, MS_BIND, NULL))
        return -1;
    /* the original path, covered: still listable (empty), never writable */
    if (mount("tmpfs", GATE_REAL_DIR, "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0555"))
        return -1;
    return 0;
}

int gate_set_password(struct gate_state *st, const char *new_password)
{
    /* libxcrypt wants the whole of its output size here, not as much as a
     * setting takes: with less it returns ERANGE and no hash is made at all,
     * and then every password is wrong (found in a VM 2026-10-12) */
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    unsigned char rbytes[16];
    struct crypt_data data;
    char *result;

    if (getrandom(rbytes, sizeof(rbytes), 0) != (ssize_t)sizeof(rbytes))
        return -1;
    if (!crypt_gensalt_rn("$y$", 0, (const char *)rbytes, sizeof(rbytes), setting, sizeof(setting)))
        return -1;
    memset(&data, 0, sizeof(data));
    result = crypt_rn(new_password, setting, &data, sizeof(data));
    if (!result) {
        memset(&data, 0, sizeof(data));
        return -1;
    }
    snprintf(st->hash, sizeof(st->hash), "%s", result);
    memset(&data, 0, sizeof(data));
    return 0;
}

int gate_check_password(const struct gate_state *st, const char *attempt)
{
    struct crypt_data data;
    char *result;
    int ok;

    if (!st->hash[0])
        return 0;
    memset(&data, 0, sizeof(data));
    result = crypt_rn(attempt, st->hash, &data, sizeof(data));
    ok = result && !strcmp(result, st->hash);
    memset(&data, 0, sizeof(data));
    return ok;
}

void gate_load(struct gate_state *st)
{
    char line[512];
    FILE *f = fopen(GATE_CONF, "re");

    st->password_on = 1;
    st->hash[0] = 0;
    snprintf(st->distro, sizeof(st->distro), "%s", GATE_DEFAULT_DISTRO);
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "password=", 9))
                st->password_on = strcmp(line + 9, "off") != 0;
            else if (!strncmp(line, "hash=", 5))
                snprintf(st->hash, sizeof(st->hash), "%s", line + 5);
            else if (!strncmp(line, "distro=", 7) && line[7])
                snprintf(st->distro, sizeof(st->distro), "%s", line + 7);
        }
        fclose(f);
    }
    /* no file (first start, new install), or one without a usable hash */
    if (!st->hash[0]) {
        gate_set_password(st, GATE_DEFAULT_PASSWORD);
        gate_save(st);
    }
}

int gate_save(const struct gate_state *st)
{
    char tmp[PATH_MAX];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.new", GATE_CONF);
    if (!(f = fopen(tmp, "we")))
        return -1;
    fprintf(f, "password=%s\nhash=%s\ndistro=%s\n", st->password_on ? "on" : "off", st->hash, st->distro);
    fclose(f);
    return rename(tmp, GATE_CONF) ? -1 : 0;
}
