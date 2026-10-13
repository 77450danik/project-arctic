#!/usr/bin/env python3
"""Builds Arctic's Internet Explorer: arctichtml.dll, the engine, and
iexplore.exe, the browser (docs/browser.md).

They are plain Windows programs, not modules of Wine: what this builds runs
on any Windows 10 or 11 as it does in Arctic, and it is built with a
Windows toolchain - llvm-mingw in tools\\llvm-mingw when it is there (in
Arctic too, no Linux needed), else a mingw-w64 cross compiler on the PATH
(x86_64-w64-mingw32-gcc in CI). The sources it borrows (deps.txt) are
downloaded once, checked and compiled into the engine.

Only what changed is compiled again: an object is rebuilt when its source,
or a header the compiler said it read, is newer than it.

usage: build.py [--out DIR] [--cc COMPILER] [--install DIR] [--clean] [-j N]
Output: OUT/Internet Explorer/{iexplore.exe,arctichtml.dll}, OUT being
out/browser of this checkout unless --out says otherwise.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# the part of lexbor the engine stands on (docs/browser.md, stage 1): the
# parser, the DOM, CSS and its selectors, the matching of styles, encodings
LEXBOR_MODULES = ["core", "tag", "ns", "dom", "html", "css", "selectors", "style", "encoding"]

WARNINGS = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
            "-Wno-sign-compare"]
DEFINES = ["-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-DUNICODE", "-D_UNICODE"]


def log(message):
    print(message, flush=True)


# --- the toolchain ---

def find_compiler(requested):
    if requested:
        return requested
    if os.environ.get("CC"):
        return os.environ["CC"]
    suffix = ".exe" if os.name == "nt" else ""
    bundled = os.path.join(ROOT, "tools", "llvm-mingw", "bin", "x86_64-w64-mingw32-clang" + suffix)
    if os.path.exists(bundled):
        return bundled
    for name in ("x86_64-w64-mingw32-clang", "x86_64-w64-mingw32-gcc"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("no compiler: put llvm-mingw in tools/llvm-mingw or a mingw-w64 cross compiler on the PATH")


def sibling_tool(compiler, tool):
    """x86_64-w64-mingw32-clang -> x86_64-w64-mingw32-windres, next to it."""
    head, name = os.path.split(compiler)
    stem, ext = os.path.splitext(name) if name.endswith(".exe") else (name, "")
    prefix = stem.rsplit("-", 1)[0]
    candidate = os.path.join(head, prefix + "-" + tool + ext) if head else prefix + "-" + tool
    if head and os.path.exists(candidate):
        return candidate
    return shutil.which(prefix + "-" + tool) or candidate


# --- the borrowed sources ---

def read_deps():
    deps = {}
    with open(os.path.join(HERE, "deps.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, version, url, sha256 = line.split()
            deps[name] = (version, url, sha256)
    return deps


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(out, name, version, url, sha256):
    """The source tree of a dependency, downloaded and unpacked once."""
    tree = os.path.join(out, "deps", "%s-%s" % (name, version))
    if os.path.exists(os.path.join(tree, ".unpacked")):
        return tree
    archive = os.path.join(out, "downloads", "%s-%s.tar.gz" % (name, version))
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    if not os.path.exists(archive) or sha256_of(archive) != sha256:
        log("downloading %s %s" % (name, version))
        with urllib.request.urlopen(url) as response, open(archive + ".part", "wb") as f:
            shutil.copyfileobj(response, f)
        os.replace(archive + ".part", archive)
    actual = sha256_of(archive)
    if actual != sha256:
        sys.exit("%s: sha256 %s, deps.txt says %s" % (archive, actual, sha256))
    shutil.rmtree(tree, ignore_errors=True)
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            # the archive's own top directory goes: the tree is <name>-<version>
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            member.name = parts[1]
            members.append(member)
        tar.extractall(tree, members=members, filter="data")
    open(os.path.join(tree, ".unpacked"), "w").close()
    return tree


# --- compiling only what changed ---

def depfile_paths(text):
    """The prerequisites of a make rule as the compiler writes it (-MMD): paths
    are separated by blanks, a blank inside a path is escaped, lines go on
    after a backslash, and a Windows path has a colon after its drive."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    rule = text.split(": ", 1)
    body = rule[1] if len(rule) == 2 else ""
    paths, current, i = [], "", 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body) and body[i + 1] == " ":
            current += " "
            i += 2
            continue
        if ch.isspace():
            if current:
                paths.append(current)
            current = ""
        else:
            current += ch
        i += 1
    if current:
        paths.append(current)
    return paths


def up_to_date(obj, source):
    dep = obj + ".d"
    if not os.path.exists(obj) or not os.path.exists(dep):
        return False
    built = os.path.getmtime(obj)
    if os.path.getmtime(source) > built:
        return False
    with open(dep, encoding="utf-8", errors="replace") as f:
        for path in depfile_paths(f.read()):
            if not os.path.exists(path) or os.path.getmtime(path) > built:
                return False
    return True


def compile_one(cc, flags, source, obj):
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    command = [cc, "-c", "-o", obj, "-MMD", "-MF", obj + ".d"] + flags + [source]
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    return source, result.returncode, (result.stdout + result.stderr).strip()


def compile_all(cc, jobs, units):
    """units: (source, object, flags). Returns the objects, or exits on errors."""
    todo = [(s, o, f) for s, o, f in units if not up_to_date(o, s)]
    if todo:
        log("compiling %d of %d files" % (len(todo), len(units)))
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for source, code, output in pool.map(lambda u: compile_one(cc, u[2], u[0], u[1]), todo):
            if output:
                log(output)
            if code:
                failed.append(source)
    if failed:
        sys.exit("%d files did not compile:\n  %s" % (len(failed), "\n  ".join(failed)))
    return [o for _, o, _ in units]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.stdout.strip() or result.stderr.strip():
        log((result.stdout + result.stderr).strip())
    if result.returncode:
        sys.exit("failed: %s" % " ".join(command))


def newer(target, inputs):
    if not os.path.exists(target):
        return True
    built = os.path.getmtime(target)
    return any(os.path.getmtime(i) > built for i in inputs)


# --- the targets ---

def objects_for(sources, base, obj_dir):
    return [(s, os.path.splitext(os.path.join(obj_dir, os.path.relpath(s, base)))[0] + ".o", None)
            for s in sources]


def build(args):
    started = time.time()
    out = os.path.abspath(args.out)
    obj_dir = os.path.join(out, "obj")
    cc = find_compiler(args.cc)
    ar = sibling_tool(cc, "ar")
    windres = sibling_tool(cc, "windres")
    is_gcc = os.path.basename(cc).rsplit("-", 1)[-1].startswith("gcc")
    log("compiler: %s" % cc)

    deps = read_deps()
    vulkan = fetch(out, "vulkan-headers", *deps["vulkan-headers"])
    lexbor = fetch(out, "lexbor", *deps["lexbor"])
    lexbor_src = os.path.join(lexbor, "source")
    quickjs = fetch(out, "quickjs-ng", *deps["quickjs-ng"])

    common = ["-O2", "-ffunction-sections", "-fdata-sections"]

    # lexbor, as a static library: its own warnings are not ours to fix, and
    # its port of Windows calls the ANSI functions, so no UNICODE for it
    sources = []
    for module in LEXBOR_MODULES:
        for dirpath, _, files in os.walk(os.path.join(lexbor_src, "lexbor", module)):
            sources += [os.path.join(dirpath, f) for f in sorted(files) if f.endswith(".c")]
    port = os.path.join(lexbor_src, "lexbor", "ports", "windows_nt", "lexbor", "core")
    sources += [os.path.join(port, f) for f in sorted(os.listdir(port)) if f.endswith(".c")]
    lexbor_flags = common + ["-w", "-D_WIN32_WINNT=0x0A00", "-DLEXBOR_STATIC", "-I" + lexbor_src]
    units = [(s, o, lexbor_flags) for s, o, _ in objects_for(sources, lexbor_src, os.path.join(obj_dir, "lexbor"))]
    lexbor_objs = compile_all(cc, args.jobs, units)
    liblexbor = os.path.join(obj_dir, "liblexbor.a")
    if newer(liblexbor, lexbor_objs):
        if os.path.exists(liblexbor):
            os.remove(liblexbor)
        # in chunks: a command line of Windows holds 32767 characters
        for i in range(0, len(lexbor_objs), 60):
            run([ar, "qc", liblexbor] + lexbor_objs[i:i + 60])
        run([ar, "s", liblexbor])

    # QuickJS-ng, the scripts of the page (docs/browser.md, stage 4): the
    # engine itself, without its libc of files and processes
    sources = [os.path.join(quickjs, f) for f in ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c")]
    quickjs_flags = common + ["-w", "-funsigned-char", "-D_GNU_SOURCE", "-DWIN32_LEAN_AND_MEAN",
                              "-D_WIN32_WINNT=0x0A00", "-DQUICKJS_NG_BUILD", "-D_CRT_SECURE_NO_WARNINGS"]
    units = [(s, o, quickjs_flags) for s, o, _ in objects_for(sources, quickjs, os.path.join(obj_dir, "quickjs"))]
    quickjs_objs = compile_all(cc, args.jobs, units)
    libquickjs = os.path.join(obj_dir, "libquickjs.a")
    if newer(libquickjs, quickjs_objs):
        if os.path.exists(libquickjs):
            os.remove(libquickjs)
        run([ar, "qcs", libquickjs] + quickjs_objs)

    # what the engine embeds of script/*.js: each a C array in obj/gen/<name>_js.h
    gen_dir = os.path.join(obj_dir, "gen")
    os.makedirs(gen_dir, exist_ok=True)
    script_dir = os.path.join(HERE, "script")
    for f in sorted(os.listdir(script_dir)):
        if not f.endswith(".js"):
            continue
        source = os.path.join(script_dir, f)
        header = os.path.join(gen_dir, f[:-3] + "_js.h")
        if newer(header, [source]):
            data = open(source, "rb").read() + b"\0"
            name = f[:-3] + "_js"
            lines = [", ".join("0x%02x" % c for c in data[i:i + 24]) for i in range(0, len(data), 24)]
            with open(header, "w", encoding="ascii") as out_h:
                out_h.write("/* generated by build.py from script/%s */\nstatic const char %s[] = {\n%s\n};\n"
                            % (f, name, ",\n".join(lines)))

    # the engine
    engine_dir = os.path.join(HERE, "engine")
    include_dir = os.path.join(HERE, "include")
    engine_flags = common + DEFINES + WARNINGS + ["-DLEXBOR_STATIC", "-I" + include_dir, "-I" + engine_dir,
                                        "-I" + os.path.join(vulkan, "include"), "-I" + lexbor_src, "-I" + quickjs, "-I" + gen_dir]
    # the engine is these folders together: the page, the network, the filter
    sources = []
    for part in ("engine", "net", "filter", "script"):
        part_dir = os.path.join(HERE, part)
        sources += [os.path.join(part_dir, f) for f in sorted(os.listdir(part_dir)) if f.endswith(".c")]
    units = [(s, o, engine_flags) for s, o, _ in objects_for(sources, HERE, os.path.join(obj_dir, "engine"))]
    engine_objs = compile_all(cc, args.jobs, units)

    ie_dir = os.path.join(out, "Internet Explorer")
    os.makedirs(ie_dir, exist_ok=True)
    dll = os.path.join(ie_dir, "arctichtml.dll")
    implib = os.path.join(obj_dir, "libarctichtml.dll.a")
    deffile = os.path.join(engine_dir, "arctichtml.def")
    runtime = ["-static-libgcc"] if is_gcc else []
    if newer(dll, engine_objs + [liblexbor, libquickjs, deffile]):
        log("linking arctichtml.dll")
        run([cc, "-shared", "-s", "-Wl,--gc-sections", "-o", dll] + engine_objs + [liblexbor, libquickjs, deffile,
            "-Wl,--out-implib," + implib] + runtime + ["-lws2_32", "-lsecur32", "-lcrypt32", "-lnormaliz", "-lshell32",
                                                       "-lshlwapi", "-lole32", "-luuid", "-luser32", "-lgdi32"])

    # the browser
    frame_dir = os.path.join(HERE, "iexplore")
    frame_flags = common + DEFINES + WARNINGS + ["-municode", "-I" + include_dir]
    sources = [os.path.join(frame_dir, f) for f in sorted(os.listdir(frame_dir)) if f.endswith(".c")]
    units = [(s, o, frame_flags) for s, o, _ in objects_for(sources, frame_dir, os.path.join(obj_dir, "iexplore"))]
    frame_objs = compile_all(cc, args.jobs, units)
    rc = os.path.join(frame_dir, "iexplore.rc")
    res = os.path.join(obj_dir, "iexplore", "iexplore.res.o")
    rc_inputs = [rc, os.path.join(frame_dir, "resource.h"), os.path.join(frame_dir, "iexplore.manifest"),
                 os.path.abspath(__file__)]     # the flags above are here
    if newer(res, rc_inputs):
        # UTF-8: llvm-windres loses the .rc's own #pragma code_page in its preprocessing
        run([windres, "--codepage=65001", "--input-format=rc", "--output-format=coff", "-I", frame_dir,
             "-i", rc, "-o", res])
    exe = os.path.join(ie_dir, "iexplore.exe")
    if newer(exe, frame_objs + [res, implib]):
        log("linking iexplore.exe")
        run([cc, "-mwindows", "-municode", "-s", "-Wl,--gc-sections", "-Wl,--stack,16777216", "-o", exe] + frame_objs + [res, implib]
            + runtime + ["-lcomctl32", "-luser32", "-lgdi32"])

    # the engine with no window, for the tests (docs/browser.md, component 7)
    run_dir = os.path.join(HERE, "htmlrun")
    sources = [os.path.join(run_dir, f) for f in sorted(os.listdir(run_dir)) if f.endswith(".c")]
    units = [(s, o, frame_flags) for s, o, _ in objects_for(sources, run_dir, os.path.join(obj_dir, "htmlrun"))]
    run_objs = compile_all(cc, args.jobs, units)
    runner = os.path.join(ie_dir, "htmlrun.exe")
    if newer(runner, run_objs + [implib]):
        log("linking htmlrun.exe")
        run([cc, "-mconsole", "-municode", "-s", "-o", runner] + run_objs + [implib] + runtime)

    # the licences the borrowed code asks to travel with it
    licences = os.path.join(ie_dir, "Licenses")
    os.makedirs(licences, exist_ok=True)
    for src, name in ((os.path.join(lexbor, "LICENSE"), "lexbor LICENSE.txt"),
                      (os.path.join(lexbor, "NOTICE"), "lexbor NOTICE.txt")):
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(licences, name))

    log("built in %.1f s: %s" % (time.time() - started, ie_dir))
    return ie_dir


def install(ie_dir, target):
    """Into a folder of Program Files: each file through a new name, so a
    browser that is running keeps the one it has open."""
    for dirpath, _, files in os.walk(ie_dir):
        rel = os.path.relpath(dirpath, ie_dir)
        dest_dir = os.path.normpath(os.path.join(target, rel))
        os.makedirs(dest_dir, exist_ok=True)
        for name in files:
            dest = os.path.join(dest_dir, name)
            shutil.copyfile(os.path.join(dirpath, name), dest + ".new")
            try:
                os.replace(dest + ".new", dest)
            except PermissionError:
                # Windows does not replace a module in use: move it aside
                os.replace(dest, dest + ".old")
                os.replace(dest + ".new", dest)
    log("installed in %s" % target)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join(ROOT, "out", "browser"))
    parser.add_argument("--cc", help="the compiler (default: tools/llvm-mingw, else mingw-w64 on the PATH)")
    parser.add_argument("--install", metavar="DIR", help="copy the result there, e.g. C:\\Program Files\\Internet Explorer")
    parser.add_argument("--clean", action="store_true", help="forget the objects (not the downloads)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    if args.clean:
        shutil.rmtree(os.path.join(os.path.abspath(args.out), "obj"), ignore_errors=True)
    ie_dir = build(args)
    if args.install:
        install(ie_dir, args.install)


if __name__ == "__main__":
    main()
