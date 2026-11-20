/* The floor under a wineserver round trip: what the Linux kernel of this PC
 * charges for a bare system call and for one thread waking another, with no
 * Wine at all. Runs in a WSL container (same kernel as the host).
 *
 * gcc -O2 -pthread -o ipcfloor tests/tools/ipcfloor.c */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

#define N 200000
static int req[2], rep[2], sock[2];
static atomic_int word;
static int spin;

static void *pipe_server(void *arg)
{
    char c;
    for (int i = 0; i < N; i++) {
        if (read(req[0], &c, 1) != 1 || write(rep[1], &c, 1) != 1)
            break;
    }
    return NULL;
}

static void *sock_server(void *arg)
{
    char buf[64];
    for (int i = 0; i < N; i++) {
        if (recv(sock[1], buf, 64, 0) <= 0 || send(sock[1], buf, 64, 0) <= 0)
            break;
    }
    return NULL;
}

static void futex_wait(atomic_int *w, int val)
{
    if (spin)
        for (int i = 0; i < 4000 && atomic_load(w) == val; i++)
            __builtin_ia32_pause();
    while (atomic_load(w) == val)
        syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static void futex_wake(atomic_int *w)
{
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void *futex_server(void *arg)
{
    for (int i = 0; i < N; i++) {
        futex_wait(&word, 2 * i);       /* request i is word = 2i+1 */
        atomic_store(&word, 2 * i + 2); /* reply */
        futex_wake(&word);
    }
    return NULL;
}

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void run(const char *name, void *(*server)(void *), void (*client)(int), int pin_cpus)
{
    pthread_t t;
    atomic_store(&word, 0);
    if (pin_cpus)
        pin(0);
    pthread_create(&t, NULL, server, (void *)(long)pin_cpus);
    double t0 = now_us();
    for (int i = 0; i < N; i++)
        client(i);
    double dt = now_us() - t0;
    pthread_join(t, NULL);
    printf("%-44s %7.2f us\n", name, dt / N);
    if (pin_cpus) {
        cpu_set_t all;
        CPU_ZERO(&all);
        for (int c = 0; c < 64; c++)
            CPU_SET(c, &all);
        pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
    }
}

static void pipe_client(int i)
{
    char c = 1;
    if (write(req[1], &c, 1) != 1 || read(rep[0], &c, 1) != 1)
        exit(1);
}

static void sock_client(int i)
{
    char buf[64] = {0};
    if (send(sock[0], buf, 64, 0) <= 0 || recv(sock[0], buf, 64, 0) <= 0)
        exit(1);
}

static void futex_client(int i)
{
    atomic_store(&word, 2 * i + 1);
    futex_wake(&word);
    futex_wait(&word, 2 * i + 1);
}

static void *pipe_server_pinned(void *a) { pin(1); return pipe_server(a); }
static void *futex_server_pinned(void *a) { pin(1); return futex_server(a); }

int main(void)
{
    double t0 = now_us();
    for (int i = 0; i < N * 5; i++)
        syscall(SYS_getppid);
    printf("%-44s %7.2f us\n", "bare system call (getppid)", (now_us() - t0) / (N * 5));

    pipe(req);
    pipe(rep);
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock);
    run("pipe round trip (as wineserver), any CPU", pipe_server, pipe_client, 0);
    run("pipe round trip, two fixed CPUs", pipe_server_pinned, pipe_client, 1);
    run("unix socket round trip, any CPU", sock_server, sock_client, 0);
    run("futex round trip, any CPU", futex_server, futex_client, 0);
    run("futex round trip, two fixed CPUs", futex_server_pinned, futex_client, 1);
    spin = 1;
    run("futex round trip, spin 4000 before sleeping", futex_server, futex_client, 0);
    return 0;
}
