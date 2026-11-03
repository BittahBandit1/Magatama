#!/usr/bin/env python3
"""Regenerate the omitted battle/effect/AI routines from the locked USA ELF."""
from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1];elf=root.parent/'disc/SLUS_209.11'
data=elf.read_bytes();expected='a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff'
if hashlib.sha256(data).hexdigest()!=expected:raise SystemExit('USA ELF identity mismatch')
cache=Path(os.environ.get('FUKAMI_DIR',str(Path.home()/'fukami')))/'build/fukami-generate/recompiler'
deps=cache/'_deps';build=root/'build/nocturne-effects';build.mkdir(parents=True,exist_ok=True)
source=root/'third_party/ps2recomp';gen=build/'generate'
cmd=['c++','-std=c++20','-O2',*['-I'+str(p) for p in [source/'ps2xRecomp/include',source/'ps2xRuntime/include',deps/'fmt-src/include',deps/'rabbitizer-src/include',deps/'rabbitizer-src/tables']],str(root/'tools/nocturne-effects/generate.cpp'),str(source/'ps2xRecomp/src/lib/code_generator.cpp'),str(source/'ps2xRecomp/src/lib/r5900_decoder.cpp'),str(deps/'fmt-build/libfmt.a'),str(cache/'ps2recomp/ps2xRecomp/librabbitizer.a'),'-o',str(gen)]
subprocess.run(cmd,check=True)
out=root/'src/product/nocturne_bezier.cpp';subprocess.run([str(gen),str(elf),str(out)],check=True)
print('Generated sixteen original functions:17cd90,1ac298,17cb88,188b40,1824c0,2862d0,162448,192440,192a70,192a80,192a90,192a98,16be98,16beb8,16bed0,1667f0')
