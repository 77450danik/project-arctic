/* arctic-gpu: udev runs it for every display adapter on PCI, after its own
 * module autoloading has had its go, and finishes choosing the driver the way
 * Windows would pick the right one without asking.
 *
 * NVIDIA: the open NVIDIA module is loaded by its alias and takes Turing and
 * newer cards; it declines older ones, which then get nouveau (blacklisted
 * for autoloading by nvidia-utils, so it never races NVIDIA for a new card).
 * nvidia_drm, the part that shows a picture, has no alias of its own.
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
#include <sys/wait.h>
#include <unistd.h>

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
    snprintf(name, sizeof(name), "%s", strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
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
        if (!*driver(dir) && modprobe("nouveau"))
            say("%04x:%04x: nouveau did not load", vendor, device);
        if (!strcmp(driver(dir), "nvidia") && modprobe("nvidia_drm"))
            say("%04x:%04x: nvidia_drm did not load", vendor, device);
    }

    if (*driver(dir))
        say("%04x:%04x %s: %s", vendor, device, strrchr(devpath, '/') + 1, driver(dir));
    else
        say("%04x:%04x %s: no driver, the firmware's framebuffer stays", vendor, device,
            strrchr(devpath, '/') + 1);
    return 0;
}
