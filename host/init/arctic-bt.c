/* arctic-bt: what the Bluetooth driver of the Windows side (winebth.sys,
 * which runs as the unprivileged user nt) needs root for. The driver itself
 * talks to the controller's kernel driver; this only hands it the handles
 * the kernel gives root alone (docs/bluetooth.md). One request a connection
 * on /run/arctic/bt.sock, the answer one word, with a descriptor:
 *
 *   mgmt          the management channel of every controller (HCI_CHANNEL_CONTROL),
 *                 bound here so that the kernel trusts it with all its commands
 *   listen PSM    an L2CAP socket bound to a PSM below 0x1001 (SDP 1, AVCTP 23,
 *                 AVDTP 25, AVCTP browsing 27), which needs CAP_NET_BIND_SERVICE;
 *                 the driver listens and accepts on it itself
 *   unblock       Bluetooth's soft block off (rfkill), when the radio is switched on
 *
 * Answers: ok (with the descriptor), error. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/arctic/bt.sock"
#define NT_USER "nt"

/* the kernel's (include/net/bluetooth): not in the C library's headers */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH        31
#endif
#define BTPROTO_L2CAP       0
#define BTPROTO_HCI         1
#define HCI_DEV_NONE        0xffff
#define HCI_CHANNEL_CONTROL 3
#define BDADDR_BREDR        0

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct sockaddr_l2 {
    sa_family_t l2_family;
    uint16_t l2_psm;
    uint8_t l2_bdaddr[6];
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
};

/* rfkill (linux/rfkill.h) */
struct rfkill_event {
    uint32_t idx;
    uint8_t type;
    uint8_t op;
    uint8_t soft;
    uint8_t hard;
};
#define RFKILL_TYPE_BLUETOOTH 2
#define RFKILL_OP_CHANGE_ALL  3

static void say(const char *fmt, ...)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "arctic-bt: ");
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

static int open_mgmt(void)
{
    struct sockaddr_hci addr = {.hci_family = AF_BLUETOOTH, .hci_dev = HCI_DEV_NONE,
                                .hci_channel = HCI_CHANNEL_CONTROL};
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);

    if (fd < 0) {
        say("management socket: %s", strerror(errno));
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        say("management channel: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int open_l2cap(unsigned int psm)
{
    struct sockaddr_l2 addr = {.l2_family = AF_BLUETOOTH, .l2_psm = (uint16_t)psm, .l2_bdaddr_type = BDADDR_BREDR};
    int fd;

    /* the PSMs of the profiles Windows serves, nothing else */
    if (psm != 1 && psm != 23 && psm != 25 && psm != 27)
        return -1;
    if ((fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP)) < 0) {
        say("L2CAP socket: %s", strerror(errno));
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        say("PSM %u: %s", psm, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int unblock(void)
{
    struct rfkill_event event = {.type = RFKILL_TYPE_BLUETOOTH, .op = RFKILL_OP_CHANGE_ALL, .soft = 0};
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    ssize_t len;

    if (fd < 0)
        return -1;
    len = write(fd, &event, sizeof(event));
    close(fd);
    return len == sizeof(event) ? 0 : -1;
}

static void answer(int client, int fd)
{
    char word[] = "ok";
    struct iovec iov = {.iov_base = word, .iov_len = 2};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};

    if (fd >= 0) {
        struct cmsghdr *cmsg;

        memset(&control, 0, sizeof(control));
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof(control.buf);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    sendmsg(client, &msg, MSG_NOSIGNAL);
}

static void serve(int client)
{
    char request[64];
    ssize_t len = read(client, request, sizeof(request) - 1);
    unsigned int psm;
    int fd = -1;

    if (len <= 0)
        return;
    request[len] = 0;
    request[strcspn(request, "\r\n")] = 0;
    if (!strcmp(request, "mgmt"))
        fd = open_mgmt();
    else if (sscanf(request, "listen %u", &psm) == 1)
        fd = open_l2cap(psm);
    else if (!strcmp(request, "unblock")) {
        if (!unblock()) {
            answer(client, -1);
            return;
        }
    }
    if (fd < 0) {
        write(client, "error", 5);
        return;
    }
    answer(client, fd);
    close(fd);
}

int main(void)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET_PATH};
    struct passwd *pw = getpwnam(NT_USER);
    int fd;

    signal(SIGPIPE, SIG_IGN);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return 1;
    unlink(SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 16)) {
        say("%s: %s", SOCKET_PATH, strerror(errno));
        return 1;
    }
    /* the Windows world's alone */
    chmod(SOCKET_PATH, 0600);
    if (pw)
        chown(SOCKET_PATH, pw->pw_uid, pw->pw_gid);

    for (;;) {
        int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

        if (client < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        serve(client);
        close(client);
    }
}
