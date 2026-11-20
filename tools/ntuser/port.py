#!/usr/bin/env python3
"""The mechanical part of bringing a file of Wine's server into win32k
(docs/win32k.md): the rest is done by hand, as the compiler shows it.

port.py <server/file.c> <kernel/drivers/win32k/file.c>

The file keeps Wine's style and layout, so that later versions of Wine can be
compared with it and brought over again.
"""
import re
import sys

SERVER_INCLUDES = re.compile(r'^#include [<"](config\.h|assert\.h|stdarg\.h|stdio\.h|stdlib\.h|string\.h|'
                             r'errno\.h|limits\.h|sys/[a-z]+\.h|unistd\.h|ntstatus\.h|winternl\.h|windef\.h|'
                             r'winbase\.h|winuser\.h|winerror\.h|ntuser\.h|ddk/wdm\.h|wine/list\.h|wine/rbtree\.h|'
                             r'request\.h|object\.h|process\.h|thread\.h|handle\.h|user\.h|file\.h|unicode\.h|'
                             r'security\.h|mapping\.h|directory\.h)[>"]\s*$', re.M)

REPLACEMENTS = [
    (r"\bcurrent\b(?!_time)", "w32_current"),
    (r"\bstruct process\b", "struct w32_process"),
    (r"\bstruct thread\b", "struct w32_thread"),
    (r"\blist_add_tail\(", "wlist_add_tail("),
    (r"\blist_empty\(", "wlist_empty("),
    (r"\blist_move_tail\(", "wlist_move_tail("),
    (r"\brealloc\(", "w32_realloc("),
    (r"\bcurrent_time\b", "current_time_now()"),
    (r"#define WIN32_NO_STATUS\s*\n", ""),
    # the kernel builds with -Wimplicit-fallthrough=5: only the keyword counts
    (r"/\* *fall ?through *\*/", "fallthrough;"),
]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="utf-8").read()
    # the licence header stays; an SPDX line goes first
    text = "// SPDX-License-Identifier: LGPL-2.1-or-later\n" + text
    first = True

    def include(m):
        nonlocal first
        if first:
            first = False
            return '#include "user.h"  /* win32k: all that the server\'s headers gave */'
        return ""

    text = SERVER_INCLUDES.sub(include, text)
    for pattern, new in REPLACEMENTS:
        text = re.sub(pattern, new, text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("%s -> %s" % (src, dst))


if __name__ == "__main__":
    main()
