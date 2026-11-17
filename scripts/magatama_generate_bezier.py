#!/usr/bin/env python3
"""Generate the omitted battle/effect/AI routines from your own USA ELF (SLUS_209.11, SHA-256 checked).

    python3 scripts/magatama_generate_bezier.py [--elf PATH] [--out PATH]

Writes generated/nocturne/effects/nocturne_bezier.cpp by default. That file is translated game code: it lives under
generated/ (ignored by git) and is never committed or redistributed.
"""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--elf',default=str(root.parent/'disc/SLUS_209.11'))
ap.add_argument('--out',default=str(root/'generated/nocturne/effects/nocturne_bezier.cpp'))
args=ap.parse_args();elf=Path(args.elf)
data=elf.read_bytes();expected='a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff'
if hashlib.sha256(data).hexdigest()!=expected:raise SystemExit('USA ELF identity mismatch')
# The recompiler build whose fmt/rabbitizer this tool links: scripts/magatama_generate.py's, else a Fukami checkout's.
cache=Path(os.environ['MAGATAMA_RECOMPILER_BUILD']) if os.environ.get('MAGATAMA_RECOMPILER_BUILD') else root/'build/magatama-generate/recompiler'
if not cache.is_dir():cache=Path(os.environ.get('FUKAMI_DIR',str(Path.home()/'fukami')))/'build/fukami-generate/recompiler'
deps=cache/'_deps';build=root/'build/nocturne-effects';build.mkdir(parents=True,exist_ok=True)
source=root/'third_party/ps2recomp';gen=build/'generate'
cmd=['c++','-std=c++20','-O2',*['-I'+str(p) for p in [source/'ps2xRecomp/include',source/'ps2xRuntime/include',deps/'fmt-src/include',deps/'rabbitizer-src/include',deps/'rabbitizer-src/tables']],str(root/'tools/nocturne-effects/generate.cpp'),str(source/'ps2xRecomp/src/lib/code_generator.cpp'),str(source/'ps2xRecomp/src/lib/r5900_decoder.cpp'),str(deps/'fmt-build/libfmt.a'),str(cache/'ps2recomp/ps2xRecomp/librabbitizer.a'),'-o',str(gen)]
subprocess.run(cmd,check=True)
out=Path(args.out);out.parent.mkdir(parents=True,exist_ok=True);subprocess.run([str(gen),str(elf),str(out)],check=True)
print('Generated sixteen original functions:17cd90,1ac298,17cb88,188b40,1824c0,2862d0,162448,192440,192a70,192a80,192a90,192a98,16be98,16beb8,16bed0,1667f0')
