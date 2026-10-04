#!/usr/bin/env python3
"""wsl.exe for the Linux side of Arctic: a command in another WSL distribution.

Inside Arctic, Claude Code's Bash runs in arctic-build, and Linux cannot start
wsl.exe there. arctic-lxss listens on 127.0.0.1 and a distribution shares the
host's network, so this speaks its protocol (host/init/arctic-lxss.c) itself:

    python3 tools/arctic/wsl.py -d arctic-ros -u root -- bash tools/wsl/build-reactos.sh

It runs in the folder it is started from (the drives are /c, /d... in every
distribution) and exits with the command's exit code.
"""
import argparse
import os
import socket
import struct
import sys
import threading

MAGIC = b"ARCTLXS1"
F_STDIN, F_STDIN_EOF, F_STDOUT, F_STDERR, F_EXIT, F_ERROR, F_SIGNAL = range(7)
REQ_GITBASH = 2
SESSION = ["/c/ProgramData/Arctic/Lxss/session", "/mnt/c/ProgramData/Arctic/Lxss/session"]


def string(s):
    data = s.encode()
    return struct.pack("<I", len(data)) + data


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-d", "--distribution", default="")
    parser.add_argument("-u", "--user", default="")
    parser.add_argument("--cd", default=os.getcwd())
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    session = next((p for p in SESSION if os.path.exists(p)), None)
    if not session:
        sys.exit("wsl.py: no C:\\ProgramData\\Arctic\\Lxss\\session: not in Arctic, or arctic-lxss is not running")
    port, token = open(session).read().split()
    sock = socket.create_connection(("127.0.0.1", int(port)))

    env = [f"{k}={v}" for k, v in os.environ.items()
           if k not in ("PATH", "HOME", "USER", "LOGNAME", "SHELL", "PWD", "OLDPWD", "SHLVL", "_", "WSL_DISTRO_NAME")]
    request = MAGIC + token.encode() + struct.pack("<III", REQ_GITBASH, len(command), len(env))
    request += string(args.distribution) + string(args.user) + string(args.cd)
    request += b"".join(string(a) for a in command) + b"".join(string(e) for e in env)
    sock.sendall(request)

    def pump_stdin():
        try:
            while True:
                data = os.read(0, 65536)
                if not data:
                    break
                sock.sendall(struct.pack("<BI", F_STDIN, len(data)) + data)
        except OSError:
            pass
        try:
            sock.sendall(struct.pack("<BI", F_STDIN_EOF, 0))
        except OSError:
            pass

    threading.Thread(target=pump_stdin, daemon=True).start()

    def recv_all(n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    while True:
        head = recv_all(5)
        if head is None:
            sys.exit("wsl.py: the connection to arctic-lxss was lost")
        kind, length = struct.unpack("<BI", head)
        data = recv_all(length) if length else b""
        if kind == F_STDOUT:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        elif kind == F_STDERR:
            sys.stderr.buffer.write(data)
            sys.stderr.buffer.flush()
        elif kind == F_ERROR:
            sys.exit("wsl.py: " + data.decode(errors="replace"))
        elif kind == F_EXIT:
            os._exit(struct.unpack("<I", data[:4])[0] & 0xFF)


if __name__ == "__main__":
    main()
