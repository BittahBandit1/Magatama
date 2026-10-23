#!/bin/zsh -f
# Magatama test runner: lets Claude rebuild and run the game on this Mac without you pasting commands.
#
#   scripts/magatama_agent.sh          (leave it running in its own Terminal tab; Ctrl+C stops it)
#
# It watches ~/magatama/jobs for a file named next.job. A job may only say three things:
#   build=1                 rebuild first (scripts/magatama_build.sh)
#   seconds=<5..120>        how long to let the game run before closing it (SIGTERM = a normal quit)
#   env=<NAME>=<VALUE>      a debug switch; NAME must start with RRV_, MAGATAMA_ or MTL_, VALUE is
#                           letters, digits and , : . _ - only
# Nothing else in the file is accepted, so a job can never run an arbitrary command. Results go to
# ~/magatama/jobs/<id>.done (status lines) and the usual logs (build-magatama.log, ~/magatama/logs).
set -u
repo="${0:A:h:h}"
jobs="$HOME/magatama/jobs"
mkdir -p "$jobs"
print "magatama agent: watching $jobs (Ctrl+C to stop)"
while true; do
    if [[ ! -f "$jobs/next.job" ]]; then sleep 1; continue; fi
    id="$(date +%Y%m%d-%H%M%S)"
    mv "$jobs/next.job" "$jobs/$id.job"
    done_file="$jobs/$id.done"
    build=0; seconds=15; envs=(); bad=""
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        if [[ "$line" == build=1 ]]; then build=1
        elif [[ "$line" =~ '^seconds=([0-9]+)$' ]] && (( match[1] >= 5 && match[1] <= 120 )); then seconds=$match[1]
        elif [[ "$line" =~ '^env=((RRV_|MAGATAMA_|MTL_)[A-Z0-9_]+)=([A-Za-z0-9,:._-]*)$' ]]; then envs+=("$match[1]=$match[3]")
        else bad="$line"; break
        fi
    done < "$jobs/$id.job"
    if [[ -n "$bad" ]]; then print "status=rejected\nline=$bad" > "$done_file"; print "[$id] rejected: $bad"; continue; fi
    print "[$id] job: build=$build seconds=$seconds env=${envs[*]:-none}"
    if (( build )); then
        if ! "$repo/scripts/magatama_build.sh" > "$repo/build-magatama.log" 2>&1; then
            print "status=build-failed\nlog=$repo/build-magatama.log" > "$done_file"; print "[$id] build failed"; continue
        fi
    fi
    before="$(ls -t "$HOME/magatama/logs" 2>/dev/null | head -1)"
    env "${envs[@]}" "$repo/scripts/magatama_run.sh" > /dev/null 2>&1 &
    runner=$!
    sleep "$seconds"
    pkill -TERM -f "$repo/build/magatama/bin/Fukami" 2>/dev/null
    for i in {1..20}; do kill -0 $runner 2>/dev/null || break; sleep 0.5; done
    if kill -0 $runner 2>/dev/null; then pkill -KILL -f "$repo/build/magatama/bin/Fukami"; result=killed; else result=ok; fi
    wait $runner 2>/dev/null
    log="$(ls -t "$HOME/magatama/logs" | head -1)"
    [[ "$log" == "$before" ]] && log=""
    print "status=$result\nlog=$HOME/magatama/logs/$log" > "$done_file"
    print "[$id] done ($result): $log"
done
