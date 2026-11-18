#!/usr/bin/env python3
"""Stage the compiler that the Android app runs on the phone to build the game (src/app/android: GameBuilder).

    python3 tools/android/stage_toolchain.py --termux ROOT --ndk NDK --out DIR

ROOT is an unpacked set of Termux packages (clang, lld, libllvm, libcompiler-rt and their libraries; see
tools/android/termux-toolchain.txt): data/data/com.termux/files/usr inside it. NDK is the NDK the runtime is built
with: its sysroot (headers, the API-28 link stubs, libc++) and libunwind are what the game is compiled and linked
against on the phone, as in the desktop build. Output:

  DIR/lib/       for the APK's lib/arm64-v8a: the executables and their libraries, each named lib*.so (the only files
                 Android unpacks from an APK and lets an app run), their library names and RUNPATH rewritten
                 ($ORIGIN, our names) so they find each other and nothing of Termux's prefix
  DIR/toolchain/ for the toolchain archive the app unpacks (assets/build/toolchain.zip): sysroot/ and clang/
                 (the compiler's resource directory: its headers, compiler-rt builtins, libunwind)

The executables: libmg_clang.so (clang 21, the compiler and driver), libmg_lld.so (lld; run as ld.lld through a
symlink, as lld picks its flavour from its own name).
"""
from __future__ import annotations

import argparse, shutil, struct, sys
from pathlib import Path

# file in Termux's lib/ (or bin/) -> name in the APK. Their DT_NEEDED and DT_SONAME entries are renamed the same way.
LIBS = {
    "bin/clang-21": "libmg_clang.so",
    "bin/lld": "libmg_lld.so",
    "lib/libLLVM.so": "libLLVM.so",
    "lib/libclang-cpp.so": "libclang-cpp.so",
    "lib/libc++_shared.so": "libmg_cxx.so",        # Termux's libc++ (its LLVM was built against it), apart from the game's
    "lib/libffi.so": "libffi.so",
    "lib/libxml2.so.16": "libmg_xml2.so",
    "lib/libz.so.1": "libmgz.so",
    "lib/libzstd.so.1": "libmgzs.so",
    "lib/libiconv.so": "libiconv.so",
}
# (A new name may not be longer than the old one: the strings are rewritten in place.)
NEEDED_RENAME = {"libc++_shared.so": "libmg_cxx.so", "libxml2.so.16": "libmg_xml2.so", "libz.so.1": "libmgz.so",
                 "libzstd.so.1": "libmgzs.so"}
SYSTEM = {"libc.so", "libm.so", "libdl.so", "liblog.so"}
DT_NEEDED, DT_STRTAB, DT_SONAME, DT_RPATH, DT_RUNPATH = 1, 5, 14, 15, 29


def patch_elf(path: Path) -> list[str]:
    """Rewrite DT_NEEDED / DT_SONAME / DT_RUNPATH strings in place (each new string is no longer than the old one)."""
    data = bytearray(path.read_bytes())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"{path}: not a 64-bit little-endian ELF")
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads, dynamic = [], None
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dynamic = (p_offset, p_filesz)
    if dynamic is None:
        raise SystemExit(f"{path}: no dynamic section")

    def file_offset(vaddr: int) -> int:
        for v, o, size in loads:
            if v <= vaddr < v + size:
                return vaddr - v + o
        raise SystemExit(f"{path}: address {vaddr:#x} outside the file")

    entries = []
    for at in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, val = struct.unpack_from("<qQ", data, at)
        if tag == 0:
            break
        entries.append((tag, val))
    strtab = file_offset(next(v for t, v in entries if t == DT_STRTAB))
    needed = []
    for tag, val in entries:
        if tag not in (DT_NEEDED, DT_SONAME, DT_RPATH, DT_RUNPATH):
            continue
        start = strtab + val
        end = data.index(0, start)
        old = data[start:end].decode()
        if tag in (DT_RPATH, DT_RUNPATH):
            new = "$ORIGIN"
        elif tag == DT_SONAME:
            new = LIBS.get(f"lib/{old}", old)
        else:
            new = NEEDED_RENAME.get(old, old)
            needed.append(new)
        if new != old:
            if len(new) > len(old):
                raise SystemExit(f"{path}: {new} is longer than {old}")
            data[start:end] = new.encode() + b"\0" * (len(old) - len(new))
    path.write_bytes(bytes(data))
    return needed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--termux", type=Path, required=True)
    ap.add_argument("--ndk", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    usr = args.termux / "data/data/com.termux/files/usr"
    out = args.out
    if out.exists():
        shutil.rmtree(out)
    (out / "lib").mkdir(parents=True)
    names = set(LIBS.values())
    for src, name in LIBS.items():
        dest = out / "lib" / name
        shutil.copyfile((usr / src).resolve(), dest)
        for lib in patch_elf(dest):
            if lib not in names and lib not in SYSTEM:
                raise SystemExit(f"{name} needs {lib}, which is neither staged nor an Android system library")

    # The compiler's resource directory: clang 21's own headers and builtins, the NDK's libunwind.
    tc = out / "toolchain"
    clang = tc / "clang"
    shutil.copytree(usr / "lib/clang/21/include", clang / "include")
    (clang / "lib/linux/aarch64").mkdir(parents=True)
    shutil.copy2(usr / "lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a", clang / "lib/linux")
    prebuilt = next((args.ndk / "toolchains/llvm/prebuilt").iterdir())
    ndk_clang = next((prebuilt / "lib/clang").iterdir())
    for f in ("libunwind.a", "libatomic.a"):
        shutil.copy2(ndk_clang / "lib/linux/aarch64" / f, clang / "lib/linux/aarch64")
    # The sysroot: every header but the other architectures', and the arm64 libraries (API 28 link stubs, libc++).
    sysroot = prebuilt / "sysroot/usr"
    other = {"arm-linux-androideabi", "i686-linux-android", "x86_64-linux-android", "riscv64-linux-android"}
    shutil.copytree(sysroot / "include", tc / "sysroot/usr/include",
                    ignore=lambda d, names: [n for n in names if n in other])
    lib = tc / "sysroot/usr/lib/aarch64-linux-android"
    (lib / "28").mkdir(parents=True)
    for f in (sysroot / "lib/aarch64-linux-android/28").iterdir():
        shutil.copy2(f, lib / "28")
    for f in ("libc++_shared.so", "libc++abi.a", "libc++_static.a", "libc++experimental.a"):
        shutil.copy2(sysroot / "lib/aarch64-linux-android" / f, lib)
    # libc++.so in 28/ is a linker script naming libc++_shared.so: keep it as the NDK has it.
    total = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"toolchain staged in {out} ({total // (1 << 20)} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
