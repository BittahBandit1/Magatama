// patches.cpp: the Magatama override layer for Shin Megami Tensei: Nocturne (USA, SLUS_209.11).
//
// This replaces Fukami's Ridge Racer V override layer (see git history for the RR5 version). It is the
// only place game behaviour changes; generated/nocturne/output is never edited. main_product.cpp calls
// registerPatches() after the generated functions are registered.
//
// What it does today:
//   1. Binds Sony library entry points the recompiler did not emit a body for (nocturne_stub_addrs.inc)
//      to the runtime's ps2_stubs:: handlers, through a trampoline that returns to $ra like the
//      generated stub wrappers do. Kept from Fukami's layer unchanged in behaviour.
//   2. RRV_FN_HITS=<hex>[,<hex>...] | all: counts entries into guest functions and prints them at exit,
//      to answer "does Nocturne ever reach this routine?" while bringing the game up.
// Nocturne-specific fixes are added below as the boot log reveals them.

#include "patches.h"
#include "rrv_gs_trace_hooks.h"
#include "nocturne_silent_sound.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "rrv_guest_scheduler.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── Library-import stubs without generated bodies ─────────────────────────
struct MagatamaBoundStub { PS2Runtime::RecompiledFunction fn; const char* name; };
static std::unordered_map<uint32_t, MagatamaBoundStub>& magatamaStubMap() {
    static std::unordered_map<uint32_t, MagatamaBoundStub> m;
    return m;
}

static void magatamaStubTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    auto& m = magatamaStubMap();
    if (auto it = m.find(entry); it != m.end() && it->second.fn) {
        const auto& bound = it->second;
        std::string costName = "ps2_stubs::";
        costName += bound.name;
        if (std::string_view{bound.name} == "sceGsSyncV") {
            bound.fn(rdram, ctx, runtime); // charge only after the modelled wait returns
            runtime->gate3ChargeNamedHleV1(ctx, costName.c_str());
        } else {
            runtime->gate3ChargeNamedHleV1(ctx, costName.c_str());
            bound.fn(rdram, ctx, runtime);
        }
    }
    if (ctx->pc == entry) {
        ctx->pc = GPR_U32(ctx, 31); // jr $ra
    }
}

static PS2Runtime::RecompiledFunction magatamaResolveStub(std::string_view n) {
#define MAGATAMA_RESOLVE_STUB(name) if (n == std::string_view{#name}) return &ps2_stubs::name;
    PS2_STUB_LIST(MAGATAMA_RESOLVE_STUB)
#undef MAGATAMA_RESOLVE_STUB
    return nullptr;
}

// The callback table at 0x31DFAC (entry 57, slot 0x31E894) references
// 0x193478. Generation folded this leaf into entry_00193468 after that
// routine's return, leaving it unreachable through the function dispatcher.
// Preserve the original two instructions, including the store in the JR delay
// slot; the locked SLUS_209.11 contains 03E00008 AC850024 at this address.
static void nocturneCallback193478(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x193478u; // jr $ra
    runtime->gate3CheckpointV1(ctx);
    runtime->gate3BeginInstructionV1(ctx);
    const uint32_t target = GPR_U32(ctx, 31);
    ctx->pc = 0x19347Cu;
    ctx->in_delay_slot = true;
    ctx->branch_pc = 0x193478u;
    runtime->gate3BeginInstructionV1(ctx);
    WRITE32(ADD32(GPR_U32(ctx, 4), 36), GPR_U32(ctx, 5)); // sw $a1,0x24($a0)
    ctx->in_delay_slot = false;
    ctx->pc = target;
}

// Additional colour leaves omitted by the same discovery gap as193478.
// The USA ELF contains JR $ra; SW $a1,offset($a0) at each entry. Restore
// callable entries, including the delay-slot store; never call the enclosing
// position setter, whose earlier return makes these instructions unreachable.
template<uint32_t Entry, uint32_t Offset>
static void nocturneEffectColorCallback(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = Entry;
    runtime->gate3CheckpointV1(ctx);
    runtime->gate3BeginInstructionV1(ctx);
    const uint32_t target = GPR_U32(ctx, 31);
    ctx->pc = Entry + 4;
    ctx->in_delay_slot = true;
    ctx->branch_pc = Entry;
    runtime->gate3BeginInstructionV1(ctx);
    WRITE32(ADD32(GPR_U32(ctx, 4), Offset), GPR_U32(ctx, 5));
    ctx->in_delay_slot = false;
    ctx->pc = target;
}

// ── RRV_FN_HITS: guest-function entry counter (default off) ───────────────
namespace {
std::map<uint32_t, PS2Runtime::RecompiledFunction>& fnHitOrig() {
    static std::map<uint32_t, PS2Runtime::RecompiledFunction> m;
    return m;
}
std::map<uint32_t, std::atomic<uint64_t>>& fnHitCount() {
    static std::map<uint32_t, std::atomic<uint64_t>> m;
    return m;
}
std::map<uint32_t, std::atomic<uint64_t>>& fnHitNs() {   // host time spent inside (including callees)
    static std::map<uint32_t, std::atomic<uint64_t>> m;
    return m;
}
void fnHitTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    if (auto it = fnHitCount().find(entry); it != fnHitCount().end()) {
        it->second.fetch_add(1, std::memory_order_relaxed);
    }
    // MAGATAMA_FN_TRACE=1 also logs each entry with the guest frame (first 6000 entries).
    static const bool fnTrace = [] { const char* v = std::getenv("MAGATAMA_FN_TRACE"); return v && v[0] == '1'; }();
    static std::atomic<uint64_t> fnTraced{0};
    if (fnTrace && fnTraced.fetch_add(1, std::memory_order_relaxed) < 200000) {
        std::fprintf(stderr, "[fn] f=%llu pc=%08x a0=%08x ra=%08x\n",
            (unsigned long long)runtime->gate3TemporalV1().starts(), entry, GPR_U32(ctx, 4), GPR_U32(ctx, 31));
    }
    if (auto it = fnHitOrig().find(entry); it != fnHitOrig().end() && it->second) {
        // Optional slow-call receipts for selected RRV_FN_HITS functions. These
        // are inclusive wall times (including nested calls and guest waits),
        // not exclusive CPU samples. Never emit a line for every frame.
        static const uint64_t slowUs = [] {
            const char* v = std::getenv("MAGATAMA_FN_SLOW_US");
            return v ? std::strtoull(v, nullptr, 10) : 0ull;
        }();
        static std::atomic<uint32_t> slowLogged{0};
        const uint64_t firstField = slowUs ? runtime->gate3TemporalV1().starts() : 0;
        const uint32_t caller = slowUs ? GPR_U32(ctx, 31) : 0;
        const int guestContext = rrv::guest_time::context_id;
        const auto t0 = std::chrono::steady_clock::now();
        it->second(rdram, ctx, runtime);
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        fnHitNs()[entry].fetch_add(elapsedNs, std::memory_order_relaxed);
        if (slowUs && elapsedNs / 1000 >= slowUs &&
            slowLogged.fetch_add(1, std::memory_order_relaxed) < 2048) {
            std::fprintf(stderr,
                "[fn-slow] pc=%08x ra=%08x ctx=%d fields=%llu-%llu host_begin_us=%llu elapsed_us=%llu inclusive=1\n",
                entry, caller, guestContext, static_cast<unsigned long long>(firstField),
                static_cast<unsigned long long>(runtime->gate3TemporalV1().starts()),
                static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                    t0.time_since_epoch()).count()), static_cast<unsigned long long>(elapsedNs / 1000));
        }
    } else if (ctx->pc == entry) {
        std::fprintf(stderr, "[fn-hits] %08x has no body to forward to\n", entry);
        ctx->pc = GPR_U32(ctx, 31);
    }
}
void dumpFnHits() {
    uint64_t total = 0;
    for (const auto& [addr, count] : fnHitCount()) {
        const uint64_t c = count.load(std::memory_order_relaxed);
        total += c;
        if (c) std::fprintf(stderr, "[fn-hits] %08x %llu  %.1f ms total\n", addr, static_cast<unsigned long long>(c),
                            fnHitNs()[addr].load(std::memory_order_relaxed) / 1e6);
    }
    std::fprintf(stderr, "[fn-hits] %zu armed, %llu entries\n", fnHitCount().size(),
                 static_cast<unsigned long long>(total));
}
void armFnHits(PS2Runtime& runtime, const char* spec) {
    std::vector<uint32_t> addrs;
    if (std::strcmp(spec, "all") == 0) {
        addrs = runtime.allFunctionAddresses();
    } else {
        for (const char* p = spec; *p;) {
            char* end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p) break;
            addrs.push_back(addr);
            p = (*end == ',') ? end + 1 : end;
        }
    }
    for (const uint32_t addr : addrs) {
        if (!addr) continue;
        fnHitOrig()[addr] = runtime.hasFunction(addr) ? runtime.lookupFunction(addr) : nullptr;
        (void)fnHitCount()[addr];
        (void)fnHitNs()[addr];
        runtime.registerFunction(addr, fnHitTrampoline);
    }
    std::atexit(dumpFnHits);
    std::fprintf(stderr, "[fn-hits] armed %zu function(s)\n", addrs.size());
}
} // namespace

// ── Nocturne fixes ────────────────────────────────────────────────────────

// 0x2CB950 sceCdLayerSearchFile(sceCdlFILE* fp, const char* name, int layer). Nocturne ships on a DVD and uses the
// dual-layer search; its library body asks the IOP's CD search RPC service, which the runtime does not model, so the
// main thread waited on that RPC forever (first boot: black screen after sceCdLayerSearchFile -> sceSifCallRpc ->
// WaitSema). The disc is single-layer (4.27 GB), so this is sceCdSearchFile, whose runtime handler resolves the name
// against the disc folder; the layer argument ($a2) is not needed.
static void nocturne_sceCdLayerSearchFile(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceCdSearchFile");
    ps2_stubs::sceCdSearchFile(rdram, ctx, runtime);
    if (ctx->pc == entry) {
        ctx->pc = GPR_U32(ctx, 31); // jr $ra
    }
}

// Nocturne talks to its IOP modules through libmrpc (MSIFRPC.IRX: the multi-thread SIF RPC library) as well as
// plain SIF RPC. The runtime models RPC at the EE library level (ps2_stubs::sceSifBindRpc / sceSifCallRpc) and
// runs no IOP, so libmrpc's own bodies wait forever for an IOP reply: sceSifMInitRpc sends SIF command
// 0x80000001 and spins on sceSifGetSreg(1) (second boot: 170 million calls of 0x2DDDF0). The libmrpc client calls
// take the same arguments, in the same order, as their libsif counterparts, and the client record keeps the
// server pointer where the runtime's bind writes it, so each is routed to the libsif handler.
template <void (*Handler)(uint8_t*, R5900Context*, PS2Runtime*)>
static void nocturneRoute(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, const char* cost) {
    const uint32_t entry = ctx->pc;
    runtime->gate3ChargeNamedHleV1(ctx, cost);
    Handler(rdram, ctx, runtime);
    if (ctx->pc == entry) ctx->pc = GPR_U32(ctx, 31); // jr $ra
}
// 0x2CCE90 sceSifMInitRpc(mode): mark libmrpc initialised (its flag word at 0x52BA88) and return.
static void nocturne_sceSifMInitRpc(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceSifInitRpc");
    const uint32_t one = 1u;
    std::memcpy(rdram + 0x52BA88u, &one, sizeof(one));
    SET_GPR_U32(ctx, 2, 0u);
    if (ctx->pc == entry) ctx->pc = GPR_U32(ctx, 31);
}
static void nocturne_sceSifMBindRpc(uint8_t* r, R5900Context* c, PS2Runtime* rt) {
    nocturneRoute<&ps2_stubs::sceSifBindRpc>(r, c, rt, "ps2_stubs::sceSifBindRpc");
}
static void nocturne_sceSifMCallRpc(uint8_t* r, R5900Context* c, PS2Runtime* rt) {
    nocturneRoute<&ps2_syscalls::sceSifCallRpc>(r, c, rt, "ps2_syscalls::sceSifCallRpc");
}
// 0x2CD2F8 sceSifMUnBindRpc: nothing to release in the runtime's model.
static void nocturne_sceSifMUnBindRpc(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    runtime->gate3ChargeNamedHleV1(ctx, "ps2_stubs::sceSifExitRpc");
    SET_GPR_U32(ctx, 2, 0u);
    if (ctx->pc == entry) ctx->pc = GPR_U32(ctx, 31);
}

// 0x2C0FF8 sifSndInitCmdIF (Nocturne's sound command interface, its own name from the message it prints): spins
// until sceSifGetSreg(31) is non-zero, the flag the game's IOP sound driver raises when it is up (fourth boot:
// thread 9 looping 0x2C1020..0x2C103C <-> 0x2DDDF0). There is no IOP sound driver for Nocturne yet, so raise the
// flag in the guest's SIF software-register table (sceSifGetSreg reads 0x59BBC0 + 4 * reg) and run the original.
// The game then sends its sound commands through sceSifSendCmd; Nocturne is silent until its driver is modelled.
static PS2Runtime::RecompiledFunction g_sifSndInitCmdIF = nullptr;
static void nocturne_sifSndInitCmdIF(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t ready = 1u;
    std::memcpy(rdram + 0x59BBC0u + 31u * 4u, &ready, sizeof(ready));
    g_sifSndInitCmdIF(rdram, ctx, runtime);
}

// CP0 Status for guest contexts (defined in Kernel/Syscalls/Thread.cpp). Nocturne's main loop sleeps
// (SleepThread) until its VBlank INTC handler wakes it; the runtime only delivers an interrupt to a context whose
// Status has IE, EIE and the INTC line set, and every context started at 0, so the fifth boot ended in "Gate3 all
// blocked: no admitted temporal wake source". 0x10C01 = IE | IM2 (INTC) | IM3 (DMAC) | EIE, what the kernel gives
// a user thread once it is running (EI/DI then toggle EIE as before).
extern uint32_t g_magatamaThreadStatus;
extern bool g_magatamaStrictVifDirect;
static constexpr uint32_t kNocturneThreadStatus = 0x00010C01u;

// ── Disc latency for Nocturne's file worker ───────────────────────────────
// The runtime serves disc reads instantly. Nocturne starts a background load, and only afterwards stores the
// request handle in its file object (obj+0xC). The file worker (thread 6, priority 72) preempts the game thread
// (priority 96) as soon as the request is queued, and with an instant disc the whole request (open, read, close,
// free) finished before the game thread got to that store, so it wrote back a handle that had already been freed
// and later freed it again (ninth boot: double free of 0x5E1D00 -> allocator corruption -> NULL -> hang). On a
// PS2 the worker blocks on the drive at the open and at each read, and the game thread runs meanwhile. Model that
// block: after the archive open (FUN_002BDB80) and each archive read (FUN_002BDBD0) the worker waits for the next
// VBlank start in guest time. MAGATAMA_CD_LATENCY=0 turns it off.
namespace ps2_syscalls {
int gate3TemporalWait(uint8_t*, R5900Context* ctx, PS2Runtime* runtime, std::function<bool()> ready);
int gate3TemporalWaitForStart(uint8_t*, R5900Context* ctx, PS2Runtime* runtime, uint64_t start);
}
namespace {
std::map<uint32_t, PS2Runtime::RecompiledFunction>& cdLatencyOrig() { static std::map<uint32_t, PS2Runtime::RecompiledFunction> m; return m; }
void cdLatencyTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    cdLatencyOrig().at(entry)(rdram, ctx, runtime);
    // Keep the function's result and return address across the wait.
    const __m128i v0 = ctx->r[2], v1 = ctx->r[3];
    const uint32_t pc = ctx->pc;
    auto& owner = runtime->gate3TemporalV1();
    const uint64_t target = owner.starts() + 1;
    // Keep the exact same one-start latency and wait
    // result, but describe its readiness to the temporal owner explicitly.
    // Arbitrary waits retain their original per-checkpoint polling behavior.
    static const bool startWait = [] {
        const char* v = std::getenv("MAGATAMA_CD_START_WAIT");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    if (startWait)
        ps2_syscalls::gate3TemporalWaitForStart(rdram, ctx, runtime, target);
    else
        ps2_syscalls::gate3TemporalWait(rdram, ctx, runtime, [&owner, target] { return owner.starts() >= target; });
    ctx->r[2] = v0; ctx->r[3] = v1; ctx->pc = pc;
}
void armCdLatency(PS2Runtime& runtime) {
    if (const char* v = std::getenv("MAGATAMA_CD_START_WAIT"); !(v && std::strcmp(v, "0") == 0))
        std::fprintf(stderr, "[cd-latency] modeled video-start wait enabled; one-start delay unchanged\n");
    for (const uint32_t addr : {0x2BDB80u, 0x2BDBD0u}) {
        if (!runtime.hasFunction(addr)) continue;
        cdLatencyOrig()[addr] = runtime.lookupFunction(addr);
        runtime.registerFunction(addr, cdLatencyTrampoline);
    }
}
} // namespace

// FUN_002ADA68 (start of a frame) first spins on the byte at gp-0x28E5 until the previous frame's GS work is
// done; an interrupt handler clears it (FUN_002ADB68). Recompiled, every pass of that 3-instruction loop costs a
// full Gate-3 checkpoint but only a few guest cycles, so guest time crawls while it spins: with movies playing the
// whole game dropped to about a third of real speed. Park the thread in guest time until the flag clears (exactly
// what the spin waits for), then run the function, which finds the flag clear and carries on.
namespace {
PS2Runtime::RecompiledFunction g_frameWaitOrig;
void frameWaitTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    // MAGATAMA_GS_TRACE_AT=<frame>: start an armed RRV_GS_TRACE capture once the guest frame count reaches <frame>.
    static const uint64_t traceAt = [] { const char* v = std::getenv("MAGATAMA_GS_TRACE_AT"); return v ? std::strtoull(v, nullptr, 10) : 0ull; }();
    if (traceAt && runtime->gate3TemporalV1().starts() >= traceAt) rrv::gstrace::trigger();
    const uint32_t flag = (GPR_U32(ctx, 28) - 0x28E5u) & 0x01FFFFFFu;
    if (rdram[flag] != 0) {
        const R5900Context saved = *ctx;
        ps2_syscalls::gate3TemporalWait(rdram, ctx, runtime, [rdram, flag] { return rdram[flag] == 0; });
        *ctx = saved;
    }
    g_frameWaitOrig(rdram, ctx, runtime);
}
void armFrameWait(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x2ADA68u)) return;
    g_frameWaitOrig = runtime.lookupFunction(0x2ADA68u);
    runtime.registerFunction(0x2ADA68u, frameWaitTrampoline);
}
} // namespace

// EIntr (0x2E40B8) re-enables interrupts; deliver IPU DMA completions that arrived while they were off.
namespace ps2_syscalls { void magatamaDeliverDeferredDmac(uint8_t* rdram, PS2Runtime* runtime); }
namespace {
PS2Runtime::RecompiledFunction g_eintrOrig;
void eintrTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_eintrOrig(rdram, ctx, runtime);
    const __m128i v0 = ctx->r[2];
    ps2_syscalls::magatamaDeliverDeferredDmac(rdram, runtime);
    ctx->r[2] = v0;
}
void armEintrDelivery(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x2E40B8u)) return;
    g_eintrOrig = runtime.lookupFunction(0x2E40B8u);
    runtime.registerFunction(0x2E40B8u, eintrTrampoline);
}
} // namespace

// MAGATAMA_TRACE_LOADWAIT=1: FUN_00296B28 spins on FUN_00296AC8(obj) until a loaded object is ready
// (obj+0 type, obj+1 state == 6, and for type 1 the unpacked flag at obj+0x68). Sample what it is waiting on.
namespace {
PS2Runtime::RecompiledFunction g_loadReadyOrig;
void loadReadyTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t obj = GPR_U32(ctx, 4);
    g_loadReadyOrig(rdram, ctx, runtime);
    static uint64_t n = 0;
    if (++n % 50000 == 1) {
        auto b = [rdram](uint32_t a) { return (unsigned)rdram[a & 0x01FFFFFFu]; };
        uint32_t w[8]; for (int i = 0; i < 8; ++i) std::memcpy(&w[i], rdram + ((obj + 4u * i) & 0x01FFFFFFu), 4);
        uint16_t f68; std::memcpy(&f68, rdram + ((obj + 0x68u) & 0x01FFFFFFu), 2);
        const uint32_t q = 0x575618u;
        std::fprintf(stderr, "[loadwait] n=%llu obj=%08x type=%u state=%u ready=%u unpacked=%u words=%08x %08x %08x %08x %08x %08x %08x %08x "
            "queue: head=%u count=%u seq=%u\n", (unsigned long long)n, obj, b(obj), b(obj + 1), GPR_U32(ctx, 2), f68,
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], b(q + 4), b(q + 6), b(q + 7));
    }
}
void armLoadWaitTrace(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x296AC8u)) return;
    g_loadReadyOrig = runtime.lookupFunction(0x296AC8u);
    runtime.registerFunction(0x296AC8u, loadReadyTrampoline);
}
} // namespace

// Read-only graphics handoff trace. Log initial calls and occasional samples so
// a stuck producer/consumer handshake can be inspected without flooding the log.
namespace {
struct GraphicsTap { PS2Runtime::RecompiledFunction orig; uint64_t calls = 0; };
std::map<uint32_t, GraphicsTap>& graphicsTaps() {
    static std::map<uint32_t, GraphicsTap> taps;
    return taps;
}
void graphicsTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    auto& tap = graphicsTaps().at(entry);
    const uint64_t n = ++tap.calls;
    const uint32_t gp = GPR_U32(ctx, 28) & 0x1fffffffu;
    const uint32_t caller = GPR_U32(ctx, 31), sp = GPR_U32(ctx, 29);
    const int context = rrv::guest_time::context_id;
    // Common kernel wrappers are useful only when called by graphics worker 3.
    const bool kernelTap = entry == 0x2DBDC0u || entry == 0x2DBEC0u;
    const bool interruptWake = entry == 0x2DCD50u && context < 0;
    const bool sample = (!kernelTap || context == 3) &&
        (n <= 24 || n % (interruptWake ? 60000u : 120u) == 0);
    if (sample && gp >= 0x2937u && gp < 0x2000000u) {
        std::fprintf(stderr,
            "[graphics] pc=%08x ctx=%d n=%llu ra=%08x a0=%08x a1=%08x "
            "field=%02x producer=%02x consumer=%02x slots=%02x/%02x ready=%02x worker=%02x\n",
            entry, rrv::guest_time::context_id, (unsigned long long)n,
            GPR_U32(ctx, 31), GPR_U32(ctx, 4), GPR_U32(ctx, 5),
            rdram[gp-0x2937], rdram[gp-0x292e], rdram[gp-0x2916],
            rdram[gp-0x2918], rdram[gp-0x2917], rdram[gp-0x28ec], rdram[gp-0x2927]);
    }
    if (sample) {
        std::fprintf(stderr, "[graphics-call] enter pc=%08x ctx=%d ra=%08x sp=%08x s0=%08x s1=%08x s6=%08x\n",
            entry, context, caller, sp, GPR_U32(ctx, 16), GPR_U32(ctx, 17), GPR_U32(ctx, 22));
    }
    tap.orig(rdram, ctx, runtime);
    // Capture status at the exact point the worker resumes from DMA sleep.
    // Read the existing GS mirror; do not force a backend flush or MMIO access.
    if (entry == 0x2DBDC0u && context == 3 &&
        (caller == 0x2AAE98u || caller == 0x2ADD0Cu || caller == 0x2ADD7Cu)) {
        std::fprintf(stderr, "[graphics-resume] caller=%08x pc=%08x csr=%016llx imr=%016llx\n",
            caller, ctx->pc, (unsigned long long)runtime->memory().gs().csr,
            (unsigned long long)runtime->memory().gs().imr);
    }
    if (sample) {
        // A different PC can be a guest continuation; report it without treating
        // every host C++ return as completion of the guest routine.
        std::fprintf(stderr, "[graphics-call] leave entry=%08x ctx=%d pc=%08x caller=%08x sp=%08x v0=%08x s6=%08x\n",
            entry, context, ctx->pc, caller, GPR_U32(ctx, 29), GPR_U32(ctx, 2), GPR_U32(ctx, 22));
    }
}
void armGraphicsTrace(PS2Runtime& runtime) {
    for (uint32_t addr : {0x2AA2F8u, 0x2AA168u, 0x2AA1B0u, 0x2A9F90u,
                          0x2A9FC8u, 0x2A9C08u, 0x2A9D40u, 0x2ACB20u,
                          0x2ADB98u, 0x2AADC8u, 0x2DC120u, 0x2C98A0u,
                          0x2C9BD0u, 0x2C9728u, 0x2C96E8u, 0x2C9D70u,
                          0x2BCFD0u, 0x2AC990u, 0x2DBDC0u, 0x2DBEC0u,
                          0x2AADA0u, 0x2A9CF8u, 0x2DCD50u, 0x2DBBC0u}) {
        if (!runtime.hasFunction(addr)) continue;
        graphicsTaps()[addr] = {runtime.lookupFunction(addr), 0};
        runtime.registerFunction(addr, graphicsTrampoline);
    }
    std::fprintf(stderr, "[graphics] handoff and worker call/return trace armed\n");
}
} // namespace

// ── MAGATAMA_WAITS=1: where are guest threads blocked? (debug, default off) ─
// Wraps the game's WaitSema / SleepThread / WaitEventFlag wrappers and remembers, per guest stack (one per
// thread), the last call: site ($ra), argument and whether it returned. Printed at exit, so a stuck boot shows
// which thread sleeps where, waiting for what.
namespace {
struct WaitSite { uint32_t ra = 0, arg = 0; uint64_t calls = 0, returns = 0; const char* what = ""; };
std::mutex g_waitMutex;
std::map<uint32_t, WaitSite>& waitSites() { static std::map<uint32_t, WaitSite> m; return m; }
std::map<uint32_t, std::pair<const char*, PS2Runtime::RecompiledFunction>>& waitOrig() {
    static std::map<uint32_t, std::pair<const char*, PS2Runtime::RecompiledFunction>> m; return m;
}
void waitTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc, ra = GPR_U32(ctx, 31), arg = GPR_U32(ctx, 4);
    const uint32_t key = GPR_U32(ctx, 29) >> 14; // 16 KB bucket of the guest stack: one per thread
    const auto& orig = waitOrig().at(entry);
    { std::lock_guard lock(g_waitMutex); auto& w = waitSites()[key]; w.ra = ra; w.arg = arg; ++w.calls; w.what = orig.first; }
    orig.second(rdram, ctx, runtime);
    { std::lock_guard lock(g_waitMutex); ++waitSites()[key].returns; }
}
void dumpWaits() {
    std::lock_guard lock(g_waitMutex);
    for (const auto& [key, w] : waitSites())
        std::fprintf(stderr, "[waits] stack~%08x last=%s(0x%x) from ra=%08x calls=%llu returns=%llu%s\n", key << 14,
                     w.what, w.arg, w.ra, (unsigned long long)w.calls, (unsigned long long)w.returns,
                     w.calls != w.returns ? "  <-- BLOCKED" : "");
}
void armWaits(PS2Runtime& runtime) {
    const std::pair<uint32_t, const char*> targets[] = {
        {0x2DBEE0u, "WaitSema"}, {0x2DBDC0u, "SleepThread"}, {0x2DBEF0u, "PollSema"}};
    for (const auto& [addr, name] : targets) {
        if (!runtime.hasFunction(addr)) continue;
        waitOrig()[addr] = {name, runtime.lookupFunction(addr)};
        runtime.registerFunction(addr, waitTrampoline);
    }
    (void)waitSites(); // construct the map before registering the exit dump, so it outlives the dump
    std::atexit(dumpWaits);
    std::fprintf(stderr, "[waits] armed\n");
}
} // namespace

// ── MAGATAMA_TRACE_IO=1: Nocturne's file-request system (debug, default off) ─
// The game queues file requests per device (cdrom0:, host0:, pfs0:, cdimg:/movie) to worker threads. The
// seventh boot left the game thread waiting for request completion; this prints each request as it is queued,
// processed and finished, with its device, opcode, state and path.
namespace {
std::string guestString(uint8_t* rdram, uint32_t addr) {
    std::string s;
    addr &= 0x1FFFFFFFu;
    if (!addr || addr >= 0x2000000u) return s;
    for (uint32_t i = 0; i < 96 && addr + i < 0x2000000u && rdram[addr + i]; ++i) {
        const uint8_t c = rdram[addr + i];   // never echo control bytes (BEL made Terminal beep) or binary data
        s.push_back(c >= 0x20 && c < 0x7F ? char(c) : '.');
    }
    return s;
}
uint32_t guest32(uint8_t* rdram, uint32_t addr) { uint32_t v = 0; std::memcpy(&v, rdram + (addr & 0x1FFFFFFFu), 4); return v; }
uint8_t guest8(uint8_t* rdram, uint32_t addr) { return rdram[addr & 0x1FFFFFFFu]; }
struct IoTap { const char* name; int kind; PS2Runtime::RecompiledFunction orig; };
std::map<uint32_t, IoTap>& ioTaps() { static std::map<uint32_t, IoTap> m; return m; }
std::mutex g_ioMutex;
void ioTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc, ra = GPR_U32(ctx, 31), a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6);
    const IoTap& tap = ioTaps().at(entry);
    {
        std::lock_guard lock(g_ioMutex);
        if (tap.kind == 0) // a0 = request record
            std::fprintf(stderr, "[io] %-10s req=%08x dev=%u op=%u state=%u cb=%08x done=%u ctx=%08x/%d path=\"%s\" ra=%08x\n",
                         tap.name, a0, guest8(rdram, a0 + 0x14), guest8(rdram, a0 + 0x15), guest8(rdram, a0 + 0x16),
                         guest32(rdram, a0 + 0x38), uint32_t(guest32(rdram, GPR_U32(ctx, 28) - 0x2820u) & 0xFFFFu),
                         GPR_U32(ctx, 29), rrv::guest_time::context_id, guestString(rdram, guest32(rdram, a0 + 0x10)).c_str(), ra);
        else if (tap.kind == 1) { // a0 = path string
            const std::string path = guestString(rdram, a0);
            std::fprintf(stderr, "[io] %-10s path=\"%s\" a1=%x a2=%x ra=%08x\n", tap.name, path.c_str(), a1, a2, ra);
            // MAGATAMA_GS_TRACE_ON=<text>: start an armed RRV_GS_TRACE capture when a file whose path contains <text>
            // is opened (with RRV_GS_TRACE=<file> RRV_GS_TRACE_ARM=1 RRV_GS_TRACE_FRAMES=<n>).
            if (const char* on = std::getenv("MAGATAMA_GS_TRACE_ON"); on && on[0] && path.find(on) != std::string::npos)
                rrv::gstrace::trigger();
        }
        else if (tap.kind == 4) // CreateThread(param): func +4, stack +8, size +0xC, priority +0x14
            std::fprintf(stderr, "[io] %-10s func=%08x prio=%u (caller ctx=%d) ra=%08x\n", tap.name, guest32(rdram, a0 + 4),
                         guest32(rdram, a0 + 0x14), rrv::guest_time::context_id, ra);
        else if (tap.kind == 5) // ChangeThreadPriority(tid, prio)
            std::fprintf(stderr, "[io] %-10s tid=%d prio=%u (caller ctx=%d) ra=%08x\n", tap.name, int(a0), a1,
                         rrv::guest_time::context_id, ra);
        else if (tap.kind == 3) { // game file object: a0 = object (state at +1, request handle at +0xC)
            const uint32_t obj = tap.name[0] == 'g' ? a0 : GPR_U32(ctx, 8);
            std::fprintf(stderr, "[io] %-10s a0=%08x a1=%x obj=%08x obj.state=%u obj.req=%08x ctx=%08x/%d ra=%08x\n", tap.name, a0, a1,
                         obj, guest8(rdram, obj + 1), guest32(rdram, obj + 0xC), GPR_U32(ctx, 29), rrv::guest_time::context_id, ra);
        }
        else // callback (req, status, a2, a3)
            std::fprintf(stderr, "[io] %-10s req=%08x status=%u a2=%x a3=%x ra=%08x\n", tap.name, a0, a1, a2, GPR_U32(ctx, 7), ra);
    }
    const uint32_t obj8 = GPR_U32(ctx, 8);
    tap.orig(rdram, ctx, runtime);
    if (tap.kind == 3 && tap.name[0] == 'f') {
        std::lock_guard lock(g_ioMutex);
        std::fprintf(stderr, "[io] %-10s returned pc=%08x obj=%08x obj.state=%u obj.req=%08x s0=%08x ctx=%08x\n", tap.name,
                     ctx->pc, obj8, guest8(rdram, obj8 + 1), guest32(rdram, obj8 + 0xC), GPR_U32(ctx, 16), GPR_U32(ctx, 29));
    }
    if (tap.kind == 4) {
        std::lock_guard lock(g_ioMutex);
        std::fprintf(stderr, "[io] %-10s -> tid %d\n", tap.name, int32_t(GPR_U32(ctx, 2)));
    }
    if (tap.kind == 1) {
        std::lock_guard lock(g_ioMutex);
        std::fprintf(stderr, "[io] %-10s -> %d\n", tap.name, int32_t(GPR_U32(ctx, 2)));
    }
}
void armIoTrace(PS2Runtime& runtime) {
    const struct { uint32_t addr; const char* name; int kind; } taps[] = {
        {0x2BE5B8u, "enqueue", 0}, {0x2BE720u, "finish", 0}, {0x2BE878u, "fail", 0},
        {0x2BE6A8u, "release", 0}, {0x2BF688u, "close-free", 0}, {0x2BF638u, "close-req", 0},
        {0x296D90u, "file-cb", 3}, {0x296648u, "game-free", 3},
        {0x2DBCA0u, "CreateThr", 4}, {0x2DBD30u, "ChgPrio", 5},
        {0x2BE328u, "sync-cb", 2}, {0x2BF208u, "route", 1}, {0x2BDB80u, "cd-open", 1}, {0x2BDBD0u, "cd-read", 1}};
    for (const auto& tap : taps) {
        if (!runtime.hasFunction(tap.addr)) { std::fprintf(stderr, "[io] no function at %08x\n", tap.addr); continue; }
        ioTaps()[tap.addr] = {tap.name, tap.kind, runtime.lookupFunction(tap.addr)};
        runtime.registerFunction(tap.addr, ioTrampoline);
    }
    std::fprintf(stderr, "[io] armed\n");
}
} // namespace

// ── MAGATAMA_TRACE_ALLOC=1: the game's small-object allocator (debug, default off) ─
// FUN_002A8BA8(size) hands out blocks from 4 KB pages of a 2 MB arena (FUN_002A8D38); FUN_002A8BD8(ptr) frees.
// The eighth boot got NULL back for a 0x40-byte file request after ~136 loads. This keeps every live block with
// its size and caller, and on the first NULL prints the live blocks grouped by caller.
namespace {
PS2Runtime::RecompiledFunction g_allocOrig = nullptr, g_freeOrig = nullptr, g_callocOrig = nullptr;
thread_local uint32_t g_outerRa = 0; // the caller of FUN_002A8BA8, which reaches the allocator through itself
std::mutex g_allocMutex;
std::map<uint32_t, std::pair<uint32_t, uint32_t>>& liveBlocks() { static std::map<uint32_t, std::pair<uint32_t, uint32_t>> m; return m; }
uint64_t g_allocs = 0, g_frees = 0, g_freeUnknown = 0; bool g_allocReported = false;
void allocTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t size = GPR_U32(ctx, 4), ra = g_outerRa ? g_outerRa : GPR_U32(ctx, 31);
    g_outerRa = 0;
    g_allocOrig(rdram, ctx, runtime);
    const uint32_t block = GPR_U32(ctx, 2);
    std::lock_guard lock(g_allocMutex);
    ++g_allocs;
    if (block) {
        if (auto it = liveBlocks().find(block); it != liveBlocks().end())
            std::fprintf(stderr, "[alloc] block %08x handed out twice: size 0x%x ra=%08x (live: size 0x%x ra=%08x)\n",
                         block, size, ra, it->second.first, it->second.second);
        liveBlocks()[block] = {size, ra};
        return;
    }
    if (g_allocReported) return;
    g_allocReported = true;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> byCaller; // ra -> (count, bytes)
    for (const auto& [b, info] : liveBlocks()) { auto& c = byCaller[info.second]; ++c.first; c.second += info.first; }
    std::fprintf(stderr, "[alloc] NULL for size 0x%x from ra=%08x after %llu allocs / %llu frees (%llu unknown frees), "
                 "%zu live blocks:\n", size, ra, (unsigned long long)g_allocs, (unsigned long long)g_frees,
                 (unsigned long long)g_freeUnknown, liveBlocks().size());
    const uint32_t gp = GPR_U32(ctx, 28);
    std::fprintf(stderr, "[alloc] arena=%08x pages=%u base=%08x end=%08x free-page-head=%08x gp=%08x\n",
                 guest32(rdram, gp - 0x2118u), guest32(rdram, gp - 0x2108u), guest32(rdram, gp - 0x2110u),
                 guest32(rdram, gp - 0x210Cu), guest32(rdram, gp - 0x2114u), gp);
    for (const auto& [caller, c] : byCaller)
        std::fprintf(stderr, "[alloc]   ra=%08x live=%llu bytes=%llu\n", caller, (unsigned long long)c.first,
                     (unsigned long long)c.second);
}
void callocTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_outerRa = GPR_U32(ctx, 31);
    g_callocOrig(rdram, ctx, runtime);
    g_outerRa = 0;
}
void freeTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t block = GPR_U32(ctx, 4);
    {
        std::lock_guard lock(g_allocMutex);
        ++g_frees;
        if (block && liveBlocks().erase(block) == 0) {
            ++g_freeUnknown;
            std::fprintf(stderr, "[alloc] free of unknown block %08x ra=%08x\n", block, GPR_U32(ctx, 31));
        }
    }
    g_freeOrig(rdram, ctx, runtime);
}
void armAllocTrace(PS2Runtime& runtime) {
    // FUN_002A8AF8 is the block allocator itself (FUN_002A8BA8 = it plus clearing), so every allocation passes here.
    if (!runtime.hasFunction(0x2A8AF8u) || !runtime.hasFunction(0x2A8BD8u)) return;
    (void)liveBlocks();
    g_allocOrig = runtime.lookupFunction(0x2A8AF8u); runtime.registerFunction(0x2A8AF8u, allocTrampoline);
    g_freeOrig = runtime.lookupFunction(0x2A8BD8u); runtime.registerFunction(0x2A8BD8u, freeTrampoline);
    if (runtime.hasFunction(0x2A8BA8u)) { g_callocOrig = runtime.lookupFunction(0x2A8BA8u); runtime.registerFunction(0x2A8BA8u, callocTrampoline); }
    std::fprintf(stderr, "[alloc] armed\n");
}
} // namespace

// ── Registration entry point ──────────────────────────────────────────────
// Optional silent IOP compatibility. Observe accepted bank commands and
// answer only known bank queries; unrelated sound commands keep their path.
namespace {
NocturneSilentSoundBanks g_silentSoundBanks;
std::map<uint32_t, PS2Runtime::RecompiledFunction> g_silentSoundOrig;
void silentSoundTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc, ra = GPR_U32(ctx, 31), a0 = GPR_U32(ctx, 4);
    std::string path;
    const auto memoryBank=entry==0x2C10E8u && a0==0x1B0u
        ? nocturneSilentMemoryMidiBank(rdram,32u*1024u*1024u,GPR_U32(ctx,6),GPR_U32(ctx,7))
        : std::nullopt;
    if (entry == 0x2C10E8u && a0 == 0xA0u && GPR_U32(ctx, 7) >= 20u)
        path = guestString(rdram, GPR_U32(ctx, 6));
    g_silentSoundOrig.at(entry)(rdram, ctx, runtime);
    if (ctx->pc != ra) return; // do not synthesize completion across a guest continuation
    const uint64_t frame = runtime->gate3TemporalV1().starts();
    if (entry == 0x2C10E8u && GPR_U32(ctx, 2) != 0 &&
        path.size() == 19 && path.compare(0, 11, "/sound/MIDI") == 0 && path.compare(15, 4, ".SMG") == 0) {
        char* end = nullptr;
        const unsigned long bank = std::strtoul(path.c_str() + 11, &end, 16);
        if (end == path.c_str() + 15 && bank <= 0xffff) {
            g_silentSoundBanks.request(static_cast<uint16_t>(bank), frame);
            std::fprintf(stderr, "[sound-silent] virtual bank request %04lx ready after frame %llu\n",
                bank, (unsigned long long)frame);
        }
    } else if (entry == 0x2C10E8u && GPR_U32(ctx,2)!=0 && memoryBank) {
        g_silentSoundBanks.request(*memoryBank,frame);
        std::fprintf(stderr,"[sound-silent] virtual memory bank request %04x ready after frame %llu\n",
            unsigned(*memoryBank),(unsigned long long)frame);
    } else if (entry == 0x2C1C20u &&
               g_silentSoundBanks.ready(static_cast<uint16_t>(a0 >> 16), frame)) {
        SET_GPR_U32(ctx, 2, 1u); // Nocturne's resident-bank status, observed in its query routine
    }
}
void armSilentSound(PS2Runtime& runtime) {
    for (uint32_t addr : {0x2C10E8u, 0x2C1C20u}) {
        if (!runtime.hasFunction(addr)) throw std::runtime_error("Nocturne silent sound target missing");
        g_silentSoundOrig[addr] = runtime.lookupFunction(addr);
        runtime.registerFunction(addr, silentSoundTrampoline);
    }
    std::fprintf(stderr, "[sound-silent] virtual bank completion enabled; audio playback unavailable\n");
}
} // namespace

// Silent movie audio. Nocturne's movies (.IPS) interleave audio with video. The demuxer copies audio into a 64 KB
// ring that the IOP sound driver drains by calling back into the EE (RPC -> FUN_002c2218's registered provider,
// 0x2C5098 here, "give me n bytes"). With no IOP driver nothing drains it, the ring fills, and the reader stops
// issuing disc reads (FUN_002c55f0 requires 16 KB of free audio space), so the video ring starves and the movie
// freezes after ~20 frames. Video timing is driven by the game's own frame tick (FUN_002c4d30), so here, after
// each tick, the provider is asked for audio exactly as the IOP would, and the bytes are discarded.
namespace {
PS2Runtime::RecompiledFunction g_movieTickOrig;
inline uint32_t guestWord(const uint8_t* rdram, uint32_t addr) {
    uint32_t v; std::memcpy(&v, rdram + (addr & 0x01FFFFFFu), 4); return v;
}
void silentMovieTick(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_movieTickOrig(rdram, ctx, runtime);
    const uint32_t gp = GPR_U32(ctx, 28);
    const uint32_t provider = guestWord(rdram, gp - 0x27ACu);
    const uint32_t stream = guestWord(rdram, gp - 0x2008u);
    if (provider != 0x2C5098u || stream == 0) return;
    const uint32_t movie = guestWord(rdram, stream + 0x1Cu);
    if (!movie) return;
    // The tick runs from the game's frame callback, where the provider's WaitSema must not be called, so apply the
    // provider's own bookkeeping directly: fill (0x68) -= n, read offset (0x64) += n modulo the 64 KB ring.
    // A demuxer store racing with this simply overwrites one drain step; the audio itself is discarded.
    constexpr uint32_t kChunk = 0x1000u;   // keep the ring at most half full so reads never stall on audio
    auto put = [rdram](uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & 0x01FFFFFFu), &v, 4); };
    static const bool traceMovie = [] { const char* v = std::getenv("MAGATAMA_TRACE_MOVIE"); return v && v[0] == '1'; }();
    if (traceMovie) {
        static uint64_t n = 0;
        if (++n % 30 == 1)
            std::fprintf(stderr, "[movie] ms=%lld f=%llu state=%u video=%08x afill=%08x aread=%08x a6c=%08x skip=%08x left=%08x\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() % 1000000,
                (unsigned long long)runtime->gate3TemporalV1().starts(), rdram[(stream + 1u) & 0x01FFFFFFu],
                guestWord(rdram, movie + 0x5Cu), guestWord(rdram, movie + 0x68u), guestWord(rdram, movie + 0x64u),
                guestWord(rdram, movie + 0x6Cu), guestWord(rdram, movie + 0x70u), guestWord(rdram, stream + 0x18u));
        const uint32_t player = guestWord(rdram, gp - 0x2028u);
        if (player) {
            auto b = [rdram, player](uint32_t o) { return (unsigned)rdram[(player + o) & 0x01FFFFFFu]; };
            std::fprintf(stderr, "[player] obj=%08x decoding=%08x todma=%08x c=%u e=%u f=%u 10=%u 11=%u 12=%u 13=%u "
                "1a=%u 50=%u 51=%u ring=%u head=%u 64=%u 65=%u\n", player, guestWord(rdram, gp - 0x201Cu),
                guestWord(rdram, gp - 0x2018u), b(0xC), b(0xE), b(0xF), b(0x10), b(0x11), b(0x12), b(0x13), b(0x1A),
                b(0x50), b(0x51), b(0x52), b(0x53), b(0x64), b(0x65));
        }
    }
    // While 0x70 > 0 the provider serves a lead-in from 0x74 instead of the ring; consume it the same way.
    if (const int32_t lead = static_cast<int32_t>(guestWord(rdram, movie + 0x70u)); lead > 0) {
        put(movie + 0x70u, static_cast<uint32_t>(lead - static_cast<int32_t>(kChunk)));
        put(movie + 0x74u, guestWord(rdram, movie + 0x74u) + kChunk);
        return;
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t fill = guestWord(rdram, movie + 0x68u);
        if (fill <= 0x8000u || fill > 0x10000u) break;
        uint32_t read = guestWord(rdram, movie + 0x64u) + kChunk;
        if (read > 0xFFFFu) read -= 0x10000u;
        put(movie + 0x68u, fill - kChunk);
        put(movie + 0x64u, read);
    }
}
void armSilentMovieAudio(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x2C4D30u) || !runtime.hasFunction(0x2C5098u))
        throw std::runtime_error("Nocturne movie audio targets missing");
    g_movieTickOrig = runtime.lookupFunction(0x2C4D30u);
    runtime.registerFunction(0x2C4D30u, silentMovieTick);
    std::fprintf(stderr, "[sound-silent] movie audio is drained silently\n");
}
} // namespace

// MAGATAMA_FRAME448=1 (experiment): full-height frames. Nocturne's screen table (0x529358, 12-byte entries:
// w, display h, buffer h, psm, zpsm, mode) gives every mode a 448-line display but a 224-line draw buffer: the game
// renders one half-height field per vsync. FUN_002A9AA8 copies the chosen entry into the live config the buffer
// allocator and the display setup read; doubling the buffer height there asks the game for 448-line buffers.
namespace {
PS2Runtime::RecompiledFunction g_screenCfgOrig;
PS2Runtime::RecompiledFunction g_screenResetOrig;
void screenResetFieldMode(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (GPR_U32(ctx, 4) == 0u) {
        // Retain the supported NTSC interlaced counter route. FIELD mode
        // scans a full-height buffer; FRAME mode uses a half-height buffer.
        SET_GPR_U32(ctx, 7, 0u);
        std::fprintf(stderr, "[frame448] interlaced FIELD scanout trial\n");
    }
    g_screenResetOrig(rdram, ctx, runtime);
}
void screenCfg448(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t dst = GPR_U32(ctx, 4) & 0x01FFFFFFu;
    g_screenCfgOrig(rdram, ctx, runtime);
    int16_t dispH, bufH;
    std::memcpy(&dispH, rdram + dst + 2u, 2);
    std::memcpy(&bufH, rdram + dst + 4u, 2);
    if (bufH * 2 == dispH) {
        std::memcpy(rdram + dst + 4u, &dispH, 2);
        std::fprintf(stderr, "[frame448] screen config %08x: buffer height %d -> %d\n", dst, bufH, dispH);
        // Optional VRAM-budget trial: 512x448 Z16 saves 448 KiB, allowing
        // the movie's 768 KiB texture to fit beside the full-height buffers.
        // Depth/projection correctness still needs replay validation.
        const char* z16 = std::getenv("MAGATAMA_FRAME448_Z16");
        if (z16 && z16[0] == '1' && rdram[dst + 7u] == 0x30u) {
            rdram[dst + 7u] = 0x32u;
            std::fprintf(stderr, "[frame448] experimental depth format PSMZ32 -> PSMZ16\n");
        }
    }
}
void armFrame448(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x2A9AA8u)) throw std::runtime_error("Nocturne screen config target missing");
    g_screenCfgOrig = runtime.lookupFunction(0x2A9AA8u);
    runtime.registerFunction(0x2A9AA8u, screenCfg448);
    const char* fieldMode = std::getenv("MAGATAMA_FRAME448_FIELD_MODE");
    if (fieldMode && fieldMode[0] == '1') {
        if (!runtime.hasFunction(0x2C81A8u)) throw std::runtime_error("Nocturne ResetGraph target missing");
        g_screenResetOrig = runtime.lookupFunction(0x2C81A8u);
        runtime.registerFunction(0x2C81A8u, screenResetFieldMode);
    }
}
} // namespace

// Optional VRAM arena diagnostics: both allocation directions share 28-byte
// region records. Log before entering the allocator, which can spin on failure.
namespace {
struct VramTap { PS2Runtime::RecompiledFunction orig; uint64_t calls = 0; };
std::map<uint32_t, VramTap> g_vramTaps;
void vramTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc;
    auto& tap = g_vramTaps.at(entry);
    const uint32_t gp = GPR_U32(ctx, 28) & 0x01ffffffu;
    ++tap.calls;
    if (tap.calls <= 512 && gp >= 0x20e0u && gp < 0x02000000u) {
        const uint32_t head = guestWord(rdram, gp - 0x20e0u);
        uint32_t node = head, largest = 0, nodes = 0;
        uint64_t freeWords = 0;
        while (node && node <= 0x02000000u - 28u && nodes < 512u) {
            ++nodes;
            if (guestWord(rdram, node + 8u) == 0u) {
                const uint32_t size = guestWord(rdram, node + 16u);
                freeWords += size;
                largest = std::max(largest, size);
            }
            node = guestWord(rdram, node + 4u);
            if (node == head) break;
        }
        std::fprintf(stderr, "[vram-summary] pc=%08x n=%llu caller=%08x ctx=%d request=%08x "
            "free=%llx largest=%08x nodes=%u\n", entry, (unsigned long long)tap.calls,
            GPR_U32(ctx, 31), rrv::guest_time::context_id, GPR_U32(ctx, 4),
            (unsigned long long)freeWords, largest, nodes);
    }
    if (tap.calls <= 32 || GPR_U32(ctx, 4) >= 0x10000u) {
        std::fprintf(stderr, "[vram] enter pc=%08x caller=%08x ctx=%d request=%08x tag=%08x\n",
            entry, GPR_U32(ctx, 31), rrv::guest_time::context_id, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        if (gp >= 0x20e0u && gp < 0x02000000u) {
            for (uint32_t offset : {0x20e0u, 0x20dcu}) {
                const uint32_t head = guestWord(rdram, gp - offset);
                uint32_t node = head;
                for (unsigned i = 0; node && node <= 0x02000000u - 28u && i < 32; ++i) {
                    std::fprintf(stderr, "[vram] head=-%04x node=%08x prev=%08x next=%08x "
                        "tag=%08x start=%08x size=%08x\n", offset, node,
                        guestWord(rdram, node), guestWord(rdram, node + 4u),
                        guestWord(rdram, node + 8u), guestWord(rdram, node + 12u), guestWord(rdram, node + 16u));
                    node = guestWord(rdram, node + (offset == 0x20e0u ? 4u : 0u));
                    if (node == head) break;
                }
            }
        }
    }
    tap.orig(rdram, ctx, runtime);
}
void armVramTrace(PS2Runtime& runtime) {
    for (uint32_t addr : {0x2AA438u, 0x2AA558u}) {
        if (!runtime.hasFunction(addr)) throw std::runtime_error("Nocturne VRAM allocator target missing");
        g_vramTaps[addr] = {runtime.lookupFunction(addr), 0};
        runtime.registerFunction(addr, vramTrampoline);
    }
}
} // namespace

// Free camera (R3 toggles; Magatama addition, off by default). Stock Nocturne only "peeks" with the right stick:
// fldCamMain switches to fldCamUpdateChase while the stick is pushed (the angle follows the stick, then swings back on
// release), and fldPlayerCalc_Nml ignores movement meanwhile. In free-camera mode the host hides the right stick from
// the game and instead turns the field camera yaw (gp-0x570C = 0x54EA64, degrees; calcCamNormal places the camera from
// it and the player code moves relative to it), so the view stays where it is put and walking follows the view.
// MAGATAMA_CAMLOG=1 also logs yaw/player rotation/stick bytes every 20 calls.
std::atomic<int> g_freeCamera{0};
std::atomic<float> g_freeCameraStickX{0.0f}, g_freeCameraStickY{0.0f};
namespace {
PS2Runtime::RecompiledFunction g_camMainOrig;
constexpr uint32_t kFieldCameraYaw = 0x54EA64u;
void camMainHook(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    auto f = [rdram](uint32_t a) { float v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; };
    auto put = [rdram](uint32_t a, float v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); };
    if (g_freeCamera.load(std::memory_order_relaxed)) {
        float x = g_freeCameraStickX.load(std::memory_order_relaxed);
        static const float testX = [] { const char* v = std::getenv("MAGATAMA_FREE_CAMERA_TEST"); return v ? std::strtof(v, nullptr) : 0.0f; }();
        if (testX != 0.0f) x = testX; // diagnostic: a constant right-stick X for replays
        constexpr float kDead = 0.18f, kDegPerFrame = 3.5f; // ~210 degrees/second at full tilt, eased near centre
        if (std::fabs(x) > kDead) {
            const float t = (std::fabs(x) - kDead) / (1.0f - kDead);
            float yaw = f(kFieldCameraYaw) + (x > 0 ? 1.0f : -1.0f) * t * t * kDegPerFrame;
            if (yaw >= 360.0f) yaw -= 360.0f;
            if (yaw < 0.0f) yaw += 360.0f;
            put(kFieldCameraYaw, yaw);
        }
    }
    const float y0 = f(kFieldCameraYaw);
    g_camMainOrig(rdram, ctx, runtime);
    static const bool log = [] { const char* v = std::getenv("MAGATAMA_CAMLOG"); return v && v[0] == '1'; }();
    static uint64_t n = 0;
    if (log && ++n % 20 == 0) {
        const uint8_t* st = rdram + 0x54E908u;
        std::fprintf(stderr, "[camlog] f=%llu Y %.2f->%.2f rot292=%.2f 296=%.2f mode4=%d st24=%d stick=%u,%u,%u,%u free=%d hx=%.2f\n",
            (unsigned long long)runtime->gate3TemporalV1().starts(), y0, f(kFieldCameraYaw), f(0x2FBF80u + 292u),
            f(0x2FBF80u + 296u), (int)guestWord(rdram, 0x2FBF80u + 4u), (int)guestWord(rdram, 0x2FBF80u + 24u),
            st[0], st[1], st[2], st[3], g_freeCamera.load(), g_freeCameraStickX.load());
    }
}
void armFreeCamera(PS2Runtime& runtime) {
    if (!runtime.hasFunction(0x12F718u)) throw std::runtime_error("Nocturne fldCamMain missing");
    g_camMainOrig = runtime.lookupFunction(0x12F718u);
    runtime.registerFunction(0x12F718u, camMainHook);
    if (const char* v = std::getenv("MAGATAMA_FREE_CAMERA"); v && v[0] == '1') g_freeCamera.store(1);
}
} // namespace
extern "C" void rrv_free_camera_set(int on) { g_freeCamera.store(on ? 1 : 0, std::memory_order_relaxed); }
extern "C" int rrv_free_camera_active(void) { return g_freeCamera.load(std::memory_order_relaxed); }
extern "C" void rrv_free_camera_stick(float x, float y) {
    g_freeCameraStickX.store(x, std::memory_order_relaxed);
    g_freeCameraStickY.store(y, std::memory_order_relaxed);
}

// The title-screen freeze waits for the game's IOP sound status. Observe the
// EE command queue and SIF traffic without manufacturing a success response.
namespace {
struct SoundTap { PS2Runtime::RecompiledFunction orig; uint64_t calls = 0; };
std::map<uint32_t, SoundTap>& soundTaps() {
    static std::map<uint32_t, SoundTap> taps;
    return taps;
}
void soundTrampoline(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t entry = ctx->pc, ra = GPR_U32(ctx, 31);
    const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
    const uint32_t a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
    auto& tap = soundTaps().at(entry);
    const uint64_t n = ++tap.calls;
    const bool sample = n <= 24 || n % 60000 == 0;
    const uint32_t gp = GPR_U32(ctx, 28) & 0x1fffffffu;
    if (sample) {
        std::fprintf(stderr, "[sound] enter pc=%08x n=%llu ctx=%d ra=%08x args=%08x/%08x/%08x/%08x\n",
            entry, (unsigned long long)n, rrv::guest_time::context_id, ra, a0, a1, a2, a3);
        if (entry == 0x2C10E8u && a2 && a3)
            std::fprintf(stderr, "[sound] payload=\"%s\"\n", guestString(rdram, a2).c_str());
        if (entry == 0x2DE2B0u) {
            const uint32_t packet = a1 & 0x1fffffffu;
            if (packet && packet <= 0x2000000u - 48u) {
                std::fprintf(stderr, "[sound] sif-packet");
                for (uint32_t off = 0; off < 48; off += 4)
                    std::fprintf(stderr, " %08x", guest32(rdram, packet + off));
                std::fprintf(stderr, "\n");
            }
        }
    }
    tap.orig(rdram, ctx, runtime);
    if (sample && gp >= 0x27b0u && gp < 0x2000000u) {
        std::fprintf(stderr, "[sound] leave entry=%08x pc=%08x result=%08x load=%08x status=%08x\n",
            entry, ctx->pc, GPR_U32(ctx, 2), guest32(rdram, gp - 0x27b0u),
            guest32(rdram, gp - 0x2798u));
    }
}
void armSoundTrace(PS2Runtime& runtime) {
    for (uint32_t addr : {0x2C10E8u, 0x2C1CA0u, 0x2C1C20u, 0x2DE2B0u, 0x2DE120u, 0x2DE920u}) {
        if (!runtime.hasFunction(addr)) continue;
        soundTaps()[addr] = {runtime.lookupFunction(addr), 0};
        runtime.registerFunction(addr, soundTrampoline);
    }
    std::fprintf(stderr, "[sound] command/status trace armed\n");
}
} // namespace

void registerPatches(PS2Runtime& runtime)
{
    // Nocturne registers guest interrupt handlers (AddIntcHandler) and alarms (SetAlarm); Ridge Racer V used
    // neither, so Fukami's runtime refuses both until a policy is bound (third boot: thread 2 threw "kernel
    // acknowledge policy unresolved" right after the first VIF0 microprogram upload). Bind the runtime's own
    // policies: level-based INTC handler delivery, and alarm callbacks as HLE admissions. Revisit if handlers
    // misbehave (the runtime does not acknowledge INTC_STAT for the guest).
    runtime.gate3BindUnqualifiedIntcHandlerPolicyV1();
    runtime.gate3BindUnqualifiedAlarmPolicyV1();
    // Nocturne splits IMAGE tags and pixels across distinct VIF DIRECTHL
    // commands. Do not interpret the next command's opcode as raw pixels.
    const char* strictVif = std::getenv("MAGATAMA_VIF_STRICT_DIRECT");
    g_magatamaStrictVifDirect = !(strictVif && strictVif[0] == '0');
    g_magatamaThreadStatus = kNocturneThreadStatus; // threads created by CreateThread/StartThread
    runtime.cpu().cop0_status = kNocturneThreadStatus; // the main thread (loadELF keeps it)

    struct StubBind { uint32_t addr; const char* name; };
    static const StubBind kStubBinds[] = {
#include "nocturne_stub_addrs.inc"
    };
    size_t bound = 0;
    for (const auto& s : kStubBinds) {
        if (runtime.hasFunction(s.addr)) {
            continue; // a generated body (or a generated stub wrapper) wins
        }
        if (auto fn = magatamaResolveStub(s.name)) {
            magatamaStubMap()[s.addr] = {fn, s.name};
            runtime.registerFunction(s.addr, magatamaStubTrampoline);
            ++bound;
        }
    }
    std::fprintf(stderr, "[magatama] bound %zu library entry point(s) to runtime handlers\n", bound);

    if (!runtime.hasFunction(0x193478u)) {
        runtime.registerFunction(0x193478u, nocturneCallback193478);
    }

    if (!runtime.hasFunction(0x17E2A0u))
        runtime.registerFunction(0x17E2A0u, nocturneEffectColorCallback<0x17E2A0u, 0x50u>);
    if (!runtime.hasFunction(0x17EAC8u))
        runtime.registerFunction(0x17EAC8u, nocturneEffectColorCallback<0x17EAC8u, 0x58u>);
    if (!runtime.hasFunction(0x17F3C8u))
        runtime.registerFunction(0x17F3C8u, nocturneEffectColorCallback<0x17F3C8u, 0x60u>);

    runtime.registerFunction(0x2CB950u, nocturne_sceCdLayerSearchFile);
    runtime.registerFunction(0x2CCE90u, nocturne_sceSifMInitRpc);
    runtime.registerFunction(0x2CD138u, nocturne_sceSifMBindRpc);
    runtime.registerFunction(0x2CD2F8u, nocturne_sceSifMUnBindRpc);
    runtime.registerFunction(0x2CD418u, nocturne_sceSifMCallRpc);
    if (runtime.hasFunction(0x2C0FF8u)) {
        g_sifSndInitCmdIF = runtime.lookupFunction(0x2C0FF8u);
        runtime.registerFunction(0x2C0FF8u, nocturne_sifSndInitCmdIF);
    }

    if (const char* lat = std::getenv("MAGATAMA_CD_LATENCY"); !(lat && lat[0] == '0')) {
        armCdLatency(runtime);
    }
    armEintrDelivery(runtime);
    if (const char* lw = std::getenv("MAGATAMA_TRACE_LOADWAIT"); lw && lw[0] == '1') armLoadWaitTrace(runtime);
    if (const char* fw = std::getenv("MAGATAMA_FRAME_WAIT"); !(fw && fw[0] == '0')) {
        armFrameWait(runtime);
    }
    if (const char* al = std::getenv("MAGATAMA_TRACE_ALLOC"); al && al[0] == '1') {
        armAllocTrace(runtime);
    }
    if (const char* io = std::getenv("MAGATAMA_TRACE_IO"); io && io[0] == '1') {
        armIoTrace(runtime);
    }
    if (const char* w = std::getenv("MAGATAMA_WAITS"); w && w[0] == '1') {
        armWaits(runtime);
    }
    if (const char* gfx = std::getenv("MAGATAMA_TRACE_GRAPHICS"); gfx && gfx[0] == '1') {
        armGraphicsTrace(runtime);
    }
    if (const char* silent = std::getenv("MAGATAMA_SILENT_SOUND"); silent && silent[0] == '1') {
        armSilentSound(runtime);
        armSilentMovieAudio(runtime);
    }
    if (const char* f448 = std::getenv("MAGATAMA_FRAME448"); f448 && f448[0] == '1') armFrame448(runtime);
    armFreeCamera(runtime);
    if (const char* vr = std::getenv("MAGATAMA_TRACE_VRAM"); vr && vr[0] == '1') armVramTrace(runtime);
    if (const char* snd = std::getenv("MAGATAMA_TRACE_SOUND"); snd && snd[0] == '1') {
        armSoundTrace(runtime);
    }
    if (const char* spec = std::getenv("RRV_FN_HITS"); spec && spec[0]) {
        armFnHits(runtime, spec);
    }
}
