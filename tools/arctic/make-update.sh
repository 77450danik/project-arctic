#!/bin/bash
# An update for an Arctic, made of what was built since (docs/updates.md):
# run in the arctic-build distribution, in Arctic itself (wsl.exe, bash.exe)
# or in WSL on Windows 10 with --to.
#
#   --windows  ReactOS of this checkout (out/reactos, tools/wsl/build-reactos.sh):
#              the files of C:\Windows that differ from the ones on that C:
#   --host     host.sqfs of the last image build (tools/wsl/build-image.sh):
#              Wine, the host's programs, drivers' user space
#              and the files of C: it has no program for any more (delete)
#   --root     the Windows tools of that build in C:\Program Files: PowerShell 7,
#              Git for Windows, GitHub CLI (ci/install-windows-tools.sh), the
#              files that differ from the ones on that C:
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
# usage: make-update.sh [--to DIR] [--windows] [--host] [--root] [--boot] [--live]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=/root/arctic/out
C=
WINDOWS=0 HOST=0 PROGRAMS=0 BOOT=0 LIVE=0
while [ $# -gt 0 ]; do
    case $1 in
    --to) C=$2; shift ;;
    --windows) WINDOWS=1 ;;
    --host) HOST=1 ;;
    --root) PROGRAMS=1 ;;
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
[ $((WINDOWS + HOST + PROGRAMS + BOOT)) -gt 0 ] || { echo "nothing to put in: --windows, --host, --root or --boot"; exit 2; }

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
    # What ReactOS built once and Wine builds now (the Sound panel): the copy
    # on C: is ReactOS's still, a native file that Wine would load before its
    # own and that wineboot leaves alone. Wine's goes in its place.
    for name in mmsys.cpl; do
        wine_file="$BUILD/wine/usr/lib/wine/x86_64-windows/$name"
        [ -f "$wine_file" ] && [ ! -e "$SRC/System32/$name" ] && [ -f "$C/Windows/System32/$name" ] || continue
        if ! grep -qa "Wine builtin DLL" "$C/Windows/System32/$name" && ! cmp -s "$wine_file" "$C/Windows/System32/$name"; then
            changed+=("System32/$name")
            mkdir -p "$U/Windows/System32"
            cp "$wine_file" "$U/Windows/System32/$name"
        fi
    done
    # The builtins of Wine as this build has them. wineboot brings the prefix
    # on C: up to a new Wine, but a module that is new to it (appinfo.dll,
    # usercpl.dll of the gate) is in no wine.inf, so the update carries what
    # differs - which after one build is a handful of files.
    PFX="$BUILD/prefix/drive_c/windows"
    if [ -d "$PFX/system32" ]; then
        wine_files=0
        for dir in system32 syswow64; do
            [ -d "$PFX/$dir" ] || continue
            for f in "$PFX/$dir"/*; do
                [ -f "$f" ] || continue
                name=${f##*/}
                # what ReactOS builds is taken above, from out/reactos
                [ -e "$SRC/System32/$name" ] && continue
                cmp -s "$f" "$C/Windows/$dir/$name" 2>/dev/null && continue
                changed+=("$dir/$name")
                mkdir -p "$U/Windows/$dir"
                cp "$f" "$U/Windows/$dir/$name"
                wine_files=$((wine_files + 1))
            done
        done
        echo "the builtins of Wine: $wine_files files differ"
    fi
    echo "C:\\Windows: ${#changed[@]} files differ"
    printf '  %s\n' "${changed[@]}"
    if [ ${#changed[@]} -gt 0 ] && [ "$LIVE" = 1 ]; then
        # Linux replaces a file a process has open: the process keeps the old
        # one, the next one to start takes the new. Not so the KnownDLLs: Wine
        # maps them once at the start (\KnownDlls) and every process takes
        # that copy, so they stay in the package for the restart
        B="$C/Windows/System32/Host/Live/$(date +%Y%m%d-%H%M%S)"
        known=0
        for dest in "${changed[@]}"; do
            case ${dest,,} in
            system32/shell32.dll|system32/shlwapi.dll)
                echo "  $dest: a KnownDLL, installed at the restart"
                known=1
                continue ;;
            esac
            mkdir -p "$(dirname "$B/$dest")" "$(dirname "$C/Windows/$dest")"
            [ -e "$C/Windows/$dest" ] && cp -p "$C/Windows/$dest" "$B/$dest"
            mv -f "$U/Windows/$dest" "$C/Windows/$dest.new"
            mv -f "$C/Windows/$dest.new" "$C/Windows/$dest"
        done
        [ "$known" = 1 ] && add_part Windows || rm -rf "$U/Windows"
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
    # Files of C: the new host has no program for: Linux's git and sh under
    # Windows names (docs/linux-gate.md). Wine would find no builtin behind
    # them; Git for Windows is the git now.
    gone=()
    for rel in Windows/System32/git.exe Windows/System32/sh.exe Windows/SysWOW64/git.exe Windows/SysWOW64/sh.exe; do
        [ -e "$C/$rel" ] && grep -qa "Wine builtin DLL" "$C/$rel" && gone+=("$rel")
    done
    if [ ${#gone[@]} -gt 0 ]; then
        touch "$U/delete"
        for rel in "${gone[@]}"; do
            grep -qxF "$rel" "$U/delete" || echo "$rel" >> "$U/delete"
        done
        add_part delete
        echo "taken away: ${gone[*]}"
    fi
fi

# --- C:\Program Files: the Windows tools of the image ---
if [ "$PROGRAMS" = 1 ]; then
    PF="$BUILD/prefix/drive_c/Program Files"
    [ -f "$PF/Git/cmd/git.exe" ] || { echo "no $PF/Git: run tools/wsl/build-image.sh"; exit 1; }
    count=0
    for tool in "PowerShell/7" "Git" "GitHub CLI"; do
        n=0
        while IFS= read -r -d '' f; do
            rel="Program Files/${f#"$PF"/}"
            cmp -s "$f" "$C/$rel" 2>/dev/null && continue
            mkdir -p "$(dirname "$U/Root/$rel")"
            cp -p "$f" "$U/Root/$rel"
            n=$((n + 1))
        done < <(find "$PF/$tool" -type f -print0)
        echo "C:\\Program Files\\${tool//\//\\}: $n files differ"
        count=$((count + n))
    done
    # their shortcuts in Start
    SM="ProgramData/Microsoft/Windows/Start Menu/Programs"
    for lnk in "PowerShell/PowerShell 7 (x64).lnk" "Git/Git Bash.lnk" "Git/Git CMD.lnk"; do
        f="$BUILD/prefix/drive_c/$SM/$lnk"
        [ -f "$f" ] && ! cmp -s "$f" "$C/$SM/$lnk" 2>/dev/null || continue
        mkdir -p "$(dirname "$U/Root/$SM/$lnk")"
        cp -p "$f" "$U/Root/$SM/$lnk"
        count=$((count + 1))
    done
    [ "$count" -gt 0 ] && add_part Root
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
