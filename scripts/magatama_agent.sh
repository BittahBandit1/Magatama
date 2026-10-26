#!/bin/zsh -f
# Magatama test runner: lets Claude rebuild and run the game on this Mac without you pasting commands.
#
#   scripts/magatama_agent.sh          (leave it running in its own Terminal tab; Ctrl+C stops it)
#
# It watches ~/magatama/jobs for a file named next.job. A job may only say three things:
#   build=1                 rebuild first (scripts/magatama_build.sh)
#   seconds=<5..900>        how long to let the game run before closing it (SIGTERM = a normal quit)
#   sample=<5..110>         also profile the game with macOS `sample` for 8 s starting this many seconds in
#                           (written to ~/magatama/logs/sample-<id>.txt)
#   replay=<name>           replay recorded input: ~/magatama/replays/<name>.workload (made by magatama_make_replay.py)
#   card=<name>             clone jobs/<name>/memcard-before for this job; never mount the live card
#   capture=<fields>        save these output fields as images, e.g. 3000-3600/60 (to ~/magatama/captures/<id>/)
#   env=<NAME>=<VALUE>      a debug switch; NAME must start with RRV_, MAGATAMA_ or MTL_, VALUE is
#                           letters, digits and , : . _ - only
# Nothing else in the file is accepted, so a job can never run an arbitrary command. Results go to
# ~/magatama/jobs/<id>.done (status lines) and the usual logs (build-magatama.log, ~/magatama/logs).
set -u
repo="${0:A:h:h}"
jobs="$HOME/magatama/jobs"
mkdir -p "$jobs"
agent_lock="$jobs/.agent-lock"
if ! mkdir "$agent_lock" 2>/dev/null; then
    print -u2 "Another agent runner may be active ($agent_lock). Check it before restarting."
    exit 1
fi
print $$ > "$agent_lock/pid"
trap 'rm -f "$agent_lock/pid"; rmdir "$agent_lock" 2>/dev/null' EXIT
stop_at="${MAGATAMA_AGENT_STOP_AT:-0}"
[[ "$stop_at" =~ '^[0-9]+$' ]] || { print -u2 "Invalid MAGATAMA_AGENT_STOP_AT"; exit 1; }
print "magatama agent: watching $jobs (Ctrl+C to stop)"
while true; do
    if (( stop_at > 0 && $(date +%s) >= stop_at )); then print "Agent deadline reached; stopped."; exit 0; fi
    if [[ ! -f "$jobs/next.job" ]]; then sleep 1; continue; fi
    # Do not consume a pending job while another game or build is running.
    if pgrep -f "$repo/build/magatama/bin/Fukami" >/dev/null ||
       pgrep -f '[/]magatama_build.sh' >/dev/null; then sleep 1; continue; fi
    id="$(date +%Y%m%d-%H%M%S)"
    mv "$jobs/next.job" "$jobs/$id.job"
    done_file="$jobs/$id.done"
    build=0; seconds=15; sample_at=0; envs=(); bad=""; card_source=""; job_card=""
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        if [[ "$line" == build=1 ]]; then build=1
        elif [[ "$line" =~ '^seconds=([0-9]+)$' ]] && (( match[1] >= 5 && match[1] <= 900 )); then seconds=$match[1]
        elif [[ "$line" =~ '^replay=([A-Za-z0-9._-]+)$' && -f "$HOME/magatama/replays/$match[1].workload" ]]; then
            envs+=("RRV_GATE3_BOUND_WORKLOAD=$HOME/magatama/replays/$match[1].workload" "RRV_GATE3_LIVE_PAD=0")
        elif [[ "$line" =~ '^capture=([0-9,/-]+)$' ]]; then
            mkdir -p "$HOME/magatama/captures/$id"
            envs+=("RRV_GATE4_GS_CAPTURE_DIR=$HOME/magatama/captures/$id" "RRV_GATE4_GS_CAPTURE_FIELDS=$match[1]")
        elif [[ "$line" =~ '^sample=([0-9]+)$' ]] && (( match[1] >= 5 && match[1] <= 110 )); then sample_at=$match[1]
        elif [[ "$line" =~ '^card=([A-Za-z0-9][A-Za-z0-9_-]*)$' && -d "$jobs/$match[1]/memcard-before/mc0" ]]; then
            card_source="$jobs/$match[1]/memcard-before"
        elif [[ "$line" =~ '^env=((RRV_|MAGATAMA_|MTL_)[A-Z0-9_]+)=([A-Za-z0-9,:._-]*)$' ]]; then envs+=("$match[1]=$match[3]")
        else bad="$line"; break
        fi
    done < "$jobs/$id.job"
    if [[ "${MAGATAMA_AGENT_CARD_ONLY:-0}" == 1 && -z "$card_source" ]]; then bad="A private card= baseline is required for overnight jobs"; fi
    if [[ -n "$bad" ]]; then print "status=rejected\nline=$bad" > "$done_file"; print "[$id] rejected: $bad"; continue; fi
    if [[ -n "$card_source" ]]; then
        job_card="$jobs/$id.memcard"
        if ! mkdir "$job_card" || ! cp -RL "$card_source/mc0" "$job_card/mc0"; then
            print "status=card-copy-failed" > "$done_file"; continue
        fi
        if [[ -d "$card_source/mc1" ]] && ! cp -RL "$card_source/mc1" "$job_card/mc1"; then
            print "status=card-copy-failed" > "$done_file"; continue
        fi
        envs+=("RRV_GATE3_MC_ROOT=$job_card")
    fi
    print "[$id] job: build=$build seconds=$seconds env=${envs[*]:-none}"
    if (( build )); then
        if ! "$repo/scripts/magatama_build.sh" > "$repo/build-magatama.log" 2>&1; then
            print "status=build-failed\nlog=$repo/build-magatama.log" > "$done_file"; print "[$id] build failed"; continue
        fi
    fi
    if (( stop_at > 0 )); then
        remaining=$(( stop_at - $(date +%s) ))
        if (( remaining < 5 )); then print "status=deadline" > "$done_file"; exit 0; fi
        (( seconds > remaining )) && seconds=$remaining
    fi
    before="$(ls -t "$HOME/magatama/logs" 2>/dev/null | head -1)"
    env "${envs[@]}" "$repo/scripts/magatama_run.sh" < /dev/null > /dev/null 2>&1 &
    runner=$!
    if (( sample_at > 0 && sample_at + 8 < seconds )); then
        sleep "$sample_at"
        pid="$(pgrep -f "$repo/build/magatama/bin/Fukami" | head -1)"
        [[ -n "$pid" ]] && sample "$pid" 8 -file "$HOME/magatama/logs/sample-$id.txt" > /dev/null 2>&1
        sleep $(( seconds - sample_at - 8 ))
    else
        sleep "$seconds"
    fi
    pkill -TERM -f "$repo/build/magatama/bin/Fukami" 2>/dev/null
    for i in {1..20}; do kill -0 $runner 2>/dev/null || break; sleep 0.5; done
    if kill -0 $runner 2>/dev/null; then pkill -KILL -f "$repo/build/magatama/bin/Fukami"; result=killed; else result=ok; fi
    wait $runner 2>/dev/null
    log="$(ls -t "$HOME/magatama/logs" | head -1)"
    [[ "$log" == "$before" ]] && log=""
    print "status=$result\nlog=$HOME/magatama/logs/$log\ncard=${job_card:-live-default}" > "$done_file"
    print "[$id] done ($result): $log"
done
