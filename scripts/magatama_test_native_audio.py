#!/usr/bin/env python3
"""Link and run the actual opt-in native SIF bridge against the built runtime."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];build=root/'build/magatama'
with tempfile.TemporaryDirectory(prefix='magatama-native-transport-') as tmp:
 tmp=Path(tmp);obj=tmp/'fixture.o';exe=tmp/'fixture'
 source='CMakeFiles/Fukami.dir/src/audio/nocturne_native_sound.cpp.o'
 lines=subprocess.check_output(['ninja','-t','commands',source],cwd=build,text=True).splitlines()
 cmd=shlex.split(next(x for x in lines if ' -c ' in x and '/src/audio/nocturne_native_sound.cpp' in x))
 native=tmp/'native.o';fresh=cmd.copy();fresh[fresh.index('-o')+1]=str(native)
 if '-MF' in fresh:fresh[fresh.index('-MF')+1]=str(native)+'.d'
 subprocess.run(fresh,cwd=build,check=True)
 cmd[cmd.index('-o')+1]=str(obj);cmd[cmd.index('-c')+1]=str(root/'tests/nocturne_native_audio_transport_test.cpp')
 if '-MF' in cmd:cmd[cmd.index('-MF')+1]=str(obj)+'.d'
 subprocess.run(cmd,cwd=build,check=True)
 lines=subprocess.check_output(['ninja','-t','commands','candidate-bin/Fukami'],cwd=build,text=True).splitlines()
 cmd=shlex.split(next(x for x in lines if ' -o candidate-bin/Fukami ' in x))
 begin=cmd.index('&&')+1 if cmd[0]==':' else 0;end=cmd.index('&&',begin) if '&&' in cmd[begin:] else len(cmd)
 cmd=cmd[begin:end]
 cmd=[x for x in cmd if not(x.startswith('CMakeFiles/Fukami.dir/') and x.endswith('.o') and not any(p in x for p in ('/src/audio/','/third_party/dq8-iop/','/src/product/patches.cpp.o','nocturne_bezier.cpp.o')))]
 cmd=[x for x in cmd if not x.endswith('/src/audio/nocturne_native_sound.cpp.o')]
 cmd[1:1]=[str(obj),str(native)];cmd[cmd.index('-o')+1]=str(exe)
 subprocess.run(cmd,cwd=build,check=True)
 env={k:v for k,v in os.environ.items() if not k.startswith(('MAGATAMA_','RRV_','MTL_'))}
 subprocess.run([str(exe)],cwd=build,env=env,check=True)
