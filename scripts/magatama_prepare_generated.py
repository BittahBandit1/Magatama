#!/usr/bin/env python3
"""Lay out generated/nocturne/ for the Magatama product build (cmake -DMAGATAMA_GAME=ON).

    python3 scripts/magatama_prepare_generated.py [--generated DIR] [--elf PATH]

  generated/nocturne/output                         -> link to the recompiler output (read-only game code)
  generated/nocturne/legacy-stubs/ps2_recompiled_stubs.h  copy of the generation's stub header
  generated/nocturne/vu/, native/                   empty: no ahead-of-time VU code or native hot functions
                                                    yet, so the runtime interprets VU microcode
  generated/nocturne/source-manifest.json           the ELF identity the product checks at launch

generated/ is ignored by git. Re-run after every regeneration.
"""
import argparse, hashlib, json, os, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--generated", default="~/magatama/generated-nocturne")
ap.add_argument("--elf", default="~/magatama/rom/SLUS_209.11")
args = ap.parse_args()

def local(p):  # the same paths as seen from wherever this runs (a mounted copy of the folder works too)
    p = Path(p)
    if p.exists():
        return p
    alt = ROOT.parent / p.relative_to("~/magatama")
    return alt

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
print(f"generated/nocturne ready: {len(files)} files, ELF sha256 {h[:16]}...")
