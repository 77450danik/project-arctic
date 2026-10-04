#!/bin/bash
# Writes a limine.conf for a boot tree: the template with @NAME@ replaced by
# the values given, and every file Limine loads given its BLAKE2b hash. With
# Secure Boot on, Limine loads nothing without one, and the config itself is
# enrolled into Limine (sign-efi.sh), so it must not change afterwards.
# usage: limine-conf.sh <template> <tree> <out> [NAME=value...]
set -euo pipefail

TEMPLATE=${1:?usage: limine-conf.sh <template> <tree> <out> [NAME=value...]}
TREE=${2:?}
CONF=${3:?}
shift 3

text=$(cat "$TEMPLATE")
for pair in "$@"; do
    text=${text//@${pair%%=*}@/${pair#*=}}
done
if [[ $text =~ @[A-Z_]+@ ]]; then
    echo "limine-conf.sh: ${BASH_REMATCH[0]} has no value"
    exit 1
fi

mkdir -p "$(dirname "$CONF")"
while IFS= read -r line; do
    if [[ $line =~ ^[[:space:]]*[a-z_]+:[[:space:]]*boot\(\):(/[^#[:space:]]+)$ ]]; then
        line="$line#$(b2sum "$TREE${BASH_REMATCH[1]}" | cut -d' ' -f1)"
    fi
    printf '%s\n' "$line"
done <<< "$text" > "$CONF"
grep -q 'boot():/[^#]*#' "$CONF" || { echo "no hashes in $CONF"; exit 1; }
