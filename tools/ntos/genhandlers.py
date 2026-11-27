#!/usr/bin/env python3
"""ntos's table of request handlers (kernel/drivers/ntos/handlers.c).

genhandlers.py <build>/server/request_handlers.h <kernel/drivers/ntos>

In the order of the protocol, as the server's req_handlers: a wrapper of one
signature for each handler ntos has, so that no call goes through a pointer
of another type (kCFI); NULL for USER's, which win32k serves and ntdll sends
there. Prints the ones that are neither, which must not exist.
"""
import os
import re
import sys


def main():
    table_src, ntos = sys.argv[1], sys.argv[2]
    names = re.findall(r"^DECL_HANDLER\((\w+)\);", open(table_src, encoding="utf-8").read(), re.M)
    have = set()
    for root, _, files in os.walk(ntos):
        for f in files:
            if f.endswith(".c"):
                have.update(re.findall(r"^DECL_HANDLER\(\s*(\w+)\s*\)\s*$", open(os.path.join(root, f), encoding="utf-8").read(), re.M))
    win32k = set()
    w32 = os.path.join(ntos, "..", "win32k", "handlers.h")
    if os.path.exists(w32):
        win32k = set(re.findall(r"\bH\((\w+)\)", open(w32, encoding="utf-8").read()))
    out = ["// SPDX-License-Identifier: LGPL-2.1-or-later",
           "/* ntos: the request handlers, in the protocol's order (made by tools/ntos/genhandlers.py) */",
           '#include "ntos.h"', ""]
    for n in names:
        if n in have:
            out.append("DECL_HANDLER(%s);" % n)
    out.append("")
    for n in names:
        if n in have:
            out.append("static void w_%s( const void *req, void *reply ) { req_%s( req, reply ); }" % (n, n))
    out += ["", "const ntos_handler ntos_handlers[REQ_NB_REQUESTS] =", "{"]
    for n in names:
        out.append("    %s,%s" % ("w_" + n if n in have else "NULL", "" if n in have else "  /* %s: win32k's */" % n))
    out += ["};", ""]
    with open(os.path.join(ntos, "handlers.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    # the names, in the same order, for the lock statistics (main.c)
    with open(os.path.join(ntos, "req_names.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("/* ntos: the requests' names in the protocol's order (made by tools/ntos/genhandlers.py) */\n")
        f.write("".join("NTOS_REQ_NAME(%s)\n" % n for n in names))
    missing = [n for n in names if n not in have and win32k and n not in win32k]
    print("%d requests: %d ntos, %d win32k" % (len(names), len([n for n in names if n in have]),
                                                len([n for n in names if n not in have])))
    if missing:
        print("SERVED BY NEITHER:", " ".join(missing))


if __name__ == "__main__":
    main()
