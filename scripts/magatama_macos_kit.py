#!/usr/bin/env python3
"""Make the macOS installer's build kit from a finished Magatama build: everything that is NOT game code.

    python3 scripts/magatama_macos_kit.py --build build/macos-release/product --out build/macos-release/kit

The kit holds the prebuilt runtime (object files and static libraries), the headers the game code compiles against,
the recipe (compile and link commands with placeholders), the recompiler and generators, the PCSX2 bridge and the
libraries it needs. The installer (src/launcher/macos-installer) generates the game code from the user's own disc,
compiles it with that recipe and links Magatama.app on the user's Mac. Left out of the kit, and compiled there:

  librrv_legacy_game_funcs.a   the translated game functions (generated/nocturne/output)
  nocturne_bezier.cpp.o        the battle/effect routines the function set leaves out (generated/nocturne/effects)
  ps2_vu1.cpp.o                includes the ahead-of-time VU microcode (generated/nocturne/vu)
  rrv_stubs.cpp.o              includes the generation's stub declarations (generated/nocturne/legacy-stubs)

The script fails if anything from generated/ would end up in the kit.
"""
from __future__ import annotations

import argparse, json, os, re, shlex, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME_ARCHIVE = "librrv_legacy_game_funcs.a"
# member objects (in the build's static libraries) that include generated files, and the sources they come from
GAME_MEMBERS = {"ps2recomp/ps2xRuntime/libps2_runtime.a": "ps2_vu1.cpp.o", "librrv_legacy_stubs.a": "rrv_stubs.cpp.o"}
GAME_OBJECT_SUFFIX = "nocturne_bezier.cpp.o"  # CMakeFiles/Fukami.dir/<...>/nocturne_bezier.cpp.o
SYSTEM = ("/usr/include", "/Applications/Xcode", "/Library/Developer", "/System", "/usr/lib")


def run(*cmd: str, cwd: Path | None = None) -> str:
    return subprocess.run(list(cmd), check=True, capture_output=True, text=True, cwd=cwd).stdout


class PathMap:
    """Absolute paths of the build machine -> kit placeholders."""

    def __init__(self, build: Path, fukami: Path | None):
        self.rules = [(str(ROOT / "generated/nocturne"), "@GEN@"), (str(build), "@KIT@/objects"), (str(ROOT), "@KIT@/repo")]
        if fukami:
            self.rules.append((str(fukami.resolve()), "@KIT@/fukami"))
        self.rules.append(("/opt/homebrew", "@KIT@/homebrew"))

    def kit(self, path: str) -> str | None:
        for prefix, repl in self.rules:
            if path == prefix or path.startswith(prefix + "/"):
                return repl + path[len(prefix):]
        return None


def sdk_token(arg: str) -> str:
    return re.sub(r"/Applications/Xcode[^ ]*?\.sdk", "@SDK@", re.sub(r"/Library/Developer/CommandLineTools/SDKs/[^/ ]*\.sdk", "@SDK@", arg))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--fukami", type=Path, default=Path(os.environ.get("FUKAMI_DIR", Path.home() / "fukami")))
    args = ap.parse_args()
    build, out = args.build.resolve(), args.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    pm = PathMap(build, args.fukami if args.fukami.exists() else None)
    ninja = shutil.which("ninja") or "/opt/homebrew/bin/ninja"

    def place(src: Path) -> str:
        """Copy a build-machine file into the kit; return its placeholder path."""
        # Keep the path as the build recorded it (build-deps/ may be a symlink into another checkout).
        mapped = pm.kit(str(src)) or pm.kit(str(src.resolve()))
        if mapped is None or mapped.startswith("@GEN@"):
            raise SystemExit(f"cannot place {src} in the kit")
        dest = out / mapped[len("@KIT@/"):]
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        return mapped

    # ---- the link command
    link_line = next(l for l in run(ninja, "-C", str(build), "-t", "commands", "candidate-bin/Fukami").splitlines()
                     if "-o candidate-bin/Fukami" in l)
    link_line = link_line.replace(": && ", "").replace(" && :", "")
    link = []
    for tok in shlex.split(link_line):
        if tok.startswith("-Wl,-rpath,"):
            link.append("-Wl,-rpath,@executable_path/../Frameworks")
            continue
        if tok.endswith(".tbd"):
            link.append("-l" + Path(tok).stem[3:])  # SDK stubs: libz.tbd -> -lz
            continue
        if tok.endswith(GAME_OBJECT_SUFFIX):
            link.append("@OBJ@/nocturne_bezier.o")
            continue
        if tok == GAME_ARCHIVE:
            link.append("@OBJ@/libmagatama_game.a")
            continue
        if tok in ("candidate-bin/Fukami",):
            link.append("@OUT@")
            continue
        p = Path(tok) if tok.startswith("/") else build / tok
        if tok.endswith((".o", ".a", ".dylib")) and p.is_file():
            mapped = place(p)
            rel = mapped
            if tok in GAME_MEMBERS:  # take the game-derived member out of the copy
                subprocess.run(["ar", "d", str(out / mapped[len("@KIT@/"):]), GAME_MEMBERS[tok]], check=True)
                subprocess.run(["ranlib", str(out / mapped[len("@KIT@/"):])], check=True)
            link.append(rel)
            continue
        link.append(sdk_token(tok))
    link[link.index("@OBJ@/libmagatama_game.a"):link.index("@OBJ@/libmagatama_game.a")] = ["@OBJ@/ps2_vu1.o", "@OBJ@/rrv_stubs.o"]

    # ---- the compile commands of the game-derived translation units
    cc = json.loads((build / "compile_commands.json").read_text())
    def template(match) -> dict:
        e = next(e for e in cc if match(e["file"]))
        toks = shlex.split(e["command"])
        res, skip = [], False
        for i, t in enumerate(toks):
            if skip:
                skip = False
                continue
            if t == "-o":
                res += ["-o", "@OUTPUT@"]; skip = True; continue
            if t == "-c":
                res += ["-c", "@INPUT@"]; skip = True; continue
            if t.startswith("-flto"):
                continue
            for flag in ("-I", "-isystem", "-iquote"):
                if t.startswith(flag) and len(t) > len(flag):
                    m = pm.kit(t[len(flag):])
                    t = flag + m if m else t
            if t.startswith("/") and not t.startswith(SYSTEM):
                t = pm.kit(t) or t
            res.append(sdk_token(t))
        return {"source": pm.kit(e["file"]) or e["file"], "command": res}

    recipe = {
        "unity": template(lambda f: f.endswith("Unity/unity_0_cxx.cxx")),
        "ps2_vu1": template(lambda f: f.endswith("/ps2_vu1.cpp")),
        "rrv_stubs": template(lambda f: f.endswith("/src/rrv_stubs.cpp")),
        "nocturne_bezier": template(lambda f: f.endswith("nocturne_bezier.cpp")),
        "link": link,
    }
    # The sources of those units that live in the repository (the two runtime files).
    for name in ("ps2_vu1", "rrv_stubs"):
        place(Path(recipe[name]["source"].replace("@KIT@/repo", str(ROOT))))

    # ---- every header those units read (ninja's dependency records), except generated/ and the SDK
    deps = run(ninja, "-C", str(build), "-t", "deps")
    wanted, current, copied = False, None, 0
    keep = re.compile(r"(Unity/unity_\d+_cxx\.cxx\.o|ps2_vu1\.cpp\.o|rrv_stubs\.cpp\.o|nocturne_bezier\.cpp\.o): #deps")
    seen = set()
    for line in deps.splitlines():
        if not line.startswith(" "):
            wanted = bool(keep.search(line))
            continue
        if not wanted:
            continue
        h = line.strip()
        if not h.startswith("/"):
            h = str(build / h)
        if h in seen or h.startswith(SYSTEM) or "/generated/" in h or h.endswith((".cxx", ".cpp")) and "/Unity/" in h:
            continue
        seen.add(h)
        if pm.kit(h) and Path(h).is_file():
            place(Path(h)); copied += 1
    print(f"headers: {copied}")

    (out / "recipe.json").write_text(json.dumps(recipe, indent=1) + "\n")

    # ---- the gate: nothing generated, no recompiled function symbols
    for p in out.rglob("*"):
        if p.is_file() and ("generated" in p.parts or p.suffix in (".elf", ".irx", ".iso", ".chd")):
            raise SystemExit(f"game material in the kit: {p}")
    bad = re.compile(r"\b(?:FUN|sub|entry|LAB)_00[0-9a-fA-F]{6}_0x[0-9a-f]+")
    for p in out.rglob("*.a"):
        syms = run("nm", "-gU", str(p))
        if bad.search(syms):
            raise SystemExit(f"{p} defines recompiled game functions")
    print(f"kit: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
