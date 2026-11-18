#!/usr/bin/env python3
"""Magatama for macOS: build the game on this Mac from the user's own disc image (the installer's back end).

    /usr/bin/python3 magatama_install.py --kit KIT --image DISC.iso [--app ~/Applications/Magatama.app]

KIT is the installer's build kit (scripts/magatama_macos_kit.py; Contents/Resources/kit in the installer app). The kit
holds no game code. This script
  1. imports the disc image into ~/Library/Application Support/Magatama/disc (checked: SLUS_209.11 by SHA-256),
  2. translates the game code from that executable with the kit's recompiler and generators,
  3. compiles it with Apple's command-line compiler and links the game with the kit's prebuilt runtime,
  4. puts Magatama.app together (the launcher, the game, the PCSX2 bridge) and signs it for this Mac.
It prints "step <n> <name>", "progress <done> <total> <what>", "log <text>" and finally "done <app>" or
"error <message>" lines for the installer's window. The translated code stays on this Mac; it is deleted once built.
"""
from __future__ import annotations

import argparse, concurrent.futures, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

USA_SHA256 = "a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff"
DATA = Path.home() / "Library/Application Support/Magatama"
STEPS = ["Import the disc", "Translate the game code", "Compile", "Link", "Put Magatama.app together"]


def say(*parts) -> None:
    print(*parts, flush=True)


def fail(message: str) -> None:
    say("error", message)
    sys.exit(1)


def run(cmd, log=None, **kw) -> None:
    proc = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    if log:
        log.write(proc.stdout)
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.strip().splitlines()[-12:])
        fail(f"{Path(str(cmd[0])).name} failed ({proc.returncode}):\n{tail}")


def step(n: int) -> None:
    say("step", n, STEPS[n])


def main() -> int:
    global DATA
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kit", type=Path, required=True)
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--app", type=Path, default=Path.home() / "Applications/Magatama.app")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4)))
    ap.add_argument("--keep", action="store_true", help="keep the work folder (for debugging)")
    ap.add_argument("--data", type=Path, default=Path.home() / "Library/Application Support/Magatama", help="the data folder (default: ~/Library/Application Support/Magatama)")
    args = ap.parse_args()
    DATA = args.data.expanduser()
    DATA.mkdir(parents=True, exist_ok=True)
    # A downloaded installer runs translocated (a read-only copy) and quarantined: work from a copy of the kit with
    # the quarantine flag removed, so its tools can run.
    kit = DATA / "kit"
    if kit.exists():
        shutil.rmtree(kit)
    shutil.copytree(args.kit.resolve(), kit, symlinks=True)
    subprocess.run(["xattr", "-cr", str(kit)], capture_output=True)
    recipe = json.loads((kit / "recipe.json").read_text())
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
    if not sdk:
        fail("Apple's Command Line Tools are not installed (xcode-select --install).")
    work = Path(tempfile.mkdtemp(prefix="build-", dir=DATA))
    logfile = open(DATA / "install.log", "w")
    started = time.time()

    # 1. the disc
    step(0)
    disc = DATA / "disc"
    if args.image.is_dir():  # an extracted disc folder: used where it is (developers; no 4 GB copy)
        import hashlib
        if hashlib.sha256((args.image / "SLUS_209.11").read_bytes()).hexdigest() != USA_SHA256:
            fail("That folder does not hold SLUS_209.11 of Shin Megami Tensei: Nocturne (USA).")
        if disc.is_symlink() or disc.exists():
            disc.unlink() if disc.is_symlink() else shutil.rmtree(disc)
        disc.symlink_to(args.image.resolve())
    tool = kit / "tools/magatama-disc-import"
    proc = subprocess.Popen([str(tool), "--check", str(disc)] if args.image.is_dir() else [str(tool), str(args.image), str(disc)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    result = "result 0 ok" if args.image.is_dir() else ""
    for line in proc.stdout:
        logfile.write(line)
        if line.startswith("progress "):
            say(line.strip())
        elif line.startswith("result "):
            result = line.strip()
    proc.wait()
    status = result.split(" ", 2)
    if len(status) < 2 or status[1] != "0":
        fail(status[2] if len(status) > 2 else "The disc image could not be imported.")
    elf = disc / "SLUS_209.11"

    # 2. the game code: the recompiler, the VU microcode, the routines the function set leaves out, the stub list
    step(1)
    gen = work / "generated"
    output = gen / "output"
    for sub in ("output", "vu", "legacy-stubs", "native", "effects"):
        (gen / sub).mkdir(parents=True, exist_ok=True)
    config = (kit / "config/nocturne-recomp.toml.in").read_text()
    config = (config.replace("@ELF@", str(elf)).replace("@OUTPUT@", str(output))
              .replace("@FUNCTIONS@", str(kit / "config/nocturne-functions.csv")))
    (work / "nocturne.toml").write_text(config)
    say("log", "translating the game's functions")
    run([kit / "tools/ps2_recomp", work / "nocturne.toml"], logfile)
    shutil.copy2(output / "ps2_recompiled_stubs.h", gen / "legacy-stubs/ps2_recompiled_stubs.h")
    say("log", "translating the VU microcode")
    run([sys.executable, kit / "tools/vu-aot/vu_aot_gen.py", "--elf", elf, "--output", gen / "vu"], logfile)
    say("log", "translating the battle and effect routines")
    run([kit / "tools/nocturne-effects-generate", elf, gen / "effects/nocturne_bezier.cpp"], logfile)

    # 3. compile: the game functions in batches of 50 (as the CMake unity build does), plus the three files that
    #    include generated code
    step(2)
    objects = work / "obj"
    unity = work / "unity"
    objects.mkdir()
    unity.mkdir()
    sources = sorted(output.glob("*.cpp"))
    jobs = []
    for i in range(0, len(sources), 50):
        u = unity / f"unity_{i // 50}_cxx.cxx"
        u.write_text("".join(f'#include "{s}"\n' for s in sources[i:i + 50]))
        jobs.append(("unity", u, objects / f"unity_{i // 50}.o"))
    jobs.append(("nocturne_bezier", gen / "effects/nocturne_bezier.cpp", objects / "nocturne_bezier.o"))
    jobs.append(("ps2_vu1", None, objects / "ps2_vu1.o"))
    jobs.append(("rrv_stubs", None, objects / "rrv_stubs.o"))

    def fill(token: str) -> str:
        return (token.replace("@KIT@", str(kit)).replace("@GEN@", str(gen)).replace("@SDK@", sdk)
                .replace("@OBJ@", str(objects)))

    def compile_one(job):
        kind, src, obj = job
        t = recipe[kind]
        src = src or Path(fill(t["source"]))
        cmd = [fill(x).replace("@INPUT@", str(src)).replace("@OUTPUT@", str(obj)) for x in t["command"]]
        cmd[0] = "/usr/bin/c++" if cmd[0].endswith("c++") else cmd[0]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return job, p.returncode, p.stdout

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for job, code, text in pool.map(compile_one, jobs):
            done += 1
            if text:
                logfile.write(text)
            if code != 0:
                fail(f"compiling {job[1] or job[0]} failed:\n" + "\n".join(text.strip().splitlines()[-12:]))
            say("progress", done, len(jobs), Path(str(job[2])).name)
    run(["libtool", "-static", "-o", objects / "libmagatama_game.a", *[j[2] for j in jobs if j[0] == "unity"]], logfile)

    # 4. link
    step(3)
    game = work / "Fukami"
    cmd = [fill(x).replace("@OUT@", str(game)) for x in recipe["link"]]
    cmd[0] = "/usr/bin/c++"
    run(cmd, logfile)

    # 5. the app
    step(4)
    app = args.app.expanduser()
    staging = app.with_name(app.name + ".installing")
    if staging.exists():
        shutil.rmtree(staging)
    shutil.copytree(kit / "app/Magatama.app", staging, symlinks=True)
    shutil.copy2(game, staging / "Contents/MacOS/Fukami")
    for dylib in sorted((staging / "Contents/Frameworks").iterdir()):
        run(["codesign", "--force", "--sign", "-", "--timestamp=none", dylib], logfile)
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", staging / "Contents/MacOS/Fukami"], logfile)
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", staging], logfile)
    app.parent.mkdir(parents=True, exist_ok=True)
    if app.exists():
        shutil.rmtree(app)
    staging.rename(app)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(kit, ignore_errors=True)
    say("log", f"built in {int(time.time() - started)} s")
    say("done", app)
    return 0


if __name__ == "__main__":
    sys.exit(main())
