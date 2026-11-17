#!/usr/bin/env python3
"""Lay out generated/nocturne/ for the Magatama product build (cmake -DMAGATAMA_GAME=ON).

    python3 scripts/magatama_prepare_generated.py [--generated DIR] [--elf PATH]

  generated/nocturne/output                         -> link to the recompiler output (read-only game code)
  generated/nocturne/legacy-stubs/ps2_recompiled_stubs.h  copy of the generation's stub header
  generated/nocturne/vu/                           private ahead-of-time VU catalogue extracted from the ELF
  generated/nocturne/native/                       reserved for native hot functions
  generated/nocturne/effects/nocturne_bezier.cpp   battle/effect routines the generation leaves out
  generated/nocturne/source-manifest.json           the ELF identity the product checks at launch

generated/ is ignored by git. Re-run after every regeneration.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
# Defaults: the folder layout next to this checkout (<parent>/generated-nocturne, <parent>/rom/SLUS_209.11).
ap.add_argument("--generated", default=str(ROOT.parent / "generated-nocturne"))
ap.add_argument("--elf", default=str(ROOT.parent / "rom/SLUS_209.11"))
args = ap.parse_args()

def local(p):
    return Path(p).expanduser().resolve()

gen, elf = local(args.generated), local(args.elf)
if not (gen / "register_functions.cpp").is_file():
    raise SystemExit(f"no generation at {gen} (register_functions.cpp missing)")
dst = ROOT / "generated/nocturne"
dst.mkdir(parents=True, exist_ok=True)
link = dst / "output"
if link.is_symlink() or link.exists():
    if link.is_dir() and not link.is_symlink():
        raise SystemExit(f"{link} is a real directory; move it away first")
    link.unlink()
os.symlink(args.generated, link)  # link text is the Mac path
(dst / "legacy-stubs").mkdir(exist_ok=True)
shutil.copy2(gen / "ps2_recompiled_stubs.h", dst / "legacy-stubs/ps2_recompiled_stubs.h")
(dst / "vu").mkdir(exist_ok=True)
(dst / "native").mkdir(exist_ok=True)
h = hashlib.sha256(elf.read_bytes()).hexdigest()
files = sorted(p.name for p in gen.iterdir() if p.is_file())
manifest = {"schema": "magatama-generated-v1", "game": "Shin Megami Tensei: Nocturne (USA)",
            "game_input": {"name": "SLUS_209.11", "sha256": h, "size": elf.stat().st_size},
            "generated": {"path": args.generated, "file_count": len(files)}}
(dst / "source-manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
subprocess.run([sys.executable, str(ROOT / "tools/vu-aot/vu_aot_gen.py"),
                "--elf", str(elf), "--output", str(dst / "vu")], check=True)
effects = dst / "effects/nocturne_bezier.cpp"
if not effects.is_file():  # the omitted battle/effect routines (scripts/magatama_generate_bezier.py)
    subprocess.run([sys.executable, str(ROOT / "scripts/magatama_generate_bezier.py"), "--elf", str(elf),
                    "--out", str(effects)], check=True)
print(f"generated/nocturne ready: {len(files)} files, ELF sha256 {h[:16]}...")
