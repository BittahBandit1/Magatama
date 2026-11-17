#!/usr/bin/env python3
"""Generate Magatama's game code from your own Shin Megami Tensei: Nocturne (USA) executable.

    python3 scripts/magatama_generate.py --elf PATH/TO/SLUS_209.11 [--out DIR] [--jobs N]

  1. checks that the ELF is the USA release (SHA-256),
  2. builds the recompiler from third_party/ps2recomp (build/magatama-generate/recompiler),
  3. runs it with config/magatama/nocturne-recomp.toml.in (function boundaries: nocturne-functions.csv),
     writing the translated code to --out (default: ../generated-nocturne, next to this checkout),
  4. lays out generated/nocturne/ for the build (scripts/magatama_prepare_generated.py: VU code, the battle/effect
     routines the function set leaves out, the ELF identity).

Everything it writes is translated game code: it stays on your computer, outside git (generated/ is ignored), and
must never be committed, uploaded or shared.
"""
import argparse, hashlib, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USA_SHA256 = "a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff"

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--elf", required=True, help="your SLUS_209.11 (from the disc's root)")
ap.add_argument("--out", default=str(ROOT.parent / "generated-nocturne"), help="where the translated code goes")
ap.add_argument("--work", default=str(ROOT / "build/magatama-generate"), help="recompiler build folder")
ap.add_argument("--jobs", type=int, default=8)
args = ap.parse_args()

elf = Path(args.elf).expanduser().resolve()
out = Path(args.out).expanduser().resolve()
work = Path(args.work).expanduser().resolve()
if hashlib.sha256(elf.read_bytes()).hexdigest() != USA_SHA256:
    sys.exit(f"{elf} is not SLUS_209.11 of Shin Megami Tensei: Nocturne (USA): its SHA-256 does not match.")
if ROOT in out.parents and not (ROOT / "generated") in [out, *out.parents]:
    sys.exit("--out must be outside the repository (or under its ignored generated/ folder)")


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


recompiler = work / "recompiler"
run(["cmake", "-S", ROOT, "-B", recompiler, "-DRRV_BUILD_RECOMPILER=ON", "-DCMAKE_BUILD_TYPE=Release"])
run(["cmake", "--build", recompiler, "--target", "ps2_recomp", "--parallel", args.jobs])
tool = next((p for p in recompiler.rglob("ps2_recomp") if p.is_file() and p.stat().st_mode & 0o111), None)
if not tool:
    sys.exit(f"ps2_recomp was not built under {recompiler}")

out.mkdir(parents=True, exist_ok=True)
config = (ROOT / "config/magatama/nocturne-recomp.toml.in").read_text()
config = (config.replace("@ELF@", str(elf)).replace("@OUTPUT@", str(out))
          .replace("@FUNCTIONS@", str(ROOT / "config/magatama/nocturne-functions.csv")))
toml = work / "nocturne-recomp.toml"
toml.write_text(config)
with open(work / "recomp.log", "w") as log:
    run([tool, toml], stdout=log, stderr=subprocess.STDOUT)
print(f"translated code: {out} (log: {work / 'recomp.log'})")

run([sys.executable, ROOT / "scripts/magatama_prepare_generated.py", "--generated", out, "--elf", elf],
    env={**__import__("os").environ, "MAGATAMA_RECOMPILER_BUILD": str(recompiler)})
