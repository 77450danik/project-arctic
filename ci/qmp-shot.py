"""Takes a QEMU screenshot over QMP and saves it as PNG (standard library only).

usage: qmp-shot.py <qmp socket> <tmp.ppm> <out.png> [monitor]
       qmp-shot.py --convert <in.ppm> <out.png>

The monitor is the head of the card, 0 by default; a machine with two of
them has a second screen to take a picture of.
"""
import json
import socket
import struct
import sys
import time
import zlib


def qmp(address, ppm_path, head=0):
    # "tcp:HOST:PORT" (QEMU on Windows) or a unix socket path
    if address.startswith("tcp:"):
        host, port = address[4:].rsplit(":", 1)
        sock = socket.create_connection((host, int(port)))
    else:
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(address)
    stream = sock.makefile("rw")
    stream.readline()  # greeting

    def call(command, arguments=None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        stream.write(json.dumps(request) + "\n")
        stream.flush()
        while True:
            reply = json.loads(stream.readline())
            if "return" in reply or "error" in reply:
                return reply

    call("qmp_capabilities")
    arguments = {"filename": ppm_path}
    if head:
        arguments["head"] = head
    call("screendump", arguments)
    sock.close()


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos:pos + width * height * 3]
    raw = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(png_path, "wb").write(png)


if __name__ == "__main__":
    if sys.argv[1] == "--convert":  # a screendump taken elsewhere
        ppm_to_png(sys.argv[2], sys.argv[3])
    else:
        qmp(sys.argv[1], sys.argv[2], int(sys.argv[4]) if len(sys.argv) > 4 else 0)
        time.sleep(1)
        ppm_to_png(sys.argv[2], sys.argv[3])
