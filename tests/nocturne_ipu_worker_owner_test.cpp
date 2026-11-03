#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "runtime/rrv_ipu_worker_timing.h"
#include "runtime/rrv_nocturne_ipu_sync.h"
#include <cstdio>
#include <cstdlib>

namespace ps2_syscalls {
void AddDmacHandler(uint8_t*,R5900Context*,PS2Runtime*);
void EnableDmac(uint8_t*,R5900Context*,PS2Runtime*);
void magatamaDeferDmacCause(uint32_t);
void gate3DeliverTemporal(uint8_t*,R5900Context*,PS2Runtime*);
}
namespace {
unsigned callbackCount=0;
int callbackContext=0;
uint32_t callbackStatus=0,callbackCause=0,callbackArg=0;
void irq(uint8_t*,R5900Context* c,PS2Runtime* r) {
    r->gate3CheckpointV1(c);r->gate3BeginInstructionV1(c);
    ++callbackCount;callbackContext=rrv::guest_time::context_id;
    callbackStatus=c->cop0_status;callbackCause=GPR_U32(c,4);callbackArg=GPR_U32(c,5);c->pc=0;
}
}

int main() {
    setenv("MAGATAMA_TEST_MUTE","1",1);setenv("MAGATAMA_IPU","1",1);setenv("MAGATAMA_IPU_WORKER_TIMING","1",1);
    PS2Runtime rt;
    unsigned checks=0;
    auto check=[&](bool ok,const char* why) {++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}};
    check(rt.memory().initialize(),"actual runtime memory initialized");
    rrv::guest_time::TemporalProfile profile{};profile.terminal_starts=rrv::guest_time::kNoTerminal;
    rt.gate3ConfigureTemporalV1(profile);rrv::guest_time::context_id=1;
    PS2Runtime::GuestExecutionScope execution(&rt);
    auto* ctx=&rt.cpu();auto* ram=rt.memory().getRDRAM();auto& mem=rt.memory();
    ctx->pc=0x100000;rt.gate3CheckpointV1(ctx);
    unsigned schedules=0,commits=0;
    rrv::ipu_worker_timing::configure([&](uint64_t cycles,std::function<void()> fn) {
        ++schedules;check(cycles==64,"original worker request uses64EEcycles");
        rt.gate3TemporalV1().Admit({rt.gate3TemporalV1().now()+cycles,0x100000+schedules,[&,fn=std::move(fn)] {++commits;fn();}});
    },[](unsigned cause){ps2_syscalls::magatamaDeferDmacCause(cause);});
    auto call=[&](uint32_t mode) {
        ctx->pc=0x2cd988;SET_GPR_U32(ctx,4,mode);SET_GPR_U32(ctx,31,0x123000);
        const auto before=rt.gate3TemporalV1().now();
        ps2_stubs::sceIpuSync(ram,ctx,&rt);
        check(ctx->pc==0x123000 && !ctx->in_delay_slot,"original JR and delay slot return");
        return rt.gate3TemporalV1().now()-before;
    };
    mem.write32(0x10002000,0x10010000);
    check(schedules==1 && commits==0,"actual MMIO schedules worker without early execution");
    for(unsigned i=0;i<16;++i)call(1);
    check(commits==1,"actual EE owner commits scheduled worker");
    mem.write32(0x10002010,1u<<30);
    call(0);check(GPR_U32(ctx,2)==0,"nonbusy blocking returns zero");
    mem.write32(0x10002000,0x40000000); // real FDEC stalls with no input
    call(1);check(GPR_U32(ctx,2)==1,"poll returns real decoder busy");
    call(7);check(GPR_U32(ctx,2)==0,"original unsupported mode returns zero without waiting");
    bool fed=false;
    rt.gate3TemporalV1().Admit({rt.gate3TemporalV1().now()+1000,12,[&] {
        fed=true;
        ram[0x600000]=0x12;ram[0x600001]=0x34;ram[0x600002]=0x56;ram[0x600003]=0x78;
        mem.write32(0x1000b410,0x600000);mem.write32(0x1000b420,1);mem.write32(0x1000b400,0x101);
    }});
    const auto elapsed=call(0);
    check(fed && elapsed>=1000,"original busy loop advances owner to real hardware admission");
    check(GPR_U32(ctx,2)==0 && mem.read32(0x10002000)==0x12345678,"actual FDEC input and original sync complete");
    for(unsigned i=0;i<16;++i)call(1);
    check(!(mem.read32(0x1000b400)&0x100),"DMA STR completion preserved");
    check(mem.read32(0x1000e010)&(1<<4),"real D_STAT completion committed");
    rt.registerFunction(0x123400,irq);
    SET_GPR_U32(ctx,4,4);SET_GPR_U32(ctx,5,0x123400);SET_GPR_U32(ctx,6,0);SET_GPR_U32(ctx,7,0xabc);
    ps2_syscalls::AddDmacHandler(ram,ctx,&rt);
    SET_GPR_U32(ctx,4,4);ps2_syscalls::EnableDmac(ram,ctx,&rt);
    ctx->cop0_status=0;
    ps2_syscalls::magatamaDeferDmacCause(4);
    ps2_syscalls::gate3DeliverTemporal(ram,ctx,&rt);
    check(callbackCount==0,"masked guest state does not admit IPU IRQ");
    ctx->cop0_status=rrv::guest_time::IntcState::kStatusIe|rrv::guest_time::IntcState::kStatusEie|0x800u;
    check(rt.gate3TemporalV1().intc().Read().pending==0,"no INTC source pending in DMAC test");
    ps2_syscalls::gate3DeliverTemporal(ram,ctx,&rt);
    check(callbackCount>=1 && callbackContext<0,"IPU IRQ uses admitted callback context");
    check(callbackStatus&rrv::guest_time::IntcState::kStatusExl,"IRQ enters exception level");
    check(callbackCause==4 && callbackArg==0xabc,"original DMAC callback arguments retained without pending INTC");
    check(rrv::guest_time::context_id==1,"parent execution context restored");
    std::printf("PASS: %u actual-runtime IPU owner/sync checks; blocked %llu EE cycles; no GUI/card/audio device\n",checks,(unsigned long long)elapsed);
}
