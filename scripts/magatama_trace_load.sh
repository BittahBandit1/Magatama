#!/bin/zsh -f
# Reproduce a save-load hang on a private card copy; keep the live card intact.
set -eu
repo="${0:A:h:h}"
project="${repo:h}"
if pgrep -f '[/]magatama/bin/Fukami' >/dev/null; then
    print -u2 "Close the existing Magatama game before starting the load trace."
    exit 1
fi
[[ -d "$project/memcard/mc0" ]] || { print -u2 "No memory card at $project/memcard/mc0"; exit 1; }
mkdir -p "$project/jobs"
trace_card="$(mktemp -d "$project/jobs/load-trace-$(date +%Y%m%d-%H%M%S).XXXXXX")"
# Copy file contents so the diagnostic cannot follow a card symlink back to
# the live card. No journal is imported or restored into the user's card.
for port in mc0 mc1; do
    if [[ -d "$project/memcard/$port" ]]; then
        cp -RL "$project/memcard/$port" "$trace_card/$port"
    fi
done
export RRV_GATE3_MC_ROOT="$trace_card"
export RRV_GATE8_MC_TRACE=1
export MAGATAMA_TRACE_IO=1
export MAGATAMA_TRACE_LOADWAIT=1
export RRV_FN_HITS=151918,151960,1519a0,153010,1530f0,153168,1531c8,1553c8,1554b0,155558,1555c0,29b780
export MAGATAMA_FN_TRACE=1
export MAGATAMA_FN_SLOW_US=10000
print "== diagnostic card copy: $trace_card"
print "== Load the same slot. If it hangs, leave the window open for inspection."
exec "$repo/scripts/magatama_run.sh" "$@"
