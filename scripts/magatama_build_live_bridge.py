#!/usr/bin/env python3
"""Resume the source-built live-settings bridge, keeping normal manifest checks."""
from pathlib import Path
import os, subprocess, sys, tomllib, json, tempfile
repo=Path(__file__).resolve().parents[1]
source=repo/'build/live-settings-bridge-source'
canonical_build=repo/'build/live-settings-bridge'
build=canonical_build
fukami=Path(os.environ.get('FUKAMI_DIR',str(Path.home()/'fukami')))
metal=fukami/'build/fukami/metal'
lock=tomllib.loads((source/'config/dependencies.lock.toml').read_text())
# A different locked patch cannot reuse a CMake cache configured against the
# old retained source. Keep that build intact and use a patch-scoped build.
# All source/patch/Metal verification still runs through the normal tools.
patch_hash=lock['pcsx2_bridge_patches'][0]['sha256']
revision=lock['pcsx2']['revision']
cache_path=build/'CMakeCache.txt'
if cache_path.exists():
    home=next((line.split('=',1)[1] for line in cache_path.read_text().splitlines()
        if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL=')),None)
    expected_suffix=f"-source-{revision[:12]}-{patch_hash[:12]}"
    if not home or not Path(home).name.endswith(expected_suffix):
        build=canonical_build.resolve().parent/f'live-settings-bridge-{patch_hash[:12]}'
manifest=build/'rrv-pcsx2-gs-bridge-manifest.json'
canonical_manifest=canonical_build/'rrv-pcsx2-gs-bridge-manifest.json' 
# The shader source receipt also pins the bridge patch. Compile a fresh,
# hash-scoped resource set using the unchanged shader builder when needed;
# never relabel the older shader binaries or overwrite the user's resources.
try:
    metal_identity=json.loads((metal/'metal-resources-manifest.json').read_text())
except (OSError,ValueError):
    metal_identity={}
if (metal_identity.get('bridge_patch',{}).get('sha256') != patch_hash or
        metal_identity.get('pcsx2_revision') != revision):
    metal=canonical_build.resolve().parent/f'metal-resources-{patch_hash[:12]}'
    if not (metal/'metal-resources-manifest.json').exists():
        shader_command=[sys.executable,str(repo/'scripts/build_pcsx2_metal_resources.py'),
            '--source',str(fukami/'build-deps/pcsx2-2.8.2'),'--output',str(metal)]
        discovery=subprocess.run(['xcrun','metal','-v'],text=True,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if discovery.returncode and 'missing Metal Toolchain' in discovery.stdout:
            # Select the already mounted Apple component without changing
            # global Xcode settings, PATH, tools, or provenance checks.
            mounted=Path('/private/var/run/com.apple.security.cryptexd/mnt')
            candidates=[p for p in mounted.glob('com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain')
                if (p/'usr/metal/current/bin/metal').is_file()]
            if len(candidates)!=1:
                raise RuntimeError('Cannot identify one installed Apple Metal toolchain')
            shader_command.extend(['--toolchain-root',str(candidates[0])])
        subprocess.run(shader_command,check=True)
env=dict(os.environ,CMAKE_BUILD_PARALLEL_LEVEL=sys.argv[1] if len(sys.argv)>1 else '8')
if not (build/'CMakeCache.txt').exists():
    env.update(PCSX2_SOURCE_DIR=str(fukami/'build-deps/pcsx2-2.8.2'),
        PCSX2_GS_BRIDGE_BUILD_DIR=str(build),PCSX2_METAL_RESOURCE_DIR=str(metal),
        PCSX2_METAL_RESOURCE_MANIFEST=str(metal/'metal-resources-manifest.toml'))
    subprocess.run([str(source/'scripts/build_pcsx2_gs_bridge.sh'),
        '-DCMAKE_BUILD_TYPE=Release','-DENABLE_QT_UI=OFF','-DENABLE_TESTS=OFF',
        '-DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON',
        '-DFETCHCONTENT_SOURCE_DIR_SSE2NEON='+str(repo/'build-deps/sse2neon-92f6de1')],env=env,check=True)
else:
    # An interrupted/failed build already has the pinned patch applied. Resume
    # it without reapplying that patch or changing its source/index identity.
    cache=(build/'CMakeCache.txt').read_text().splitlines()
    pcsx2=Path(next(s.split('=',1)[1] for s in cache if s.startswith('CMAKE_HOME_DIRECTORY:INTERNAL=')))
    subprocess.run(['cmake','-S',str(pcsx2),'-B',str(build),
        '-DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON',
        '-DRRV_PCSX2_GS_METAL_RESOURCE_DIR='+str(metal)],env=env,check=True)
    subprocess.run(['cmake','--build',str(build),'--target','rrv-pcsx2-gs-bridge'],env=env,check=True)
    # Same manifest creator and complete arguments used by the normal bridge
    # builder. It requires clean committed bridge source and locked PCSX2/
    # patch/Metal inputs. Nothing edits or skips its verifier or receipts.
    subprocess.run([sys.executable,str(source/'scripts/pcsx2_bridge_manifest.py'),'create',
        '--library',str(build/'pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.dylib'),
        '--pcsx2-source',str(pcsx2),'--build-dir',str(build),
        '--metal-resource-dir',str(metal),
        '--metal-verifier-manifest',str(metal/'metal-resources-manifest.toml'),
        '--bridge-patch',str(source/'tools/patches/pcsx2-gs-bridge-target.patch'),
        '--bridge-patch-sha256',lock['pcsx2_bridge_patches'][0]['sha256'],
        '--pcsx2-commit',lock['pcsx2']['revision'],'--output',str(manifest)],check=True)

# Publish the normal creator's complete receipt only after a successful build.
# The source and previous build/receipt remain available for review/rollback.
if manifest != canonical_manifest:
    receipt=manifest.read_bytes()
    data=json.loads(receipt)
    if Path(data['library']['path']).resolve() != (build/'pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.dylib').resolve():
        raise RuntimeError('Bridge receipt does not identify the built library')
    canonical_build.mkdir(parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix='.bridge-receipt-',dir=canonical_build)
    try:
        with os.fdopen(fd,'wb') as f:
            f.write(receipt);f.flush();os.fsync(f.fileno())
        os.replace(temp,canonical_manifest)
    finally:
        if os.path.exists(temp):os.unlink(temp)
