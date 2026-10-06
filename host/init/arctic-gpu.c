/* arctic-gpu: udev runs it for every display adapter on PCI, after its own
 * module autoloading has had its go, and finishes choosing the driver the way
 * Windows would pick the right one without asking.
 *
 * NVIDIA: the open NVIDIA module is loaded by its alias and takes Turing and
 * newer cards. It declines older ones: Maxwell, Pascal and Volta then get
 * NVIDIA's proprietary modules of the same version, which wait outside the
 * module tree in /usr/lib/arctic/nvidia-legacy (the open ones are taken out
 * first, their names being the same); Kepler and older, or a card those
 * modules decline too, get nouveau (blacklisted for autoloading, so it never
 * races NVIDIA for a new card). nouveau cannot raise the clocks of these
 * cards, NVIDIA's driver can. nvidia_drm, the part that shows a picture, has
 * no alias of its own.
 * Radeon HD 7000 / R9 200 (GCN 1.0, 1.1) are sent to amdgpu by the options in
 * /etc/modprobe.d/arctic-gpu.conf; nothing to do for them here.
 *
 * What was chosen is written to the kernel log (kernel.log). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LEGACY_DIR "/usr/lib/arctic/nvidia-legacy"
#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

static void say(const char *fmt, ...)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "arctic-gpu: ");
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    va_list ap;

    va_start(ap, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - len - 1, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(buf) - 2)
        len = sizeof(buf) - 2;
    buf[len++] = '\n';
    if (fd >= 0) {
        write(fd, buf, len);
        close(fd);
    }
}

static unsigned int read_hex(const char *dir, const char *file)
{
    char path[PATH_MAX], text[32] = "";
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (!(f = fopen(path, "r")))
        return 0;
    if (!fgets(text, sizeof(text), f))
        text[0] = 0;
    fclose(f);
    return strtoul(text, NULL, 16);
}

/* the name of the driver bound to the device, "" if none */
static const char *driver(const char *dir)
{
    static char name[64];
    char path[PATH_MAX], target[PATH_MAX];
    ssize_t len;

    snprintf(path, sizeof(path), "%s/driver", dir);
    if ((len = readlink(path, target, sizeof(target) - 1)) <= 0)
        return "";
    target[len] = 0;
    snprintf(name, sizeof(name), "%.63s", strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
    return name;
}

static int modprobe(const char *module)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        /* by name, so a blacklist (which is for aliases) does not stop it */
        execl("/usr/bin/modprobe", "modprobe", module, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* the output of a program, its first line */
static int run_line(char *out, size_t size, const char *prog, char *const argv[])
{
    int fds[2], status;
    ssize_t len;
    pid_t pid;

    if (pipe(fds))
        return -1;
    if (!(pid = fork())) {
        dup2(fds[1], 1);
        close(fds[0]);
        execv(prog, argv);
        _exit(127);
    }
    close(fds[1]);
    len = pid > 0 ? read(fds[0], out, size - 1) : -1;
    close(fds[0]);
    if (pid > 0)
        waitpid(pid, &status, 0);
    out[len > 0 ? len : 0] = 0;
    out[strcspn(out, "\n")] = 0;
    return len > 0 ? 0 : -1;
}

/* NVIDIA's proprietary driver of the 580 branch drives Maxwell (GM107 on),
 * Pascal and Volta; Turing (0x1e00) and newer are the open module's */
static int legacy_nvidia(unsigned int device)
{
    return device >= 0x1340 && device < 0x1e00;
}

/* one of the proprietary modules, its dependencies (modinfo) loaded first */
static int load_legacy(const char *name, const char *params)
{
    char path[PATH_MAX], deps[512], *dep, *save;
    int fd, flags = 0, ret;

    snprintf(path, sizeof(path), LEGACY_DIR "/%s.ko", name);
    if (access(path, R_OK)) {
        snprintf(path, sizeof(path), LEGACY_DIR "/%s.ko.zst", name);
        flags = MODULE_INIT_COMPRESSED_FILE;
    }
    {
        char *const argv[] = {"modinfo", "-F", "depends", path, NULL};
        if (!run_line(deps, sizeof(deps), "/usr/bin/modinfo", argv))
            for (dep = strtok_r(deps, ",", &save); dep; dep = strtok_r(NULL, ",", &save))
                if (strncmp(dep, "nvidia", 6))
                    modprobe(dep);
    }
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    ret = syscall(SYS_finit_module, fd, params, flags);
    close(fd);
    if (ret)
        say("%s: %m", path);
    return ret;
}

/* the open modules out (they declined the card), the proprietary ones in */
static int load_legacy_driver(void)
{
    static const char *const open_modules[] = {"nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia"};
    static const char *const nodes[] = {"/dev/nvidia-uvm", "/dev/nvidia-uvm-tools", "/dev/nvidia-modeset",
                                        "/dev/nvidiactl", "/dev/nvidia0"};

    for (unsigned int i = 0; i < sizeof(open_modules) / sizeof(open_modules[0]); i++)
        syscall(SYS_delete_module, open_modules[i], O_NONBLOCK);
    /* nvidia-modprobe makes them again for the driver that stays (60-nvidia.rules) */
    for (unsigned int i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
        unlink(nodes[i]);
    if (load_legacy("nvidia", ""))
        return -1;
    if (load_legacy("nvidia-modeset", ""))
        return -1;
    /* uvm before drm: nvidia-drm's arrival runs nvidia-modprobe -u, which
     * would modprobe the open nvidia-uvm against this nvidia otherwise */
    load_legacy("nvidia-uvm", "");
    load_legacy("nvidia-drm", "modeset=1 fbdev=1");
    return 0;
}

int main(void)
{
    const char *devpath = getenv("DEVPATH");
    char dir[PATH_MAX];
    unsigned int vendor, device;

    if (!devpath)
        return 1;
    snprintf(dir, sizeof(dir), "/sys%s", devpath);
    vendor = read_hex(dir, "vendor");
    device = read_hex(dir, "device");

    if (vendor == 0x10de) {
        int legacy = 0;

        if (!*driver(dir) && legacy_nvidia(device) && !access(LEGACY_DIR, F_OK)) {
            if (!load_legacy_driver() && !strcmp(driver(dir), "nvidia"))
                legacy = 1;
            else
                say("%04x:%04x: NVIDIA's proprietary driver did not take it", vendor, device);
        }
        if (!*driver(dir) && modprobe("nouveau"))
            say("%04x:%04x: nouveau did not load", vendor, device);
        if (!legacy && !strcmp(driver(dir), "nvidia") && modprobe("nvidia_drm"))
            say("%04x:%04x: nvidia_drm did not load", vendor, device);
        if (legacy)
            say("%04x:%04x: NVIDIA's proprietary driver (Maxwell, Pascal, Volta)", vendor, device);
    }

    if (*driver(dir))
        say("%04x:%04x %s: %s", vendor, device, strrchr(devpath, '/') + 1, driver(dir));
    else
        say("%04x:%04x %s: no driver, the firmware's framebuffer stays", vendor, device,
            strrchr(devpath, '/') + 1);
    return 0;
}
