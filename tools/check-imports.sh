#!/bin/bash
# Lists the functions the ReactOS binaries import that the Wine build does not
# export, or exports only as a stub. Runs in WSL against the local Wine tree.
# usage: tools/check-imports.sh [dir with the ReactOS files]
R=${1:-"$(cd "$(dirname "$0")/.." && pwd)/out/reactos/Windows"}
SRC=/root/arctic/wine/src/dlls

for bin in "$R"/System32/*.dll "$R"/*.exe; do
    objdump -p "$bin" 2>/dev/null | awk '
        /DLL Name:/ { dll = tolower($3); sub(/\.dll$/, "", dll); next }
        /^[A-Z]/ { dll = "" }
        dll && NF == 4 && $1 ~ /^[0-9a-f]+$/ && $4 ~ /^[A-Za-z_][A-Za-z0-9_]*$/ { print dll, $4 }
    ' | sort -u | while read -r dll func; do
        spec="$SRC/$dll/$dll.spec"
        [ -f "$spec" ] || continue
        line=$(sed 's/#.*//' "$spec" | grep -E "^ *[0-9@]+ +[a-z]+( +-[^ ]+)* +$func( |\(|$)" | head -1)
        if [ -z "$line" ]; then
            echo "$(basename "$bin"): $dll.$func missing"
        elif echo "$line" | grep -qE "^ *[0-9@]+ +stub"; then
            echo "$(basename "$bin"): $dll.$func stub"
        fi
    done
done
