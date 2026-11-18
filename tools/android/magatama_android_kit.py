#!/usr/bin/env python3
"""Make the Android app's build kit from a configured Android product build: everything that is NOT game code.

    python3 tools/android/magatama_android_kit.py --build OUT/product --deps OUT/deps --ndk NDK --out DIR

The Android counterpart of scripts/magatama_macos_kit.py. The app (src/app/android/dev/magatama/GameBuilder.java)
translates the game code from the user's own disc on the phone, compiles it with the kit's recipe and the compiler
staged by tools/android/stage_toolchain.py, and links libmain.so with the prebuilt runtime. The build only has to
have produced the runtime (every input of candidate-bin/libmain.so except the game code, see GAME_* below); the game
code itself is never compiled here. Kit layout (zipped into the APK as assets/build/kit.zip):

  recipe.json        compile templates (unity, ps2_vu1, rrv_stubs, nocturne_bezier) and the link command, with
                     @CXX@ @SYSROOT@ @KIT@ @GEN@ @OBJ@ @INPUT@ @OUTPUT@ @OUT@ placeholders
  objects/ repo/ deps/   the prebuilt runtime objects and archives, the headers the game code reads, link stubs
  config/            the recompiler's configuration template and function list

The script fails if anything from generated/ would end up in the kit.
"""
from __future__ import annotations

import argparse, json, re, shlex, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME_ARCHIVE = "librrv_legacy_game_funcs.a"
GAME_MEMBERS = {"ps2recomp/ps2xRuntime/libps2_runtime.a": "ps2_vu1.cpp.o", "librrv_legacy_stubs.a": "rrv_stubs.cpp.o"}
GAME_OBJECT_SUFFIX = "nocturne_bezier.cpp.o"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--deps", type=Path, required=True)
    ap.add_argument("--ndk", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    build, deps, out = args.build.resolve(), args.deps.resolve(), args.out.resolve()
    prebuilt = next((args.ndk.resolve() / "toolchains/llvm/prebuilt").iterdir())
    sysroot = str(prebuilt / "sysroot")
    ndk_bin = prebuilt / "bin"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rules = [(str(ROOT / "generated/nocturne"), "@GEN@"), (str(build), "@KIT@/objects"), (str(deps), "@KIT@/deps"),
             (sysroot, "@SYSROOT@"), (str(ROOT), "@KIT@/repo")]
    deps_link = args.deps  # as the build recorded it (a symlink in the build folder is kept as it is)
    rules.insert(3, (str(deps_link.absolute()), "@KIT@/deps"))

    def kit(path: str) -> str | None:
        for prefix, repl in rules:
            if path == prefix or path.startswith(prefix + "/"):
                return repl + path[len(prefix):]
        return None

    def place(src: Path) -> str:
        mapped = kit(str(src)) or kit(str(src.resolve()))
        if mapped is None or mapped.startswith(("@GEN@", "@SYSROOT@")):
            raise SystemExit(f"cannot place {src} in the kit")
        dest = out / mapped[len("@KIT@/"):]
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        return mapped

    def clean(tok: str) -> str | None:
        """A build-machine argument as the phone needs it (None: leave it out)."""
        if tok == "-g" or tok.startswith("-flto"):
            return None
        if tok.endswith("/clang++") or tok.endswith("/clang"):
            return "@CXX@"
        if tok.startswith("--sysroot="):
            return "--sysroot=@SYSROOT@"
        for flag in ("-I", "-isystem", "-iquote", "-L"):
            if tok.startswith(flag) and len(tok) > len(flag):
                m = kit(tok[len(flag):])
                return flag + m if m else tok
        if tok.startswith("/"):
            return kit(tok) or tok
        return tok

    # ---- the link command
    ninja = shutil.which("ninja") or "ninja"
    line = next(l for l in subprocess.run([ninja, "-C", str(build), "-t", "commands", "candidate-bin/libmain.so"],
                                          check=True, capture_output=True, text=True).stdout.splitlines()
                if "-o candidate-bin/libmain.so" in l)
    line = line.replace(": && ", "").replace(" && :", "")
    link = []
    for tok in shlex.split(line):
        if tok.endswith(GAME_OBJECT_SUFFIX):
            link.append("@OBJ@/nocturne_bezier.o"); continue
        if tok == GAME_ARCHIVE:
            link += ["@OBJ@/ps2_vu1.o", "@OBJ@/rrv_stubs.o", "@OBJ@/libmagatama_game.a"]; continue
        if tok == "candidate-bin/libmain.so":
            link.append("@OUT@"); continue
        p = Path(tok) if tok.startswith("/") else build / tok
        if tok.endswith((".o", ".a")) and p.is_file() and not tok.startswith(sysroot):
            mapped = place(p)
            if tok in GAME_MEMBERS:  # take the game-derived member out of the copy
                ar = str(ndk_bin / "llvm-ar")
                subprocess.run([ar, "d", str(out / mapped[len("@KIT@/"):]), GAME_MEMBERS[tok]], check=True)
            link.append(mapped); continue
        if tok.startswith("-L") and kit(tok[2:]) and kit(tok[2:]).startswith("@KIT@/deps"):
            # link stubs the runtime names with -l (SDL2)
            for name in ("libSDL2.so",):
                if (Path(tok[2:]) / name).is_file():
                    place(Path(tok[2:]) / name)
        c = clean(tok)
        if c is not None:
            link.append(c)
    missing = [t for t in link if t.startswith("@KIT@") and not (out / t[len("@KIT@/"):]).exists() and not t.startswith("-")]
    if missing:
        raise SystemExit(f"link inputs missing from the kit (build them first): {missing}")

    # ---- the compile commands of the game-derived translation units
    cc = json.loads((build / "compile_commands.json").read_text())

    def template(match) -> dict:
        e = next(e for e in cc if match(e["file"]))
        toks = shlex.split(e["command"])
        res, skip = [], False
        for t in toks:
            if skip:
                skip = False; continue
            if t == "-o":
                res += ["-o", "@OUTPUT@"]; skip = True; continue
            if t == "-c":
                res += ["-c", "@INPUT@"]; skip = True; continue
            c = clean(t)
            if c is not None:
                res.append(c)
        return {"source": kit(e["file"]) or e["file"], "command": res, "build_command": e["command"]}

    recipe = {
        "unity": template(lambda f: f.endswith("Unity/unity_0_cxx.cxx")),
        "ps2_vu1": template(lambda f: f.endswith("/ps2_vu1.cpp")),
        "rrv_stubs": template(lambda f: f.endswith("/src/rrv_stubs.cpp")),
        "nocturne_bezier": template(lambda f: f.endswith("nocturne_bezier.cpp")),
        "link": link,
    }
    for name in ("ps2_vu1", "rrv_stubs"):
        place(Path(recipe[name]["source"].replace("@KIT@/repo", str(ROOT))))

    # ---- every header those units read: the preprocessor's dependency list (the units are not compiled here)
    gen_out = ROOT / "generated/nocturne/output"
    with tempfile.TemporaryDirectory() as tmp:
        unity = Path(tmp) / "unity.cxx"
        unity.write_text("".join(f'#include "{s}"\n' for s in sorted(gen_out.glob("*.cpp"))[:50]))
        bezier = Path(tmp) / "bezier.cpp"
        bezier.write_text('#include "ps2_runtime_macros.h"\n#include "ps2_runtime.h"\n')
        headers = set()
        for name, src in (("unity", unity), ("ps2_vu1", None), ("rrv_stubs", None), ("nocturne_bezier", bezier)):
            toks = shlex.split(recipe[name].pop("build_command"))
            if src is None:
                src = Path(toks[toks.index("-c") + 1])
            i = toks.index("-o"); del toks[i:i + 2]
            i = toks.index("-c"); del toks[i:i + 2]
            dep = Path(tmp) / f"{name}.d"
            subprocess.run(toks + ["-M", "-MF", str(dep), str(src)], check=True, cwd=build)
            for h in dep.read_text().replace("\\\n", " ").split(":", 1)[1].split():
                p = Path(h) if h.startswith("/") else (build / h)
                headers.add(str(p))
    copied = 0
    for h in sorted(headers):
        if h.startswith(sysroot) or h.startswith(str(prebuilt)) or "/generated/" in h or h.startswith(tempfile.gettempdir()):
            continue
        if kit(h) and Path(h).is_file():
            place(Path(h)); copied += 1
    print(f"headers: {copied}")

    # ---- the recompiler's configuration
    (out / "config").mkdir()
    for f in ("nocturne-recomp.toml.in", "nocturne-functions.csv"):
        shutil.copy2(ROOT / "config/magatama" / f, out / "config" / f)
    (out / "recipe.json").write_text(json.dumps(recipe, indent=1) + "\n")

    # ---- no debug information (half the size; the phone links faster)
    strip = str(ndk_bin / "llvm-strip")
    for p in list(out.rglob("*.a")) + list(out.rglob("*.o")):
        subprocess.run([strip, "--strip-debug", str(p)], check=True)

    # ---- the gate: nothing generated, no recompiled function symbols
    for p in out.rglob("*"):
        if p.is_file() and ("generated" in p.parts or p.suffix in (".elf", ".irx", ".iso", ".chd")):
            raise SystemExit(f"game material in the kit: {p}")
    bad = re.compile(r"\b(?:FUN|sub|entry|LAB)_00[0-9a-fA-F]{6}_0x[0-9a-f]+")
    nm = str(ndk_bin / "llvm-nm")
    for p in list(out.rglob("*.a")) + list(out.rglob("*.o")):
        syms = subprocess.run([nm, "-g", "--defined-only", str(p)], capture_output=True, text=True).stdout
        if bad.search(syms):
            raise SystemExit(f"{p} defines recompiled game functions")
    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"kit: {out} ({size // (1 << 20)} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
