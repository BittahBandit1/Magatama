#!/usr/bin/env python3
"""Turn a recorded pad log into a replay workload the runtime can play back unattended.

    python3 scripts/magatama_make_replay.py <pad.jsonl> <name>

Record with RRV_GATE3_RECORD_PAD=$HOME/magatama/replays/<name>.jsonl on a normal run. The replay is the normal
bound workload plus one 'pad' line per recorded change, written to ~/magatama/replays/<name>.workload; the agent
runs it with `replay=<name>`.
"""
import json, pathlib, sys
repo = pathlib.Path(__file__).resolve().parents[1]
src, name = pathlib.Path(sys.argv[1]), sys.argv[2]
base = (repo / "config/magatama/bound-workload.txt").read_text().splitlines()
assert base[-1].startswith("end "), "base workload must end with 'end <n>'"
pads = []
for line in src.read_text().splitlines():
    if line.strip():
        r = json.loads(line)
        pads.append(f"pad {r['cycle']} {r['sequence']} {r['port']} {r['sample']}")
body = base[:-1] + pads
out = pathlib.Path.home() / "magatama/replays" / f"{name}.workload"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(body + [f"end {len(body)}"]) + "\n")
print(f"{len(pads)} pad changes -> {out}")
