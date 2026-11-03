# Missing battle Bézier functions

The user's native-audio run `run-20261027-043105.log` ends with a fatal call to
`0x17CD90` from `0x17C534`. It repeatedly reports `0x1AC298` missing first. The
scheduler release error appears afterward during exception/shutdown handling.

The locked USA executable contains complete implementations:

- `0x17CD90`, 0x98 bytes: dds3BezierTrackPolySetVC, three-quadword trail history
  update, wrap to the start, current/count progression.
- `0x1AC298`, 0x13c bytes: effPCPBezierEval, cubic Bézier evaluation and segment /
  completion progression, including all original float operations and branches.

Neither entry nor body was emitted in the current generated function set. The
surrounding routines end at these exact addresses. `src/product/nocturne_bezier.cpp`
contains selective translations generated with the product's own vendored
R5900Decoder and CodeGenerator, with runtime checkpoints, exact instruction
accounting and branch delay slots. Product patches register only missing entries.
These are full original functions, not skipped effects or stub success results.

Regenerate from the matching local ELF (SHA256 checked before generation):

```sh
python3 scripts/magatama_generate_bezier.py
```

This reads the existing Fukami generator's cached fmt/rabbitizer dependencies;
no downloads or dependency upgrades. The decoder/emitter sources come from this
runtime's vendored PS2Recomp, preserving its existing translation policies.

Build and check:

```sh
scripts/magatama_build.sh
python3 scripts/magatama_test_bezier.py
```

558 actual-product/runtime checks cover 27 history cases (wrapping/nonwrapping,
count limits, full quadword copies and canaries) and 42 curve cases (independent
double-precision Bernstein reference; segment changes, endpoint completion,
return values, stack/callee preservation and exact branch/delay instruction
counts). The fixture also verifies the entries are absent initially and installed
by actual registerPatches, with the real generated camera function supplied as
an unrelated registration dependency. No GUI, sound device or card is used.

Original-code cross-check: matching Megami-Decomps/smt3-decomp revision
05997a758ae7008a3fc064862db9dcff5772309e, src/smt3/effect/effPCPBezier.c and the
symbol map. The 17CD90 C function agrees with the actual ELF operations; the
new implementation is generated from the original binary, not guessed from
incomplete decompilation. Exact raw instruction hashes and validation are in
outputs/magatama-20261027/battle-bezier-repair-proof.json in the Codex workspace.

The specific user battle/move still needs in-game retesting. Native audio,
previous DMA/FINISH fixes and scheduler guards are unchanged. Other missing
visual callback warnings are outside this bounded repair.


## Follow-up: further battle crash at180BA8

User run run-20261027-113628.log fatally calls180BA8 from the original colour dispatcher1761E0. Restored four additional colour leaves via the existing exact two-instruction template:17FC40(offset54/table64),180BA8(offsetD8/table66),181550(offset58/table67),181CD0(offset48/table84). The same log also reports missing17CB88(reset) and188B40(rotation); the selective generator now emits those exact7/10instruction functions. Original matrix VF28–31 side effects and reset stores/return outputs retained. No skipped effect or forced success.

Expanded tests:610history/curve/reset/rotation checks plus337actual colour/dispatcher checks. Run `MAGATAMA_EFFECT_FIXTURE=nocturne_effect_color_test.cpp python3 scripts/magatama_test_bezier.py` for the latter. Headless pass alone does not prove in-game crash resolution; see the running matrix/proof battle-effects-followup-proof.json in Codex outputs/magatama-20261027 for current validation.


## Complete colour-table audit

The subsequent Xeros-Beat replay114650 completed but warned about another omitted colour leaf194460. `python3 scripts/magatama_audit_effect_colors.py` verifies all106colour slots in the locked USA table31DF90..31F020 against the generated registry and existing product registrations. Sixteen additional exact JRRA/SWA1 leaves(18tablebindings) are emitted into src/product/nocturne_effect_color_leaves.inc. Complex bodies are never guessed;this audit reports any uncovered non-leaf entries separately(currentlyzero). Product registers only absent entries. The same include drives the asset-free actual-dispatch fixture,which now checks24leaves/1009assertions,plus610existingeffectchecks. Largest original offset1334 has full8192-bytecanaries. Current in-game coverage is documented in the external battle-colour-table-audit-proof.json and daytime journal.


## Indirect ring-matrix follow-up

DeadlyFury exposed1824C0,effect object+7Cpoints to its matrix destination. The selective generator now includes this exact11-instruction original body as nocturneRingRotateMatrix. Six overlap/header/VF/GPR/instruction cases bring the noncolour fixture to647checks;with1009colour checks,total1656. Actual8x/native job120051invokes the callback977times and reaches damage/RESULT withoutfatal. The unrelated1667F0roomwarningremains. Generator emits fivecompletefunctions;colourauditscript covers106originalcolour slots. See battle-ring-matrix-proof.json and current coverage matrix in the Codex outputs.


## Observed AI and particle omissions

The repeated encounter also called missing2862D0 (Chk_POW condition,0x88bytes) and162448 (spark-offset scaler,0x4Cbytes). Selective generation now includes these complete original bodies,bringing the helper to seven functions. The matching pinned src/smt3/newbattle/nbAi.c agrees with the original AI mask/flag logic. The product does not force a success return. Test with `MAGATAMA_EFFECT_FIXTURE=nocturne_ai_particle_test.cpp python3 scripts/magatama_test_bezier.py`. It passes4,779checks across512independentAI flag/mask/branch cases and28scale cases;existing effect/color fixtures add1,656checks,total6,435. In8x/native replay124236,theAI/scalerexecute3/8times,enemyCharmBite completes and player control returns without missing warnings/fatal. Full second-battlepostrepairret test is queued;see external battle-ai-particle-proof.json and the running matrix.
