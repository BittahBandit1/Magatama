#!/usr/bin/env python3
"""Offline aspect checks: fresh objects in a temporary directory only.

Does not run CMake, rebuild/copy the normal binary, launch a GUI, or access a
memory card. Reuses the existing product's pinned flags/libraries.
"""
from pathlib import Path
import hashlib,os,shlex,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1];build=root/'build/magatama'
elf=root.parent/'disc/SLUS_209.11'
assert hashlib.sha256(elf.read_bytes()).hexdigest()=='a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff'
with tempfile.TemporaryDirectory(prefix='magatama-aspect-') as temp:
 temp=Path(temp)
 def compile_object(target,source,dest):
  lines=subprocess.check_output(['ninja','-t','commands',target],cwd=build,text=True).splitlines()
  cmd=shlex.split(next(x for x in lines if ' -c ' in x and str(source) in x))
  cmd[cmd.index('-o')+1]=str(dest)
  if '-MF' in cmd:cmd[cmd.index('-MF')+1]=str(dest)+'.d'
  subprocess.run(cmd,cwd=build,check=True)
 # Compile each changed integration unit, without installing the objects.
 for rel,target in [('src/gs-backend/rrv_gs_backend.cpp','CMakeFiles/rrv_legacy_gs_backend.dir/src/gs-backend/rrv_gs_backend.cpp.o'),('src/host/rrv_sdl_presentation.cpp','CMakeFiles/rrv_sdl_product_host.dir/src/host/rrv_sdl_presentation.cpp.o'),('src/product/main_product.cpp','CMakeFiles/Fukami.dir/src/product/main_product.cpp.o'),('src/product/patches.cpp','CMakeFiles/Fukami.dir/src/product/patches.cpp.o')]:
  compile_object(target,root/rel,temp/(Path(rel).stem+'.o'))
 lines=subprocess.check_output(['ninja','-t','commands','CMakeFiles/Fukami.dir/src/audio/nocturne_native_sound.cpp.o'],cwd=build,text=True).splitlines()
 cmd=shlex.split(next(x for x in lines if ' -c ' in x and '/src/audio/nocturne_native_sound.cpp' in x));cmd[cmd.index('-o')+1]=str(temp/'fixture.o');cmd[cmd.index('-c')+1]=str(root/'tests/nocturne_aspect_test.cpp')
 if '-MF' in cmd:cmd[cmd.index('-MF')+1]=str(temp/'fixture.d')
 fixture_compile=cmd.copy();subprocess.run(cmd,cwd=build,check=True)
 lines=subprocess.check_output(['ninja','-t','commands','candidate-bin/Fukami'],cwd=build,text=True).splitlines();cmd=shlex.split(next(x for x in lines if ' -o candidate-bin/Fukami ' in x));begin=cmd.index('&&')+1 if cmd[0]==':' else 0;end=cmd.index('&&',begin) if '&&' in cmd[begin:] else len(cmd);cmd=cmd[begin:end]
 cmd=[x for x in cmd if not(x.startswith('CMakeFiles/Fukami.dir/') and x.endswith('.o') and not any(p in x for p in ('/src/audio/','/third_party/dq8-iop/','nocturne_bezier.cpp.o')))]
 cmd[1:1]=[str(temp/'fixture.o'),str(temp/'patches.o')];cmd[cmd.index('-o')+1]=str(temp/'fixture');subprocess.run(cmd,cwd=build,check=True)
 link=cmd.copy()
 env={k:v for k,v in os.environ.items() if not k.startswith(('MAGATAMA_','RRV_','MTL_'))};subprocess.run([str(temp/'fixture')],cwd=build,env=env,check=True)
 names=['nocturne_target_cursor_test.cpp']
 if '--regressions' in sys.argv:
  names+=['nocturne_bezier_test.cpp','nocturne_effect_color_test.cpp','nocturne_ai_particle_test.cpp','nocturne_ice_test.cpp','nocturne_track_color_test.cpp']
 for name in names:
  cmd=fixture_compile.copy();cmd[cmd.index('-c')+1]=str(root/'tests'/name);subprocess.run(cmd,cwd=build,check=True)
  cmd=link.copy();exe=temp/Path(name).stem;cmd[cmd.index('-o')+1]=str(exe);subprocess.run(cmd,cwd=build,check=True);subprocess.run([str(exe)],cwd=build,env=env,check=True)
