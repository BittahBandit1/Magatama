# DQ8 original-driver IOP core

Source: https://github.com/Sinan-Karakaya/PS2Recomp/tree/888ec479165f41b899bb081e0ca7123b8c5af31a/ps2xIOP/src/lle
Pinned by DQ8-Recompiled f6dfd968001b492415969bb70bd5aad3bfa79abd.
License: GNU GPL version3, retained in LICENSE. Original comments remain in each source.

Only the standalone R3000A/IRX/kernel/SPU2 core is imported. The existing Magatama runtime, renderer, recompiler, dependencies and Claude audio driver are not replaced. No game modules, samples or assets are distributed; tests read the user's local disc. upstream.json records hashes of the initial unmodified sources. Magatama-specific changes will be documented here.

## Magatama changes, October 27, 2026

Modified `lle/iop.h`, `lle/iop.cpp`, `lle/kernel.cpp` and
`lle/kernel_handlers.inc` only. CPU instruction implementation and SPU2 DSP
remain the pinned upstream versions.

- Added IOP SIF command buffer/handler registration, inbound interrupt dispatch
  and outbound command delivery through the EE owner interface.
- Implemented IOP-to-EE RPC requests, client bindings, busy polling, blocking
  thread waits, reply copies and completion callbacks. The owner completes
  requests on the same guest thread; no host timeout or audio callback drives
  guest state.
- Added `thbase` ordinal 43 (GetSystemTimeLow), `sysclib` ordinal 42 (the bounded
  subset of vsprintf used by these modules), and SIF bootstrap receive address.
  Ordinals checked against PS2SDK's kernel export tables; unsupported formatting
  conversions fail rather than silently succeeding.
- Module-loader waits now advance guest time so a blocking EE bind can resume.
  Unknown imports, CPU faults and failed/timed-out module starts mark the core
  unhealthy. Handler failures also fail the health check.

Protocol reference: matching Megami-Decomps/smt3-decomp revision
05997a758ae7008a3fc064862db9dcff5772309e, `src/irx/SDFIOPFL.c`,
`src/irx/SDFSDMAN.c`, corresponding headers and the actual USA ELF/IRX binaries.
Some decompilation paths are nonmatching; the integration test executes the
original binaries. See `tools/nocturne-iop/README.md` for validation and limits.

Further in-game bridge support: exposed checked IOP allocation ownership; zero/oversized allocations fail before rounding; remember bound EE service IDs while their servers register. The native product bridge runs original EE RPC server threads with normal kernel sleep/wakeup and routes SIF DMA between genuinely separate EE and IOP RAM. No runtime/scheduler guard or upstream CPU/SPU2 DSP was modified.
