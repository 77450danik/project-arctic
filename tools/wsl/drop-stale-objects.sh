#!/bin/bash
# After a local Wine build was killed (e.g. by "wsl --shutdown"), objects that
# were being written may be truncated, and make treats them as up to date.
# This removes every object older than the start of the last build run
# (the birth time of its staging tree); ccache makes rebuilding the good ones cheap.
set -euo pipefail

BUILD=${WINE_WORK:-/root/arctic/wine}/build
STAGE=${WINE_WORK:-/root/arctic/wine}/stage # recreated at the start of every run
since=$(stat -c %W "$STAGE")
count=$(find "$BUILD" -type f \( -name '*.o' -o -name '*.a' -o -name '*.res' \) ! -newermt "@$since" | wc -l)
find "$BUILD" -type f \( -name '*.o' -o -name '*.a' -o -name '*.res' \) ! -newermt "@$since" -delete
echo "removed $count objects built before $(date -d "@$since")"
