// Asset-free regression for omitted colour leaves reached through the real
// generated dds3EffectPCPSetColor dispatcher. Addresses/offsets are separately
// checked against the locked USA ELF in the recorded proof.
#include "ps2_runtime_macros.h"
#include "patches.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void sub_0012F718_0x12f718(uint8_t*, R5900Context*, PS2Runtime*);
extern void FUN_001761e0_0x1761e0(uint8_t*, R5900Context*, PS2Runtime*);
int main() {
    unsigned checks=0;
    const auto check=[&](bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}};
    struct Leaf {uint32_t entry,offset,index;};
    const Leaf leaves[]={{0x17e2a0,0x50,16},{0x17eac8,0x58,17},{0x17f3c8,0x60,19},{0x193478,0x24,57},{0x17fc40,0x54,64},{0x180ba8,0xd8,66},{0x181550,0x58,67},{0x181cd0,0x48,84},
#define MAGATAMA_EFFECT_COLOR(entry, offset, index) {entry,offset,index},
#include "../src/product/nocturne_effect_color_leaves.inc"
#undef MAGATAMA_EFFECT_COLOR
    };
    PS2Runtime runtime;check(runtime.memory().initialize(),"headless guest memory initializes");
    for(auto leaf:leaves)check(!runtime.hasFunction(leaf.entry),"callback absent before product registration");
    runtime.registerFunction(0x12f718,sub_0012F718_0x12f718); // actual camera registration dependency
    registerPatches(runtime);
    rrv::guest_time::context_id=1;
    PS2Runtime::GuestExecutionScope scope(&runtime);
    uint8_t* rdram=runtime.memory().getRDRAM();
    constexpr uint32_t destination=0x669b80;
    for(auto leaf:leaves){
        check(runtime.hasFunction(leaf.entry),"product registers callback entry");
        auto fn=runtime.lookupFunction(leaf.entry);
        for(uint32_t value:{0u,1u,0x12345678u,0x80000000u,0xffffffffu}){
            R5900Context state{};auto* ctx=&state;
            ctx->pc=leaf.entry;
            SET_GPR_U32(ctx,4,destination);SET_GPR_U32(ctx,5,value);SET_GPR_U32(ctx,31,0x176214);
            std::memset(rdram+destination,0xa5,8192);
            const auto before=state;const auto cycles=runtime.gate3ModeledEeCyclesV1();
            fn(rdram,ctx,&runtime);
            uint32_t stored;std::memcpy(&stored,rdram+destination+leaf.offset,4);
            check(stored==value,"direct leaf performs exact original delay-slot store");
            bool canaries=true;for(unsigned i=0;i<8192;++i)if((i<leaf.offset||i>=leaf.offset+4)&&rdram[destination+i]!=0xa5)canaries=false;
            check(canaries,"direct leaf leaves every surrounding object byte intact");
            check(std::memcmp(ctx->r,before.r,sizeof(ctx->r))==0,"all integer registers preserved");
            check(ctx->pc==0x176214&&!ctx->in_delay_slot&&ctx->branch_pc==leaf.entry,"JR returns with original branch coordinates");
            check(ctx->insn_count==2&&runtime.gate3ModeledEeCyclesV1()==cycles+2,"exactly two original instructions accounted");
            state={};ctx->pc=0x1761e0;
            SET_GPR_U32(ctx,4,0x600000);SET_GPR_U32(ctx,5,value);
            SET_GPR_U32(ctx,29,0x556100);SET_GPR_U32(ctx,31,0x175bc0);
            runtime.memory().write32(0x600000,leaf.index);
            runtime.memory().write32(0x600008,destination);
            runtime.memory().write32(0x31dfac+leaf.index*40,leaf.entry);
            std::memset(rdram+destination,0xa5,8192);
            FUN_001761e0_0x1761e0(rdram,ctx,&runtime);
            std::memcpy(&stored,rdram+destination+leaf.offset,4);
            check(stored==value,"actual generated table dispatcher invokes leaf store");
            check(ctx->pc==0x175bc0&&GPR_U32(ctx,29)==0x556100,"actual dispatcher returns and restores stack");
            canaries=true;for(unsigned i=0;i<8192;++i)if((i<leaf.offset||i>=leaf.offset+4)&&rdram[destination+i]!=0xa5)canaries=false;
            check(canaries,"generated dispatch preserves surrounding bytes and earlier position data");
        }
    }
    std::printf("Effect colour callbacks: %u checks passed (%zu leaves,5 values,direct and generated dispatch)\n",checks,sizeof(leaves)/sizeof(leaves[0]));
}
