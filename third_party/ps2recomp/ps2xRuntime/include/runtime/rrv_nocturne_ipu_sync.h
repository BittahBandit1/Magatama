#pragma once
#include "ps2_runtime_macros.h"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace rrv::ipu_timing {
// Locked USA SLUS_209.11 sceIpuSync, 0x2cd988..0x2cd9ec. This small
// translation retains the original polling instructions and delay slots so
// pending hardware can advance on the ordinary EE clock and checkpoint path.
inline constexpr std::array<uint32_t,26> syncWords={
    0x10800006,0x0000182d,0x24020001,0x1082000f,0x0060102d,
    0x10000012,0x00000000,0x3c031000,0x34632010,0x00000000,
    0x8c620000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x0440fffa,0x00000000,0x10000005,0x0000182d,0x3c021000,
    0x34422010,0x8c430000,0x00031fc2,0x0060102d,0x03e00008,0x00000000};

inline void syncOriginal(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    uint32_t pc=0x2cd988, target=0;
    bool delay=false, returning=false;
    for (;;) {
        ctx->pc=pc;
        if (!delay) runtime->gate3CheckpointV1(ctx);
        runtime->gate3BeginInstructionV1(ctx);
        const auto word=syncWords.at((pc-0x2cd988)/4);
        const unsigned op=word>>26,rs=(word>>21)&31,rt=(word>>16)&31,rd=(word>>11)&31;
        const int16_t imm=static_cast<int16_t>(word);
        bool branch=false, ret=false;
        uint32_t next=pc+4;
        if (!word) {} // nop
        else if (op==0 && (word&63)==0x2d) SET_GPR_U64(ctx,rd,GPR_U64(ctx,rs)+GPR_U64(ctx,rt));
        else if (op==0 && (word&63)==2) SET_GPR_S32(ctx,rd,GPR_U32(ctx,rt)>>((word>>6)&31));
        else if (op==0 && (word&63)==8) {branch=true;ret=true;next=GPR_U32(ctx,rs);}
        else if (op==9) SET_GPR_S32(ctx,rt,GPR_U32(ctx,rs)+imm);
        else if (op==15) SET_GPR_S32(ctx,rt,uint32_t(uint16_t(imm))<<16);
        else if (op==13) SET_GPR_U64(ctx,rt,GPR_U64(ctx,rs)|uint16_t(imm));
        else if (op==35) SET_GPR_S32(ctx,rt,runtime->memory().read32(GPR_U32(ctx,rs)+imm));
        else if (op==4 || op==1) {
            branch=true;
            const bool taken=op==4 ? GPR_U64(ctx,rs)==GPR_U64(ctx,rt) : GPR_S64(ctx,rs)<0;
            next=taken ? pc+4+int32_t(imm)*4 : pc+8;
        } else throw std::logic_error("unrecognised locked sceIpuSync instruction");
        if (delay) {
            ctx->in_delay_slot=false;pc=target;delay=false;
            if (returning) {ctx->pc=pc;return;}
        } else if (branch) {
            target=next;returning=ret;delay=true;
            ctx->in_delay_slot=true;ctx->branch_pc=pc;pc+=4;
        } else pc+=4;
    }
}
}
