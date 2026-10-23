#include "Common.h"
#include "IPU.h"
#include <cstdlib>
#include <stdexcept>

namespace ps2_syscalls { void magatamaDeliverDeferredDmac(uint8_t* rdram, PS2Runtime* runtime); }

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static constexpr uint32_t REG_IPU_CTRL = 0x10002010u;
        static constexpr uint32_t REG_IPU_CMD = 0x10002000u;
        static constexpr uint32_t REG_IPU_IN_FIFO = 0x10007010u;
        const bool nocturne=std::getenv("MAGATAMA_IPU") && std::getenv("MAGATAMA_IPU")[0]=='1';
        // Nocturne sceIpuInit at 0x2CDA58 references these guest tables.
        const uint32_t IQVAL_BASE = nocturne ? 0x52baa0u : 0x1721e0u; // 0x2CDAD4: lui 0x53; addiu -0x4560
        const uint32_t VQVAL_BASE = nocturne ? 0x52baf0u : 0x172230u;
        static constexpr uint32_t SETD4_CHCR_ENTRY = 0x126428u;

        if (!runtime)
            return;

        if (!runtime->memory().getRDRAM())
        {
            if (!runtime->memory().initialize())
            {
                setReturnS32(ctx, -1);
                return;
            }
        }

        if (!runtime->syncCoreSubsystems())
        {
            setReturnS32(ctx, -1);
            return;
        }

        PS2Memory &mem = runtime->memory();

        if (!nocturne && runtime->hasFunction(SETD4_CHCR_ENTRY))
        {
            auto setD4 = runtime->lookupFunction(SETD4_CHCR_ENTRY);
            ctx->r[4] = _mm_set_epi64x(0, 1);
            {
                PS2Runtime::GuestExecutionScope guestExecution(runtime);
                setD4(rdram, ctx, runtime);
            }
        }

        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);

        __m128i v;
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x00u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x10u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x20u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x30u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x40u);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);

        mem.write32(REG_IPU_CMD, 0x50000000u);
        mem.write32(REG_IPU_CMD, 0x58000000u);

        v = runtime->Load128(rdram, ctx, VQVAL_BASE + 0x00u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, VQVAL_BASE + 0x10u);
        mem.write128(REG_IPU_IN_FIFO, v);

        mem.write32(REG_IPU_CMD, 0x60000000u);
        mem.write32(REG_IPU_CMD, 0x90000000u);

        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);

        setReturnS32(ctx, 0);
    }

    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime && std::getenv("MAGATAMA_IPU") && std::getenv("MAGATAMA_IPU")[0]=='1') {
            auto& mem=runtime->memory();
            const uint32_t state=getRegU32(ctx,4);
            const auto bp=mem.read32(state+28);
            const auto buffered=((bp>>8)&15)+((bp>>16)&3);
            const auto toMadr=mem.read32(state)-buffered*16;
            const auto toQwc=mem.read32(state+8)+buffered;
            const auto fromMadr=mem.read32(state+16);
            const auto fromQwc=mem.read32(state+20);
            if(fromMadr && fromQwc) {
                mem.write32(0x1000b010,fromMadr); mem.write32(0x1000b020,fromQwc);
                mem.write32(0x1000b000,mem.read32(state+24)|0x100);
            }
            mem.write32(0x10002000,bp&127); // BCLR restores the saved bit position.
            if(toMadr && toQwc) {
                mem.write32(0x1000b410,toMadr); mem.write32(0x1000b430,mem.read32(state+4));
                mem.write32(0x1000b420,toQwc);
                mem.write32(0x1000b400,mem.read32(state+12)|0x100);
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime && std::getenv("MAGATAMA_IPU") && std::getenv("MAGATAMA_IPU")[0]=='1') {
            auto& mem=runtime->memory();
            const uint32_t state=getRegU32(ctx,4);
            const auto toChcr=mem.read32(0x1000b400);
            mem.write32(0x1000b400,toChcr&~0x100u);
            mem.write32(state,mem.read32(0x1000b410));
            mem.write32(state+4,mem.read32(0x1000b430));
            mem.write32(state+8,mem.read32(0x1000b420)); mem.write32(state+12,toChcr);
            const auto fromChcr=mem.read32(0x1000b000);
            mem.write32(0x1000b000,fromChcr&~0x100u);
            mem.write32(state+16,mem.read32(0x1000b010));
            mem.write32(state+20,mem.read32(0x1000b020)); mem.write32(state+24,fromChcr);
            mem.write32(state+28,mem.read32(0x10002020)); mem.write32(state+32,mem.read32(0x10002010));
        }
        setReturnS32(ctx, 0);
    }

    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime && std::getenv("MAGATAMA_IPU") && std::getenv("MAGATAMA_IPU")[0]=='1') {
            auto& mem=runtime->memory();
            const bool poll=getRegU32(ctx,4)==1;
            for(unsigned i=0;i<(poll?1u:1048576u);++i) {
                const bool busy=(mem.read32(0x10002010)&0x80000000u)!=0;
                if(!busy || poll) { setReturnS32(ctx,busy?1:0); return; }
                // The IPU may be waiting for input that only a DMA-completion handler supplies (the next chunk);
                // those completions are deferred, so let them run while this spins, as they would on hardware.
                ps2_syscalls::magatamaDeliverDeferredDmac(rdram,runtime);
            }
            throw std::runtime_error("sceIpuSync: decoder stalled waiting for DMA");
        }
        setReturnS32(ctx, 0);
    }
}
