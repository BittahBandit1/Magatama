#!/bin/zsh -f
# Record the exact first/second-battle route on a private card, for replay.
set -eu
repo="${0:A:h:h}"
project="${repo:h}"
if pgrep -f '[/]magatama/(bin|candidate-bin)/Fukami' >/dev/null; then
    print -u2 "Close Magatama before starting this trace."
    exit 1
fi
[[ ! -e "$project/jobs/next.job" ]] || {
    print -u2 "A runner job is pending; let it finish first."
    exit 1
}
[[ -d "$project/memcard/mc0" ]] || {
    print -u2 "No live card at $project/memcard/mc0"
    exit 1
}
mkdir -p "$project/jobs"
trace_session="$(mktemp -d "$project/jobs/audio-first-$(date +%Y%m%d-%H%M%S).XXXXXX")"
mkdir "$trace_session/memcard-before" "$trace_session/memcard-play"
python3 - "$project/memcard" "$trace_session/live-card-before.json" <<'PY'
import hashlib,json,sys
from pathlib import Path
card=Path(sys.argv[1])
hashes={str(p.relative_to(card)):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in card.rglob('*') if p.is_file()}
Path(sys.argv[2]).write_text(json.dumps(hashes,indent=2)+'\n')
PY
for trace_port in mc0 mc1; do
    if [[ -d "$project/memcard/$trace_port" ]]; then
        # Dereference links; the mounted copy cannot point into the live card.
        cp -RL "$project/memcard/$trace_port" "$trace_session/memcard-before/$trace_port"
        cp -RL "$trace_session/memcard-before/$trace_port" "$trace_session/memcard-play/$trace_port"
    fi
done
# Pin the real launch date once so the eventual replay uses the same clock.
python3 - "$repo/config/magatama/bound-workload.txt" "$trace_session/base.workload" <<'PY'
from pathlib import Path
import datetime,sys,time
lines=Path(sys.argv[1]).read_text().splitlines()
assert lines[-1].startswith('end ')
epoch=int(time.time())
offset=int(datetime.datetime.now().astimezone().utcoffset().total_seconds()/60)
body=[]
for line in lines[:-1]:
    if line.startswith('rtc_source '): continue
    if line.startswith('rtc_epoch '): line=f'rtc_epoch {epoch}'
    if line.startswith('rtc_offset_minutes '): line=f'rtc_offset_minutes {offset}'
    body.append(line)
Path(sys.argv[2]).write_text('\n'.join(body+[f'end {len(body)}'])+'\n')
PY
finish_trace() {
    local trace_status=$?
    trap - EXIT
    python3 - "$trace_session" "$project/memcard" <<'PY'
from pathlib import Path
import hashlib,json,sys
session,card=map(Path,sys.argv[1:])
before=json.loads((session/'live-card-before.json').read_text())
after={str(p.relative_to(card)):hashlib.sha256(p.read_bytes()).hexdigest()
       for p in card.rglob('*') if p.is_file()}
receipt={'live_card_unchanged':before==after,
         'changed':[k for k in sorted(set(before)|set(after)) if before.get(k)!=after.get(k)]}
(session/'live-card-verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
pad=session/'pad.jsonl'
if pad.exists():
    rows=[json.loads(line) for line in pad.read_text().splitlines() if line.strip()]
    assert all(rows[i]['cycle']>=rows[i-1]['cycle'] for i in range(1,len(rows)))
    base=(session/'base.workload').read_text().splitlines()
    assert base[-1].startswith('end ')
    body=base[:-1]+[f"pad {r['cycle']} {r['sequence']} {r['port']} {r['sample']}" for r in rows]
    (session/'replay.workload').write_text('\n'.join(body+[f'end {len(body)}'])+'\n')
    print(f'== recorded {len(rows)} pad changes: {session}/replay.workload')
print(f'== live card unchanged: {receipt["live_card_unchanged"]}')
print(f'== trace session: {session}')
PY
    return "$trace_status"
}
trap finish_trace EXIT
export RRV_GATE3_MC_ROOT="$trace_session/memcard-play"
export RRV_GATE3_BOUND_WORKLOAD="$trace_session/base.workload"
export RRV_GATE3_RECORD_PAD="$trace_session/pad.jsonl"
export MAGATAMA_SOUND_NATIVE=1
export MAGATAMA_AUDIO_TRACE=1
export MAGATAMA_PRESENT_TRACE=1
export MAGATAMA_START_CLOCK=1
print "== trace session: $trace_session"
print "== PRIVATE card copy; any saves here will not update your live progress."
print "== Load the same slot, play two battles, then close normally."
"$repo/scripts/magatama_run.sh" "$@"
