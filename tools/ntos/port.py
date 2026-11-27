#!/usr/bin/env python3
"""The mechanical part of bringing Wine's server into ntos (docs/ntos.md).

port.py <server source dir> <kernel/drivers/ntos>

Each file of the server goes to the component of ntoskrnl it is in Windows
(ob/, ke/, io/, ...); USER is win32k's already and stays out. The code keeps
Wine's style and layout, so that a later Wine can be compared with it and
brought over again; only what clashes with the kernel is renamed:

- the server's struct names the kernel has too (struct file, fd, key, mutex,
  ...) get nt_ in front;
- the C library calls the server makes (open, read, kill, fprintf, ...) go to
  ntos's own, nt_*, over the kernel's files and tasks (crt.c);
- current, the thread being served, is nt_current; current_time and
  monotonic_time are nt_current_time and nt_monotonic_time.

A file already there is only overwritten with --force: what is done by hand
after this stays.
"""
import os
import re
import sys

# file of the server -> where it goes; None: not ntos's
COMPONENTS = {
    # Ob: objects, handles, the namespace
    "object.c": "ob", "handle.c": "ob", "directory.c": "ob", "symlink.c": "ob",
    # Ke: what can be waited for
    "event.c": "ke", "mutex.c": "ke", "semaphore.c": "ke", "timer.c": "ke",
    "completion.c": "ke", "inproc_sync.c": "ke",
    # Io: files, asynchronous I/O, change notification, pipes, devices
    "fd.c": "io", "file.c": "io", "async.c": "io", "change.c": "io",
    "named_pipe.c": "io", "mailslot.c": "io", "serial.c": "io", "device.c": "io",
    # afd.sys
    "sock.c": "afd",
    # Cm
    "registry.c": "cm",
    # Ps: processes, threads, their contexts and memory
    "process.c": "ps", "thread.c": "ps", "ptrace.c": "ps", "procfs.c": "ps",
    # Se
    "token.c": "se",
    # Mm
    "mapping.c": "mm",
    # dxgkrnl
    "d3dkmt.c": "dxg",
    # Dbgk
    "debugger.c": "dbgk",
    # Lpc
    "alpc.c": "lpc",
    # condrv
    "console.c": "cd",
    # Ex: window stations and desktops as objects, the helpers
    "winstation.c": "ex", "unicode.c": "ex",
    # what the kernel does instead: the request loop, the main, the signals
    "request.c": None, "main.c": None, "signal.c": None, "mach.c": None,
    "trace.c": None, "win32k.c": None,
    # USER: win32k's
    "window.c": None, "queue.c": None, "hook.c": None, "class.c": None,
    "atom.c": None, "clipboard.c": None, "user.c": None, "region.c": None,
}

HEADERS = ["object.h", "handle.h", "file.h", "process.h", "thread.h", "request.h",
           "security.h", "unicode.h", "user.h"]

# struct names of the kernel's too
STRUCTS = ["file", "fd", "key", "sock", "inode", "file_lock", "mutex", "semaphore",
           "completion", "device", "console", "stat", "wait_queue_entry", "debug_obj", "range"]

# functions of the server the kernel has under the same name
FUNCS = ["sock_error", "get_device", "key_init", "mutex_init", "mutex_destroy", "mutex_release", "device_destroy", "cpu_set_t",
         "getrlimit", "setrlimit", "getenv", "realpath", "asprintf", "tcgetattr", "tcsetattr"]

# the C library: name -> ntos's (crt.h)
CALLS = ["open", "openat", "close", "read", "write", "pread", "pwrite", "lseek", "fstat",
         "stat", "lstat", "fstatat", "fcntl", "ioctl", "poll", "dup", "dup2", "unlink",
         "unlinkat", "rename", "renameat", "mkdir", "mkdirat", "rmdir", "fsync", "fdatasync",
         "ftruncate", "fchdir", "chdir", "getcwd", "readlink", "readlinkat", "socket",
         "socketpair", "bind", "listen", "accept", "accept4", "connect", "getsockopt",
         "setsockopt", "getsockname", "getpeername", "shutdown", "recv", "send", "recvfrom",
         "sendto", "recvmsg", "sendmsg", "kill", "tgkill", "getpid", "getppid", "getuid",
         "geteuid", "getgid", "getegid", "pipe", "pipe2", "fopen", "fdopen", "fclose",
         "fprintf", "vfprintf", "fputs", "fputc", "fwrite", "fread", "fgets", "fflush",
         "fileno", "ftell", "fseek", "rewind", "feof", "ferror", "getc", "fgetc", "ungetc",
         "printf", "perror", "abort", "exit", "_exit", "atexit", "sysconf", "statfs",
         "fstatfs", "flock", "time", "gettimeofday", "clock_gettime", "nanosleep", "usleep",
         "sched_setscheduler", "sched_getscheduler", "sched_setaffinity", "setpriority",
         "getpriority", "sigaction", "sigemptyset", "sigaddset", "sigprocmask", "signal",
         "memfd_create", "mmap", "munmap", "mprotect", "inotify_init", "inotify_init1",
         "inotify_add_watch", "inotify_rm_watch", "ptrace", "waitpid", "fork", "execv",
         "strerror", "random", "rand", "srand", "qsort", "bsearch", "strtoul", "strtol",
         "strtoull", "atoi", "realloc", "calloc", "malloc", "free", "strdup", "opendir",
         "readdir", "closedir", "fdopendir", "dirfd", "isatty", "umask", "fchmod", "chmod",
         "fchown", "futimens", "utimensat", "posix_fallocate", "fallocate"]

SERVER_INCLUDE = re.compile(r'^#\s*include [<"][^>"]+[>"][^\n]*\n', re.M)


def port_text(text, name):
    text = "// SPDX-License-Identifier: LGPL-2.1-or-later\n" + text
    first = [True]

    def include(m):
        if first[0]:
            first[0] = False
            return '#include "ntos.h"  /* ntos: all that the server\'s headers and the C library gave */\n'
        return ""

    if name.endswith(".c"):
        text = SERVER_INCLUDE.sub(include, text)
    else:
        text = SERVER_INCLUDE.sub("", text)
    # the thread being served; the kernel's current is the task. Fields of
    # that name in the protocol's structures (reply->current) stay.
    text = re.sub(r"(?<![\w.>])current\b(?!_)", "nt_current", text)
    # the protocol's thread_info: the kernel has one too (shim/wine_headers.h)
    text = re.sub(r"\bstruct thread_info\b", "struct nt_thread_info", text)
    text = re.sub(r"\bcurrent_time\b", "nt_current_time", text)
    text = re.sub(r"\bmonotonic_time\b", "nt_monotonic_time", text)
    for s in STRUCTS:
        text = re.sub(r"\bstruct %s\b" % s, "struct nt_%s" % s, text)
    for f in FUNCS:
        text = re.sub(r"(?<![\w.>])%s\b" % f, "nt_" + f, text)
    # calls, not members: nothing of . -> or a word just before
    for c in CALLS:
        text = re.sub(r"(?<![\w.>])%s(?=\s*\()" % c, "nt_" + c, text)
    # Wine's list.h against the kernel's list.h
    for f in ("list_add_tail", "list_add_head", "list_empty", "list_move_tail", "list_move_head",
              "list_init", "list_remove", "list_head", "list_tail", "list_next", "list_prev",
              "list_count", "list_add_after", "list_add_before", "list_move_after", "list_move_before",
              "list_move_slice_head", "list_move_slice_tail"):
        text = re.sub(r"\b%s\(" % f, "w" + f + "(", text)
    text = re.sub(r"/\* *fall ?through *\*/", "fallthrough;", text)
    text = re.sub(r"\bwake_up\(", "nt_wake_up(", text)
    # the kernel's receive_fd (linux/file.h)
    text = re.sub(r"\breceive_fd\b", "nt_receive_client_fd", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    force = "--force" in sys.argv
    src, dst = args
    for name in sorted(os.listdir(src)):
        if name in HEADERS:
            out = os.path.join(dst, name)
        elif name.endswith(".c") and name in COMPONENTS:
            comp = COMPONENTS[name]
            if comp is None:
                continue
            out = os.path.join(dst, comp, name)
        else:
            continue
        if os.path.exists(out) and not force:
            print("kept   %s" % out)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        text = open(os.path.join(src, name), encoding="utf-8-sig").read()
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(port_text(text, name))
        print("%s -> %s" % (name, out))
    unknown = [n for n in os.listdir(src) if n.endswith(".c") and n not in COMPONENTS]
    if unknown:
        print("NOT MAPPED:", " ".join(unknown))


if __name__ == "__main__":
    main()
