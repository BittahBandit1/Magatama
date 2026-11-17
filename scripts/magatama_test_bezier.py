#!/usr/bin/env python3
"""Exercise the restored battle Bezier routines against the built product/runtime."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];build=root/'build/magatama'
with tempfile.TemporaryDirectory(prefix='magatama-bezier-') as tmp:
 tmp=Path(tmp);obj=tmp/'fixture.o';exe=tmp/'fixture'
 source='CMakeFiles/Fukami.dir/src/audio/nocturne_native_sound.cpp.o'
 lines=subprocess.check_output(['ninja','-t','commands',source],cwd=build,text=True).splitlines()
 cmd=shlex.split(next(x for x in lines if ' -c ' in x and '/src/audio/nocturne_native_sound.cpp' in x))
 cmd[cmd.index('-o')+1]=str(obj);cmd[cmd.index('-c')+1]=str(root/'tests'/os.environ.get('MAGATAMA_EFFECT_FIXTURE','nocturne_bezier_test.cpp'))
 if '-MF' in cmd:cmd[cmd.index('-MF')+1]=str(obj)+'.d'
 subprocess.run(cmd,cwd=build,check=True)
 lines=subprocess.check_output(['ninja','-t','commands','candidate-bin/Fukami'],cwd=build,text=True).splitlines()
 cmd=shlex.split(next(x for x in lines if ' -o candidate-bin/Fukami ' in x))
 begin=cmd.index('&&')+1 if cmd[0]==':' else 0;end=cmd.index('&&',begin) if '&&' in cmd[begin:] else len(cmd)
 cmd=cmd[begin:end]
 cmd=[x for x in cmd if not(x.startswith('CMakeFiles/Fukami.dir/') and x.endswith('.o') and not any(p in x for p in ('/src/audio/','/third_party/dq8-iop/','/src/product/patches.cpp.o','nocturne_bezier.cpp.o')))]
 cmd.insert(1,str(obj));cmd[cmd.index('-o')+1]=str(exe)
 subprocess.run(cmd,cwd=build,check=True)
 env={k:v for k,v in os.environ.items() if not k.startswith(('MAGATAMA_','RRV_','MTL_'))}
 subprocess.run([str(exe)],cwd=build,env=env,check=True)
