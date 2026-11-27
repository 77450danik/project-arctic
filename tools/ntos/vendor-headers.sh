#!/bin/bash
# the Wine headers ntos is built with, from the dependency files kbuild wrote
K=/root/arctic/kbuild/linux-7.2.6
SP=${1:?usage: vendor-headers.sh <out dir>}
INC=/root/arctic/wine/src/include
rm -rf $SP && mkdir -p $SP
cd $K
cat $(find drivers/ntos -name '.*.o.cmd') | grep -o "$INC/[^ \]*" | sort -u | while read f; do
    rel=${f#$INC/}
    mkdir -p "$SP/$(dirname "$rel")"
    cp "$f" "$SP/$rel"
done
# an inline function of no storage class is a definition of its own in every
# file under the kernel's gnu_inline: static, as Wine's others are
sed -i 's/^FORCEINLINE /static FORCEINLINE /' $SP/ddk/wdm.h $SP/ddk/ntddk.h 2>/dev/null
find $SP -type f | wc -l
du -sh $SP
grep -n "SERVER_PROTOCOL_VERSION" $SP/wine/server_protocol.h
