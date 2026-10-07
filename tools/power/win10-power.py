#!/usr/bin/env python3
"""Power settings of Windows 10, for Arctic (docs/power.md).

win10-power.py SYSTEM MUIDIR REPO

From the SYSTEM hive of a Windows 10 (22H2) and the uk-UA .mui files of the
DLLs its power settings name (powrprof.dll.mui, mshtml.dll.mui, stobject,
wlansvc, shell32, wkssvc, usbui, mfplat, evr, batmeter, wmpnetwk.exe.mui):
- runtime/registry/power.reg: every power setting (subgroups, settings,
  their possible values, ranges and the default of each scheme and overlay)
  and the schemes of Windows 10 (Balanced, High performance, Power saver)
  with the overlays of the power slider, as Windows has them before anyone
  changes them. Which scheme is active stays out: an update imports the file
  again and must not undo the user's choice.
- runtime/wine/modules/dlls/powrprof/powrprof.rc: the names and descriptions
  those keys point to (@%SystemRoot%\\system32\\powrprof.dll,-N), in Ukrainian.
  Names that Windows keeps in other DLLs (the wireless adapter's in
  wlansvc.dll, USB's in usbui.dll...) move to powrprof.dll from 9000 on:
  Arctic does not have those DLLs, or not with those strings.
A graphics driver's own subgroup (Intel's) stays out: it is that driver's.

Needs regipy and pefile (a venv in the WSL build distro).
"""
import os
import re
import struct
import sys

import pefile
from regipy.registry import RegistryHive

SCHEMES = {
    "381b4222-f694-41f0-9685-ff5bb260df2e",  # Balanced
    "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c",  # High performance
    "a1841308-3541-4fab-bc81-f71556f20b4a",  # Power saver
    "961cc777-2547-4f9d-8174-7d86181b8a7a",  # Better battery overlay
    "3af9b8d9-7c97-431d-ad78-34a8bfea439f",  # High performance overlay
    "ded574b5-45a0-4f42-8737-46345c09c238",  # Max performance overlay
}
POWER = "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power"
DRIVER_SUBGROUPS = {"44f3beca-a7c0-460e-9df2-bb8b99e0cba6"}  # Intel(R) Graphics Settings
POWRPROF = "@%SystemRoot%\\system32\\powrprof.dll"
MOVED_FROM = 9000
BAL, HIGH, SAVER = ("381b4222-f694-41f0-9685-ff5bb260df2e", "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c",
                    "a1841308-3541-4fab-bc81-f71556f20b4a")
# Defaults the services of Windows give at run time (wlansvc, the USB hub
# driver...), not in the hive: the values Windows 10 shows for them
ADDED_DEFAULTS = {
    # wireless adapter: power saving mode (0 max performance .. 3 max saving)
    ("19cbb8fa-5279-450e-9fac-8a3d5fedd0c1", "12bbebe6-58d6-4636-95bb-3217ef867c1a"):
        {BAL: (0, 2), HIGH: (0, 0), SAVER: (3, 3)},
    # USB selective suspend
    ("2a737441-1930-4402-8d77-b2bebba308a3", "48e6b7a6-50f5-4782-a5d4-53bb8f07e226"):
        {BAL: (1, 1), HIGH: (1, 1), SAVER: (1, 1)},
    # dimmed display brightness
    ("7516b95f-f776-4464-8c53-06167f40cc99", "f1fbfde2-a960-4165-9f88-50667911ce96"):
        {BAL: (50, 50), HIGH: (50, 50), SAVER: (50, 50)},
    # require a password on wakeup
    (None, "0e796bdb-100d-47d6-a2d5-f7d2daa51f51"):
        {BAL: (1, 1), HIGH: (1, 1), SAVER: (1, 1)},
}


def mui_strings(path):
    pe = pefile.PE(path)
    strings = {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != pefile.RESOURCE_TYPE["RT_STRING"]:
            continue
        for n in t.directory.entries:
            for lang in n.directory.entries:
                r = lang.data.struct
                d = pe.get_data(r.OffsetToData, r.Size)
                p = 0
                for i in range(16):
                    length = struct.unpack_from("<H", d, p)[0]
                    p += 2
                    s = d[p:p + 2 * length].decode("utf-16le")
                    p += 2 * length
                    if s:
                        strings[(n.id - 1) * 16 + i] = s
    return strings


class Strings:
    """powrprof's own strings, and those taken over from other DLLs"""

    def __init__(self, muidir):
        self.muidir = muidir
        self.table = mui_strings(os.path.join(muidir, "powrprof.dll.mui"))
        self.other = {}
        self.moved = {}

    def value(self, value):
        """a registry string as powrprof.dll serves it"""
        m = re.match(r"@([^,;]+),-(\d+)(.*)$", value)
        if not m or "powrprof.dll" in m.group(1).lower():
            return value
        dll = os.path.basename(m.group(1).replace("\\", "/")).lower()
        number = int(m.group(2))
        if (dll, number) not in self.moved:
            if dll not in self.other:
                path = os.path.join(self.muidir, dll + ".mui")
                self.other[dll] = mui_strings(path) if os.path.exists(path) else {}
            text = self.other[dll].get(number)
            if text is None:
                return value
            new = MOVED_FROM + len(self.moved)
            self.moved[(dll, number)] = new
            self.table[new] = text
        rest = m.group(3)
        fallback = rest[1:] if rest[:1] in (",", ";") else ""
        return "%s,-%d,%s" % (POWRPROF, self.moved[(dll, number)], fallback)


def reg_value(name, kind, value):
    key = '"%s"' % name.replace("\\", "\\\\").replace('"', '\\"')
    if kind == "REG_DWORD":
        return "%s=dword:%08x" % (key, value & 0xFFFFFFFF)
    if kind == "REG_SZ":
        return '%s="%s"' % (key, value.replace("\\", "\\\\").replace('"', '\\"'))
    if kind in ("REG_EXPAND_SZ", "REG_MULTI_SZ"):
        if kind == "REG_MULTI_SZ":
            data = "".join(v + "\0" for v in value) + "\0"
            code = 7
        else:
            data = value + "\0"
            code = 2
        raw = data.encode("utf-16le")
        return "%s=hex(%d):%s" % (key, code, ",".join("%02x" % b for b in raw))
    if kind == "REG_BINARY":
        raw = bytes.fromhex(value) if isinstance(value, str) else value
        return "%s=hex:%s" % (key, ",".join("%02x" % b for b in raw))
    if kind == "REG_QWORD":
        return "%s=hex(b):%s" % (key, ",".join("%02x" % b for b in struct.pack("<Q", value)))
    raise ValueError(kind)


def added_defaults(regpath, out):
    parts = regpath.lower().split("\\")
    i = parts.index("powersettings")
    rest = parts[i + 1:]
    subgroup, setting = (rest[0], rest[1]) if len(rest) == 2 else (None, rest[0]) if len(rest) == 1 else (None, None)
    for scheme, (ac, dc) in ADDED_DEFAULTS.get((subgroup, setting), {}).items():
        out.append("")
        out.append("[%s\\DefaultPowerSchemeValues\\%s]" % (regpath, scheme))
        out.append(reg_value("AcSettingIndex", "REG_DWORD", ac))
        out.append(reg_value("DcSettingIndex", "REG_DWORD", dc))


def dump(hive, path, regpath, out, strings, keep=lambda name: True):
    key = hive.get_key(path)
    out.append("")
    out.append("[%s]" % regpath)
    for v in key.iter_values():
        value = v.value
        if v.value_type in ("REG_SZ", "REG_EXPAND_SZ") and isinstance(value, str):
            value = strings.value(value)
        out.append(reg_value(v.name, v.value_type, value))
    added_defaults(regpath, out)
    for sub in key.iter_subkeys():
        if keep(sub.name):
            dump(hive, path + "\\" + sub.name, regpath + "\\" + sub.name, out, strings)


def main():
    system, muidir, repo = sys.argv[1:4]
    hive = RegistryHive(system)
    strings = Strings(muidir)
    select = {v.name: v.value for v in hive.get_key("\\Select").iter_values()}
    base = "\\ControlSet%03d\\Control\\Power" % select["Current"]

    out = ["Windows Registry Editor Version 5.00",
           "",
           "; The power settings and schemes of Windows 10 (tools/power/win10-power.py, docs/power.md)"]
    dump(hive, base + "\\PowerSettings", POWER + "\\PowerSettings", out, strings,
         keep=lambda name: name.lower() not in DRIVER_SUBGROUPS)
    schemes = base + "\\User\\PowerSchemes"
    out.append("")
    out.append("[%s\\User\\PowerSchemes]" % POWER)
    for sub in hive.get_key(schemes).iter_subkeys():
        if sub.name.lower() not in SCHEMES:
            continue
        # the scheme as Windows makes it: its names, none of the user's changes
        out.append("")
        out.append("[%s\\User\\PowerSchemes\\%s]" % (POWER, sub.name.lower()))
        for v in sub.iter_values():
            if v.name in ("FriendlyName", "Description"):
                out.append(reg_value(v.name, v.value_type, strings.value(v.value)))
    # the battery icon of the notification area (batmeter.dll), which the
    # taskbar's stobject.dll starts for its power service
    out += ["",
            "[HKEY_CLASSES_ROOT\\CLSID\\{7B9C3E2A-41D8-4F6B-9A2E-6C1D5B8F0A34}]",
            '@="Battery Meter"',
            "",
            "[HKEY_CLASSES_ROOT\\CLSID\\{7B9C3E2A-41D8-4F6B-9A2E-6C1D5B8F0A34}\\InProcServer32]",
            '@="C:\\\\Windows\\\\System32\\\\batmeter.dll"',
            '"ThreadingModel"="Apartment"']
    # Power Options in the Control Panel (powercpl.dll), the shell folder of
    # Windows 10; powercfg.cpl only opens it, so it stays out of the list
    po = "HKEY_CLASSES_ROOT\\CLSID\\{025A5937-A6BE-4686-A844-36FE4BEC8B6D}"
    out += ["",
            "[%s]" % po,
            reg_value("", "REG_SZ", "Power Options").replace('""=', "@=", 1),
            reg_value("LocalizedString", "REG_EXPAND_SZ", "@%SystemRoot%\\system32\\powercpl.dll,-1"),
            reg_value("InfoTip", "REG_EXPAND_SZ", "@%SystemRoot%\\system32\\powercpl.dll,-2"),
            reg_value("System.ApplicationName", "REG_SZ", "Microsoft.PowerOptions"),
            reg_value("System.ControlPanel.Category", "REG_SZ", "1,2"),
            "",
            "[%s\\DefaultIcon]" % po,
            reg_value("", "REG_EXPAND_SZ", "%SystemRoot%\\system32\\powercpl.dll,-1").replace('""=', "@=", 1),
            "",
            "[%s\\InProcServer32]" % po,
            '@="C:\\\\Windows\\\\System32\\\\powercpl.dll"',
            '"ThreadingModel"="Apartment"',
            "",
            "[%s\\ShellFolder]" % po,
            reg_value("Attributes", "REG_DWORD", 0x28000000),
            "",
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\NameSpace\\{025A5937-A6BE-4686-A844-36FE4BEC8B6D}]",
            '@="Power Options"',
            "",
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\don't load]",
            '"powercfg.cpl"="no"']
    with open(repo + "/runtime/registry/power.reg", "w", encoding="utf-16", newline="\r\n") as f:
        f.write("\n".join(out) + "\n")

    strings_list = sorted(strings.table.items())
    rc = ["/*",
          " * Power management: resources. The names and descriptions of the power",
          " * settings and schemes, as Windows 10",
          " * (uk-UA) has them: tools/power/win10-power.py. Neutral: Arctic's",
          " * Windows speaks Ukrainian, and the registry's keys point here whatever",
          " * the locale.",
          " */",
          "",
          "#pragma code_page(65001)",
          "",
          '#include "windef.h"',
          '#include "winbase.h"',
          '#include "winuser.h"',
          '#include "winnls.h"',
          "",
          "LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL",
          "",
          "STRINGTABLE",
          "{"]
    for i, s in strings_list:
        esc = s.replace("\\", "\\\\").replace('"', '""').replace("\n", "\\n").replace("\r", "\\r")
        rc.append('    %d "%s"' % (i, esc))
    rc.append("}")
    with open(repo + "/runtime/wine/modules/dlls/powrprof/powrprof.rc", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rc) + "\n")
    print(len(out), "registry lines,", len(strings_list), "strings,", len(strings.moved), "moved")


if __name__ == "__main__":
    main()
