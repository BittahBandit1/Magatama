# Original Nocturne sound-driver bring-up

This is the first integration stage for the DQ8 Recompiled audio approach. It
runs the user's original IRX binaries on the pinned DQ8 R3000A interpreter,
kernel and SPU2 implementation. The standalone diagnostic validates the core.
The opt-in in-game bridge is in `src/audio/nocturne_native_sound.cpp`; the
existing HLE audio remains the default.

To try native game audio:

```sh
scripts/magatama_build.sh
MAGATAMA_SOUND_NATIVE=1 scripts/magatama_run.sh
```

Omit `MAGATAMA_SOUND_NATIVE` to use the existing HLE backend. Optional
`MAGATAMA_NATIVE_CAPTURE=1` records a per-run PCM WAV beside the normal log.

Run from the runtime directory:

```sh
scripts/magatama_audio_probe.sh /tmp/magatama-native-title.wav
afplay /tmp/magatama-native-title.wav
```

The first command builds only `build/nocturne-iop`, checks the matching USA ELF,
temporarily reads MIDI003A.SMG from the local DDS3 archive, runs two five-second
renders, compares them byte for byte, and checks the original driver's invalid
bank error path. The temporary bank copies are removed automatically. The only
retained media is the requested WAV. Neither command starts the game or accesses
a memory card. `MAGATAMA_DISC_ROOT` can select another extracted disc directory.

The probe starts SDFIOPFL, LIBSD, MODHSYN, MODMIDI, MODMSIN and SDFSDMAN, then sends
real initialization, bank-load and BGM SIF packets. SDFSDMAN runs its original
dispatcher, sequencer, synthesizer and DMA code. A small diagnostic EE endpoint
serves the one private bank through the original SDFIOPFL file-RPC protocol. It
does not replace the synthesizer or decode the SQ/HD bank on the host.

The work-area setup follows the USA EE code at 0x226318, 0x2c13d8, 0x2c1490 and
0x2c1578. Four program-buffer sizes are independently read from the matching
executable's table at 0x342fc0. No game code or assets are included in this tree.

## Verified October 27

- All six original IRX modules start; zero unknown imports or CPU faults.
- Original command handler sends its ready acknowledgement and three matching
  completion tags: 0x1234, 0x1235 and 0x1236.
- Original file path completes 31 EE RPCs, reading the complete 405840-byte bank.
- Five seconds / 240000 stereo frames at 48 kHz, signed 16-bit PCM. 199466 frames
  contain nonzero samples; peak 18118, RMS 1747.522. Both renders are identical.
- Original driver rejects an empty 176-byte bank and posts one error.

These prove headless original-driver execution and PCM generation. Listening
quality, comparison with hardware, sound effects, APCM movies, latency and
in-game behavior remain unverified.

## In-game bridge

The bridge overrides only the matching USA SIF entry points when selected.
EE IOP allocations return actual separate IOP addresses; SIF transfers validate
all descriptors before copying and never alias the low EE RAM. It preserves the
existing HLE route when disabled. The original driver supplies ready/completion
acknowledgements and shared-work-area status; native mode does not install the
silent bank completion or movie-ring drain.

Incoming IOP RPCs are queued to the game's registered file/SNRT server threads.
Those threads use the normal SleepThread/iWakeupThread protocol rather than
arbitrary readiness predicates. Handlers run with their original EE GP on their
own thread, including real blocking disc/provider calls. No callback is invoked
on SDL or while the R3000A interpreter is active. Guest EE cycles drive 48kHz
sample generation; SDL pulls a bounded nonblocking PCM ring only.

In-game validation October27: private-card boot, original on-memory APCM stream
requests (SNRT6/7/8), menu music, native save load to Underground Facility and
battle completion, including damage and results. Original driver also completes
memory-bank requests (SNRT3/4/5) and file APCM streams (SNRT0/1/2).
The first polling-wait implementation halved game throughput. Replacing it with
real queue sleep/wakeup increased the same75second replay from2155 to4305fields
and reduced recorded SDL underruns from229099 to6372frames. Matching field1500
is byte-identical. This is integration validation, not a claim that all pacing,
latency, clipping, listening quality or full-game audio has been resolved.

284 asset-free real-runtime SIF boundary tests pass. Run:

```sh
python3 scripts/magatama_test_native_audio.py
```

Current proof index and later battle-action results are in the project handoff
and OVERNIGHT_STATUS.md. Tests use copied memory cards; never save to the live
card during automation. No dependency upgrades or graphics changes are needed.

The diagnostic endpoint advertises the original SNRT service for binding but
deliberately rejects actual SNRT requests; this test covers SDFIOPFL RPC, not
movie streaming. Unknown kernel imports and CPU faults mark the core unhealthy
and prevent module bring-up from being reported as successful.

Upstream revisions, license and the local changes are recorded in
`third_party/dq8-iop/PROVENANCE.md` and `upstream.json`.

## October 28 playback jitter update

The original PCM producer remains guest-clocked. Its SDL ring now uses 4096 frames of prefill (85.3ms), adding 42.7ms over the prior target. This protects against short host production stalls without resampling or altering PCM. `MAGATAMA_AUDIO_BUFFER_MS=22..170` selects a target; periodic native logs report queued frames, underrun events/frames, priming silence and overruns. The actual ring passes 132,107 sanitized assertions; a synthetic 40ms jitter schedule records 786 old underrun frames versus zero new. This is offline validation, not a listening-verified fix for every stutter. A separate `MAGATAMA_MAC_QOS=1` opt-in requests user-interactive scheduling on native audio-producing EE threads and GS/VU workers, leaving CoreAudio's policy unchanged. It is off by default and has no measured gameplay speedup yet. Proof and reproducible commands are in Codex outputs/magatama-audio-20261028/audio-fix-report.md.
