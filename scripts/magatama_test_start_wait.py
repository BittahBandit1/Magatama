#!/usr/bin/env python3
"""Check typed CD waits against the already-built real runtime and scheduler.

Build with scripts/magatama_build.sh first. This headless fixture verifies
matching guest state/wake coordinates, arbitrary waits, mixed readiness order,
cancellation, and both verifier fallback and retained invariant assertions.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path)
args = ap.parse_args()
build = root / 'build/magatama'
source = root / 'tests/nocturne_start_wait_test.cpp'
patch_obj = 'CMakeFiles/Fukami.dir/src/product/patches.cpp.o'
with tempfile.TemporaryDirectory(prefix='magatama-start-wait-') as directory:
    obj = str(Path(directory) / 'fixture.o')
    exe = str(Path(directory) / 'fixture')
    commands = subprocess.check_output(['ninja', '-t', 'commands', patch_obj], cwd=build, text=True)
    line = next(x for x in commands.splitlines() if ' -c ' in x and '/src/product/patches.cpp' in x)
    command = shlex.split(line)
    command[command.index('-o') + 1] = obj
    command[command.index('-c') + 1] = str(source)
    if '-MF' in command:
        command[command.index('-MF') + 1] = obj + '.d'
    subprocess.run(command, cwd=build, check=True)
    commands = subprocess.check_output(['ninja', '-t', 'commands', 'candidate-bin/Fukami'], cwd=build, text=True)
    line = next(x for x in commands.splitlines() if ' -o candidate-bin/Fukami ' in x)
    command = shlex.split(line)
    start = command.index('&&') + 1 if command[0] == ':' else 0
    end = command.index('&&', start) if '&&' in command[start:] else len(command)
    command = command[start:end]
    command = [x for x in command if not (x.startswith('CMakeFiles/Fukami.dir/') and x.endswith('.o') and x != patch_obj)]
    command.insert(1, obj)
    command[command.index('-o') + 1] = exe
    subprocess.run(command, cwd=build, check=True)
    env = os.environ.copy()
    # Each fixture is an isolated headless runtime, without GUI/host pacing.
    for key in ('RRV_GATE3_REALTIME', 'RRV_GATE4_LAZY_VERIFY', 'RRV_GATE3_TRACE_PC', 'RRV_GATE3_PCHASH'):
        env.pop(key, None)
    results = {}
    states = {}
    for mode in ('typed', 'generic', 'mixed', 'ordered', 'cancel', 'immediate', 'invalidated', 'gs', 'guard'):
        run = subprocess.run([exe, mode], cwd=build, env={**env, **({'RRV_GATE4_LAZY_VERIFY':'1'} if mode == 'gs' else {})}, text=True, capture_output=True)
        expected = 86 if mode == 'guard' else 0
        print(run.stdout, end='')
        print(run.stderr, end='')
        if run.returncode != expected:
            raise SystemExit(f'{mode}: exit{run.returncode}, expected{expected}')
        if mode == 'guard' and 'VERIFY FAILED' not in run.stderr:
            raise SystemExit('stable-generation assertion control did not assert')
        result = re.search(r'^RESULT .+$', run.stdout, re.MULTILINE)
        if result:
            states[mode] = result.group()
        results[mode] = {'returncode': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
    if len(set(states.values())) != 1 or len(states) != 5:
        raise SystemExit('guest state or wake coordinates differ across wait implementations')
    report = {'passed': True, 'scenarios': results, 'matching_guest_state_and_wake': states}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS:9 real-runtime scenarios;5 equivalent guest-state/wake receipts; invariant assertion retained')
