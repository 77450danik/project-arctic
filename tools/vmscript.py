"""Drives a running QEMU over QMP: pointer, buttons, keys and screenshots.

Steps, one per line ('#' starts a comment):
    move X Y              pointer to screen pixel X, Y (needs the usb-tablet)
    click [left|right|middle]
    dblclick              two left clicks in a row
    down [BUTTON] / up [BUTTON]
    drag X1 Y1 X2 Y2      left button from one point to the other
    type TEXT             keys for the letters of TEXT (US layout)
    key COMBO             e.g. ret, alt-f4, ctrl-shift-esc, alt-shift
    sleep SECONDS
    shot NAME             screenshot to out/test-local/NAME.png
    plug NAME IMAGE       a USB stick with that raw image (relative to out/test-local)
    unplug NAME           pull it out
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "out", "test-local")

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', ':': 'semicolon',
           '"': 'apostrophe', '~': 'grave_accent', '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash'}
PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right', ';': 'semicolon',
         "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash', ',': 'comma', '.': 'dot', '/': 'slash',
         '\n': 'ret', '\t': 'tab'}
ALIASES = {'ctrl': 'ctrl', 'alt': 'alt', 'shift': 'shift', 'win': 'meta_l', 'enter': 'ret', 'del': 'delete'}


class Qmp:
    def __init__(self, address="tcp:127.0.0.1:4455"):
        host, port = address[4:].rsplit(":", 1)
        for _ in range(50):
            try:
                self.sock = socket.create_connection((host, int(port)))
                break
            except OSError:
                time.sleep(0.2)
        self.stream = self.sock.makefile("rw")
        self.stream.readline()
        self.call("qmp_capabilities")
        self.width, self.height = 1280, 800

    def call(self, command, arguments=None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request) + "\n")
        self.stream.flush()
        while True:
            reply = json.loads(self.stream.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(f"{command}: {reply['error']}")

    def events(self, *events):
        self.call("input-send-event", {"events": list(events)})

    def move(self, x, y):
        self.events({"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (self.width - 1))}},
                    {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (self.height - 1))}})

    def button(self, name, down):
        self.events({"type": "btn", "data": {"down": down, "button": name}})

    def key(self, qcode, down):
        self.events({"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}})

    def combo(self, keys):
        for k in keys:
            self.key(k, True)
            time.sleep(0.03)
        for k in reversed(keys):
            self.key(k, False)
            time.sleep(0.03)
        time.sleep(0.05)

    def type(self, text):
        for ch in text:
            if ch.isalpha() and ch.isupper():
                self.combo(["shift", ch.lower()])
            elif ch.isalnum():
                self.combo([ch])
            elif ch in SHIFTED:
                self.combo(["shift", SHIFTED[ch]])
            elif ch in PLAIN:
                self.combo([PLAIN[ch]])

    def shot(self, name):
        ppm = os.path.join(RES, name + ".ppm")
        png = os.path.join(RES, name + ".png")
        self.call("screendump", {"filename": ppm})
        time.sleep(0.5)
        subprocess.run([sys.executable, os.path.join(ROOT, "ci", "qmp-shot.py"), "--convert", ppm, png], check=True)
        os.remove(ppm)
        return png


def run(qmp, steps):
    for raw in steps:
        # "type" keeps its text as written, spaces at the end included
        line = raw.split("#", 1)[0].strip() if not raw.lstrip().startswith("type ") else raw.lstrip().rstrip("\r\n")
        if not line:
            continue
        cmd, _, rest = line.partition(" ")
        words = rest.split()
        print("  " + line, flush=True)
        if cmd == "move":
            qmp.move(int(words[0]), int(words[1]))
        elif cmd == "click":
            button = words[0] if words else "left"
            qmp.button(button, True)
            time.sleep(0.08)
            qmp.button(button, False)
        elif cmd == "dblclick":
            for _ in range(2):
                qmp.button("left", True)
                time.sleep(0.05)
                qmp.button("left", False)
                time.sleep(0.05)
        elif cmd in ("down", "up"):
            qmp.button(words[0] if words else "left", cmd == "down")
        elif cmd == "drag":
            x1, y1, x2, y2 = map(int, words)
            qmp.move(x1, y1)
            time.sleep(0.2)
            qmp.button("left", True)
            for i in range(1, 11):
                time.sleep(0.05)
                qmp.move(x1 + (x2 - x1) * i // 10, y1 + (y2 - y1) * i // 10)
            time.sleep(0.2)
            qmp.button("left", False)
        elif cmd == "type":
            qmp.type(rest)
        elif cmd == "key":
            qmp.combo([ALIASES.get(k, k) for k in rest.split("-")])
        elif cmd == "sleep":
            time.sleep(float(words[0]))
        elif cmd == "shot":
            print("    " + qmp.shot(words[0]), flush=True)
        elif cmd == "plug":
            image = os.path.join(RES, words[1])
            qmp.call("blockdev-add", {"driver": "raw", "node-name": words[0],
                                      "file": {"driver": "file", "filename": image}})
            qmp.call("device_add", {"driver": "usb-storage", "id": words[0], "drive": words[0],
                                    "bus": "xhci.0", "removable": True})
        elif cmd == "unplug":
            qmp.call("device_del", {"id": words[0]})
            time.sleep(1)
            try:
                qmp.call("blockdev-del", {"node-name": words[0]})
            except RuntimeError as e:
                print("    " + str(e), flush=True)
        else:
            raise ValueError("unknown step: " + line)
        time.sleep(0.15)


if __name__ == "__main__":
    q = Qmp()
    run(q, open(sys.argv[1], encoding="utf-8").read().splitlines())
