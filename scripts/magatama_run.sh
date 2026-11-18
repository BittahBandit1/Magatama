#!/bin/zsh -f
# Run the Magatama build (Nocturne) straight from the build tree, the way Fukami.app launches its game process.
#   scripts/magatama_run.sh [disc-folder]     (default ~/magatama/disc, which must hold SLUS_209.11 and the
#                                              rest of the disc's files: the ELF's folder is the cdrom0: root)
# Output goes to the terminal and to ~/magatama/logs/run-<time>.log. Extra RRV_* variables pass through.
set -eu
repo="${0:A:h:h}"
disc="${1:-$HOME/magatama/disc}"
[[ -f "$disc/SLUS_209.11" ]] || { print -u2 "no SLUS_209.11 in $disc"; exit 1; }
mkdir -p "$HOME/magatama/memcard" "$HOME/magatama/logs"
log="$HOME/magatama/logs/run-$(date +%Y%m%d-%H%M%S).log"
# Read-only guest RAM capture, with a per-run path usable by bounded jobs.
if [[ -n "${MAGATAMA_RAM_DUMP_FIELD:-}" ]]; then
    [[ "$MAGATAMA_RAM_DUMP_FIELD" =~ '^[1-9][0-9]*$' ]] || { print -u2 "Invalid RAM dump field"; exit 1; }
    export RRV_GATE3_DUMP_RAM="$MAGATAMA_RAM_DUMP_FIELD:${log%.log}.ram"
fi
# First-approach door hitch diagnostics: selected loading/door/decoder calls,
# slow receipts only, plus actual presentation timestamps. Off by default.
if [[ "${MAGATAMA_PROFILE_DOORS:-0}" == 1 ]]; then
    export RRV_FN_HITS="${RRV_FN_HITS:-127420,127580,1279e8,127bc8,143028,143e20,144428,2bdb80,2bdbd0,2c62a0}"
    export MAGATAMA_FN_SLOW_US="${MAGATAMA_FN_SLOW_US:-2000}"
    export MAGATAMA_PRESENT_TRACE="${MAGATAMA_PRESENT_TRACE:-1}"
fi
# Optional Metal presentation timestamps (submit, target, actual on-screen time).
# Use a per-run path so unattended jobs can request tracing without supplying paths.
[[ "${MAGATAMA_PRESENT_TRACE:-0}" == 1 ]] && export RRV_PCSX2_GS_PRESENT_TRACE="${log%.log}.present.txt"
[[ "${MAGATAMA_GS_COUNTERS:-0}" == 1 ]] && export RRV_GATE4_GS_COUNTERS="${log%.log}.gs-counters.txt"
[[ "${MAGATAMA_START_CLOCK:-0}" == 1 ]] && export RRV_GATE4_START_CLOCK="${log%.log}.start-clock.txt"
[[ "${MAGATAMA_NATIVE_CAPTURE:-0}" == 1 ]] && export MAGATAMA_NATIVE_WAV="${log%.log}.native.wav"
[[ "${MAGATAMA_AUDIO_OUTPUT_TRACE:-0}" == 1 ]] && export MAGATAMA_AUDIO_OUTPUT_PROBE="${log%.log}.audio-output.bin"
[[ "${MAGATAMA_TRACE_FMV:-0}" == 1 ]] && export MAGATAMA_FMV_TRACE="${log%.log}.fmv.txt"
[[ "${MAGATAMA_TRACE_RENDER_POSES:-0}" == 1 ]] && export MAGATAMA_RENDER_POSE_TRACE="${log%.log}.render-poses.bin"
[[ "${MAGATAMA_TRACE_ORIGINAL_FRAMES:-0}" == 1 ]] && export MAGATAMA_ORIGINAL_FRAME_TRACE="${log%.log}.original-frames.txt"
[[ "${MAGATAMA_TRACE_ORIGINAL_PRESENT:-0}" == 1 ]] && export MAGATAMA_ORIGINAL_PRESENT_TRACE="${log%.log}.original-present.txt"
[[ "${MAGATAMA_GS_TRACE:-0}" == 1 ]] && export RRV_GS_TRACE="${log%.log}.gs"
# Narrow per-draw diagnostics; select frames with RRV_PCSX2_GS_DRAWDUMP_FRAMES.
if [[ "${MAGATAMA_DRAW_DUMP:-0}" == 1 ]]; then
    export RRV_PCSX2_GS_DRAWDUMP="${log%.log}.draws"
    mkdir -p "$RRV_PCSX2_GS_DRAWDUMP"
fi
export RRV_GATE3_BOUND_WORKLOAD="${RRV_GATE3_BOUND_WORKLOAD:-$repo/config/magatama/bound-workload.txt}"
export RRV_GATE3_MC_ROOT="${RRV_GATE3_MC_ROOT:-$HOME/magatama/memcard}"
export RRV_GATE3_MC_PERSISTENT=1
export RRV_GATE3_LIVE_PAD="${RRV_GATE3_LIVE_PAD:-1}"
export RRV_GIF_PATH_LATENCY="${RRV_GIF_PATH_LATENCY:-0}"   # the RR5 hold keeps Nocturne's FINISH packet from the GS   # 0 for a replay (the workload carries the pad)
export RRV_GATE3_REALTIME=1
# No IOP sound driver yet: without the silent stand-in the opening movie's audio ring never drains (the movie
# never shows) and the title screen waits on sound status. MAGATAMA_SILENT_SOUND=0 restores the old path.
export MAGATAMA_SILENT_SOUND="${MAGATAMA_SILENT_SOUND:-1}"
# Sound: the host SDFSDMAN driver (src/audio/nocturne_sound.cpp) plays the game's music and effects and answers
# its bank/stream status; when it runs, the silent bank stand-in above is not used. MAGATAMA_SOUND=0 turns it off.
export MAGATAMA_SOUND="${MAGATAMA_SOUND:-1}"
# The game's own IOP sound modules (music, effects, movie audio; src/audio/nocturne_native_sound.cpp), as on iOS and
# Android. MAGATAMA_SOUND_NATIVE=0 falls back to the host SDFSDMAN driver above (no movie audio).
export MAGATAMA_SOUND_NATIVE="${MAGATAMA_SOUND_NATIVE:-1}"
# Keep time-sensitive EE/GS work responsive while system overlays (Spotlight
# typing) compete for CPU. Per-thread QoS only; no global OS policy or affinity.
# MAGATAMA_MAC_QOS=0 restores inherited scheduling for comparison.
export MAGATAMA_MAC_QOS="${MAGATAMA_MAC_QOS:-1}"
export MAGATAMA_DISC="$disc"
# Producer owns GS SIGNAL/FINISH/LABEL; the renderer does not generate these.
export RRV_GS_CONTROL="${RRV_GS_CONTROL:-producer}"
export RRV_PCSX2_GS_VSYNC=1
# Optional smoother presentation, validated with scale-4 day1 replays. More
# headroom reduces uneven display intervals but can add about one frame of
# latency. Keep the normal latency setting unless explicitly requested.
if [[ "${MAGATAMA_SMOOTH_PACING:-0}" == 1 ]]; then
    export RRV_PCSX2_GS_PRESENT_MARGIN_US="${RRV_PCSX2_GS_PRESENT_MARGIN_US:-10000}"
    export RRV_PCSX2_GS_PRESENT_CREEP_US="${RRV_PCSX2_GS_PRESENT_CREEP_US:-0}"
fi
# Fukami.app's defaults: render the GS on its own worker thread and run VU1 asynchronously, so the cost of drawing a
# frame does not land on the game thread and make frame times uneven. RRV_GS_EXECUTION=inline restores the old path.
export RRV_GS_EXECUTION="${RRV_GS_EXECUTION:-worker-sync}"
[[ "$RRV_GS_EXECUTION" == inline ]] && unset RRV_GS_EXECUTION || export RRV_VU1GS_EXECUTION="${RRV_VU1GS_EXECUTION:-owner-async}"
# Render mode and FXAA defaults are resolved by the product after saved settings.
# The product reads the saved scale, falling back to 8x. An explicit
# RRV_PCSX2_GS_FULL_SCALE still overrides it (including isolated test jobs).
export RRV_FUKAMI_INI="${RRV_FUKAMI_INI:-$HOME/magatama/magatama.ini}"
if [[ -n "${MAGATAMA_MENU_TEST:-}" ]]; then
    [[ "$MAGATAMA_MENU_TEST" == resolution || "$MAGATAMA_MENU_TEST" == rendering || "$MAGATAMA_MENU_TEST" == audit || "$MAGATAMA_MENU_TEST" == observe || "$MAGATAMA_MENU_TEST" == audio ]] || {
        print -u2 "Invalid MAGATAMA_MENU_TEST"; exit 1; }
    test_ini="${log%.log}.settings.ini"
    if [[ -f "$RRV_FUKAMI_INI" ]]; then cp "$RRV_FUKAMI_INI" "$test_ini"; else print '' > "$test_ini"; fi
    export RRV_FUKAMI_INI="$test_ini"
    # User preference: automatic tests start windowed, regardless of a saved
    # fullscreen setting or an inherited Terminal override.
    export RRV_WINDOW_FULLSCREEN=0
fi
# Explicit RRV_PCSX2_GS_FXAA still overrides the saved choice.
# PCSX2 Metal on Apple GPUs routes depth through the pixel shader (+/- 1 ulp workaround). With it, Nocturne's
# dialogue band and text (both Z=0xfffff0, ZTST GEQUAL) land 1 apart and every glyph fails the depth test, so
# in-game text never draws. Hardware depth keeps equal Z equal. MTL_SHADER_DEPTH_WORKAROUND=1 restores PCSX2's default.
export MTL_SHADER_DEPTH_WORKAROUND="${MTL_SHADER_DEPTH_WORKAROUND:-0}"
# Decode movies through the software IPU instead of placeholder MMIO results.
export MAGATAMA_IPU="${MAGATAMA_IPU:-1}"
# Original IPU worker deadlines and independent masked DMAC admission.
# Repeated intro/DDS and field/battle regressions passed; explicit0 restores
# the earlier synchronous IPU path without changing packet-read ahead.
export MAGATAMA_IPU_WORKER_TIMING="${MAGATAMA_IPU_WORKER_TIMING:-1}"
# Native VU catalogue validated against hospital/day1 replays; 0 restores interpretation.
export RRV_VU_AOT="${RRV_VU_AOT:-1}"
# macOS Metal performance HUD (the per-app toggle only applies to .app bundles; this is a bare executable).
# Preserve an explicit HUD override; otherwise startup honors the saved menu
# choice, with the existing on-by-default behavior when no choice is saved.
if [[ -n "${MAGATAMA_METAL_HUD:-}" ]]; then
    export MTL_HUD_ENABLED="$MAGATAMA_METAL_HUD"
fi
print "== log: $log"
# The runtime finds the PCSX2 GS bridge package only from the fixed layout B/bin/<program> (with the package in
# B/.rrv-resource-packages), so run a copy placed in build/magatama/bin rather than candidate-bin itself.
build="$repo/build/magatama"
mkdir -p "$build/bin"
if [[ ! -f "$build/bin/Fukami" || "$build/candidate-bin/Fukami" -nt "$build/bin/Fukami" ]]; then
    # Replace the file rather than overwrite it in place: macOS caches a binary's code signature per file, and an
    # in-place overwrite makes the kernel kill the new build at launch (zsh: killed, exit 137).
    rm -f "$build/bin/Fukami"
    cp -p "$build/candidate-bin/Fukami" "$build/bin/Fukami"
    codesign --force --sign - "$build/bin/Fukami" 2>/dev/null || true
fi
# Restart supervisor: 75 is requested only after confirmed menu Restart and
# normal runtime/GS/window teardown. Re-exec this launcher after the pipeline
# has drained, so there is never a second live game and each launch gets a log.
export MAGATAMA_RESTART_LAUNCHER=1
set +e
"$build/bin/Fukami" "$disc/SLUS_209.11" 2>&1 | tee "$log"
run_status=("${pipestatus[@]}")
set -e
if [[ "${run_status[1]}" == 75 && "${run_status[2]}" == 0 ]]; then
    exec "$repo/scripts/magatama_run.sh" "$disc"
fi
# Preserve failures from the game (or tee) instead of treating a crash as a restart.
[[ "${run_status[1]}" == 0 ]] || exit "${run_status[1]}"
exit "${run_status[2]}"
