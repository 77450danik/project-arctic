"""Takes Windows' sound events and its default sound scheme into Arctic: the
events and the scheme of a Windows 10 default profile (AppEvents of
Users\\Default\\NTUSER.DAT), the sounds they play (Windows\\Media) and the
events' names in Ukrainian (mmres.dll.mui).

Kept: the events of Windows itself and of Explorer whose names are
mmres.dll's, without the ringtones and alarms of the phone. Their names
become strings of mmsys.cpl, which shows them (the Sound panel's "Звуки");
winmm plays the scheme's sounds for MessageBeep and PlaySound's aliases.

usage: python tools/make-sounds.py NTUSER.DAT MEDIA_DIR MMRES_MUI
  MEDIA_DIR: Windows\\Media already readable (WOF-compressed files unpacked)
  writes runtime/registry/sounds.reg, runtime/art/media/*.wav and
  runtime/wine/modules/dlls/mmsys.cpl/events.rcinc
"""
import os
import re
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "out", "pylib"))
from hive2reg import Hive  # noqa: E402
import pefile  # noqa: E402

REG = os.path.join(ROOT, "runtime", "registry", "sounds.reg")
MEDIA = os.path.join(ROOT, "runtime", "art", "media")
RC = os.path.join(ROOT, "runtime", "wine", "modules", "dlls", "mmsys.cpl", "events.rcinc")
APPS = [".Default", "Explorer"]


def text(data):
    return data.decode("utf-16-le", "replace").rstrip("\0")


def value(hive, key, name=""):
    for n, typ, data in hive.values(key):
        if n == name and typ in (1, 2):
            return text(data)
    return None


def mui_strings(path):
    pe = pefile.PE(path)
    out = {}
    for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if entry.id != 6:
            continue
        for block in entry.directory.entries:
            for lang in block.directory.entries:
                rva, size = lang.data.struct.OffsetToData, lang.data.struct.Size
                data = pe.get_memory_mapped_image()[rva:rva + size]
                pos = 0
                for i in range(16):
                    length = struct.unpack_from("<H", data, pos)[0]
                    s = data[pos + 2:pos + 2 + length * 2].decode("utf-16-le")
                    pos += 2 + length * 2
                    if s:
                        out[(block.id - 1) * 16 + i] = s
    return out


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    hive = Hive(sys.argv[1])
    media_dir, uk = sys.argv[2], mui_strings(sys.argv[3])
    labels = hive.find("AppEvents\\EventLabels")
    label_of = {}
    for key in hive.subkeys(labels):
        disp = value(hive, key, "DispFileName") or ""
        m = re.match(r"@mmres\.dll,-(\d+)$", disp, re.I)
        if m:
            label_of[key["name"].lower()] = (key["name"], value(hive, key) or key["name"], int(m.group(1)))

    reg = ["REGEDIT4", "",
           "; Windows' sound events and its default sound scheme (tools/make-sounds.py, from a",
           "; Windows 10 default profile): what the Sound panel's Sounds page lists and winmm plays",
           ""]
    strings = {800: "Windows Default", 801: "No Sounds", 5856: "Windows", 5854: "File Explorer"}
    used_labels, files = {}, set()
    for app in APPS:
        app_key = hive.find("AppEvents\\Schemes\\Apps\\" + app)
        base = "HKEY_CURRENT_USER\\AppEvents\\Schemes\\Apps\\" + app
        reg += [f"[{base}]", f'@="{esc(value(hive, app_key) or app)}"',
                f'"DispFileName"="@mmsys.cpl,-{5856 if app == ".Default" else 5854}"', ""]
        for event in hive.subkeys(app_key):
            name = event["name"]
            if name.lower() not in label_of or name.startswith("Notification.Looping"):
                continue
            used_labels[name.lower()] = label_of[name.lower()]
            reg.append(f"[{base}\\{name}]")
            reg.append("")
            for scheme in hive.subkeys(event):
                if scheme["name"] not in (".Current", ".Default"):
                    continue
                path = value(hive, scheme) or ""
                wav = os.path.basename(path.replace("\\", "/"))
                if wav:
                    files.add(wav)
                    path = "C:\\Windows\\Media\\" + wav
                reg += [f"[{base}\\{name}\\{scheme['name']}]", f'@="{esc(path)}"', ""]
    for name, label, number in sorted(used_labels.values()):
        strings[number] = label
        reg += [f"[HKEY_CURRENT_USER\\AppEvents\\EventLabels\\{name}]", f'@="{esc(label)}"',
                f'"DispFileName"="@mmsys.cpl,-{number}"', ""]
    reg += ["[HKEY_CURRENT_USER\\AppEvents\\Schemes]", '@=".Default"', "",
            "[HKEY_CURRENT_USER\\AppEvents\\Schemes\\Names\\.Default]", '@="@mmsys.cpl,-800"', "",
            "[HKEY_CURRENT_USER\\AppEvents\\Schemes\\Names\\.None]", '@="@mmsys.cpl,-801"', ""]
    with open(REG, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(reg))

    os.makedirs(MEDIA, exist_ok=True)
    for wav in sorted(files):
        src = os.path.join(media_dir, wav)
        if not os.path.exists(src):
            # the folder's own spelling of the name
            match = [n for n in os.listdir(media_dir) if n.lower() == wav.lower()]
            src = os.path.join(media_dir, match[0]) if match else src
        shutil.copyfile(src, os.path.join(MEDIA, wav))

    def table(lang, get):
        rows = [f"LANGUAGE {lang}", "", "STRINGTABLE", "{"]
        for number in sorted(strings):
            rows.append(f'    {number} "{get(number).replace(chr(34), chr(34) * 2)}"')
        return rows + ["}", ""]

    rc = ["/* Windows' sound events by name: written by tools/make-sounds.py (mmres.dll.mui) */", ""]
    rc += table("LANG_ENGLISH, SUBLANG_DEFAULT", lambda n: strings[n])
    rc += table("LANG_UKRAINIAN, SUBLANG_DEFAULT", lambda n: uk.get(n, strings[n]))
    with open(RC, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rc))
    print(f"{len(used_labels)} events, {len(files)} sounds: {', '.join(sorted(files))}")


if __name__ == "__main__":
    main()
