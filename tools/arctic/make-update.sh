#!/bin/bash
# An update for an Arctic, made of what was built since (docs/updates.md):
# run in the arctic-build distribution, in Arctic itself (wsl.exe, bash.exe)
# or in WSL on Windows 10 with --to.
#
#   --windows  ReactOS of this checkout (out/reactos, tools/wsl/build-reactos.sh):
#              the files of C:\Windows that differ from the ones on that C:
#   --host     host.sqfs of the last image build (tools/wsl/build-image.sh):
#              Wine, the host's programs, drivers' user space
#   --boot     the kernel and the initrd of that build, Limine and shim,
#              signed for that C: (installed next to Windows: for its GPT GUID)
#   --live     the C:\Windows files go in at once and explorer.exe restarts;
#              the old ones wait in Host\Live until the next update
#   --to DIR   the C: to update, as a path (Arctic's own C: is /mnt/c there;
#              from Windows 10 the installed Arctic's is /mnt/f)
#
# The package goes to <C:>\Windows\System32\Host\Update, added to one already
# waiting there; "ready" is written last. The next start installs it with
# Windows' "Робота з оновленнями" screen.
# usage: make-update.sh [--to DIR] [--windows] [--host] [--boot] [--live]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=/root/arctic/out
C=
WINDOWS=0 HOST=0 BOOT=0 LIVE=0
while [ $# -gt 0 ]; do
    case $1 in
    --to) C=$2; shift ;;
    --windows) WINDOWS=1 ;;
    --host) HOST=1 ;;
    --boot) BOOT=1 ;;
    --live) LIVE=1 ;;
    *) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
    esac
    shift
done
if [ -z "$C" ]; then
    # in Arctic, C: is /mnt/c and holds the host
    [ -f /mnt/c/Windows/System32/Host/host.sqfs ] && C=/mnt/c ||
        { echo "not in Arctic: name its C: with --to (the installed one is /mnt/f from Windows 10)"; exit 2; }
fi
[ -f "$C/Windows/System32/Host/host.sqfs" ] || { echo "$C is not an Arctic C:"; exit 2; }
[ $((WINDOWS + HOST + BOOT)) -gt 0 ] || { echo "nothing to put in: --windows, --host or --boot"; exit 2; }

U="$C/Windows/System32/Host/Update"
mkdir -p "$U"
# whatever already waits stays, but is not installed while this one is made
PARTS=$(grep -v '^[0-9]' "$U/ready" 2>/dev/null || true)
grep -q '^[0-9]' "$U/ready" 2>/dev/null && PARTS=$(printf 'host.sqfs %s\nBoot' "$(cat "$U/ready")")
rm -f "$U/ready"
# a line of "ready" in place of the one for the same part
add_part() { PARTS=$({ printf '%s\n' "$PARTS" | awk -v k="${1%% *}" 'NF && $1 != k'; printf '%s\n' "$1"; }); }

# --- C:\Windows: where build-rootfs.sh puts each file of out/reactos ---
changed=()
if [ "$WINDOWS" = 1 ]; then
    SRC="$ROOT/out/reactos/Windows"
    [ -d "$SRC/System32" ] || { echo "no $SRC: run tools/wsl/build-reactos.sh (wsl -d arctic-ros)"; exit 1; }
    while IFS= read -r -d '' f; do
        rel=${f#"$SRC"/}
        dest=$rel # C: is case-insensitive: Resources\Themes is resources\themes
        if ! cmp -s "$f" "$C/Windows/$dest" 2>/dev/null; then
            changed+=("$dest")
            mkdir -p "$(dirname "$U/Windows/$dest")"
            cp "$f" "$U/Windows/$dest"
        fi
        # the shell's common controls 6.0 are the side-by-side copy
        if [ "$rel" = System32/comctl32.dll ]; then
            for sxs in "$C"/Windows/[Ww]in[Ss]x[Ss]/amd64_microsoft.windows.common-controls_*/comctl32.dll; do
                [ -e "$sxs" ] && ! cmp -s "$f" "$sxs" || continue
                dest=${sxs#"$C/Windows/"}
                changed+=("$dest")
                mkdir -p "$(dirname "$U/Windows/$dest")"
                cp "$f" "$U/Windows/$dest"
            done
        fi
    done < <(find "$SRC" -type f -print0)
    echo "C:\\Windows: ${#changed[@]} files differ"
    printf '  %s\n' "${changed[@]}"
    if [ ${#changed[@]} -gt 0 ] && [ "$LIVE" = 1 ]; then
        # Linux replaces a file a process has open: the process keeps the old
        # one, the next one to start takes the new
        B="$C/Windows/System32/Host/Live/$(date +%Y%m%d-%H%M%S)"
        for dest in "${changed[@]}"; do
            mkdir -p "$(dirname "$B/$dest")" "$(dirname "$C/Windows/$dest")"
            [ -e "$C/Windows/$dest" ] && cp -p "$C/Windows/$dest" "$B/$dest"
            mv -f "$U/Windows/$dest" "$C/Windows/$dest.new"
            mv -f "$C/Windows/$dest.new" "$C/Windows/$dest"
        done
        rm -rf "$U/Windows"
        # arctic-init starts the shell again when it ends
        pkill -x explorer.exe && echo "explorer.exe restarted" || true
        echo "in place now; the files before are in ${B#"$C"}"
    elif [ ${#changed[@]} -gt 0 ]; then
        add_part Windows
    fi
fi

# --- host.sqfs ---
if [ "$HOST" = 1 ]; then
    H="$BUILD/iso/arctic/host.sqfs"
    [ -f "$H" ] || { echo "no $H: run tools/wsl/build-image.sh"; exit 1; }
    if cmp -s "$H" "$C/Windows/System32/Host/host.sqfs"; then
        echo "host.sqfs: the same as on C:"
    else
        cp "$H" "$U/host.sqfs.part"
        mv "$U/host.sqfs.part" "$U/host.sqfs"
        add_part "host.sqfs $(stat -c %s "$U/host.sqfs")"
        echo "host.sqfs: $(du -h "$U/host.sqfs" | cut -f1)"
    fi
fi

# --- the boot files, signed for that C: ---
if [ "$BOOT" = 1 ]; then
    rm -rf "$U/Boot"
    conf="$C/Windows/Boot/Arctic/EFI/Arctic/limine.conf"
    if [ -f "$C/Windows/Boot/Arctic/EFI/Arctic/shimx64.efi" ] && [ -f "$conf" ]; then
        # installed next to Windows: \EFI\Arctic, C: named by its GPT GUID
        partuuid=$(grep -o 'arctic.root=PARTUUID=[0-9a-fA-F-]*' "$conf" | head -n1 | cut -d= -f3)
        [ -n "$partuuid" ] || { echo "no PARTUUID in $conf"; exit 1; }
        bash "$ROOT/tools/wsl/make-install-set.sh" "$partuuid" "$U/Boot" >/dev/null
    else
        [ -d "$BUILD/arctic-update/Update/Boot" ] || { echo "no stick boot set: run tools/wsl/build-image.sh"; exit 1; }
        cp -r "$BUILD/arctic-update/Update/Boot" "$U/Boot"
    fi
    add_part Boot
    echo "Boot: $(ls "$U/Boot/EFI/Arctic" | tr '\n' ' ')"
fi

PARTS=$(printf '%s\n' "$PARTS" | grep . || true)
if [ -n "$PARTS" ]; then
    sync
    printf '%s\n' "$PARTS" > "$U/ready.new"
    sync
    mv "$U/ready.new" "$U/ready"
    sync
    echo "ready: $(tr '\n' ' ' < "$U/ready")— installed at the next restart"
else
    rmdir "$U" 2>/dev/null || true
    echo "nothing waits for a restart"
fi
