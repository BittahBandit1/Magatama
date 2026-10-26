#include "ps2_runtime_macros.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <csignal>

int main(int argc, char** argv) {
    const char* mode=argc>1 ? argv[1] : "typed";
    const bool gs=std::strcmp(mode,"gs")==0;
    const bool generic=std::strcmp(mode,"generic")==0;
    const bool ordered=std::strcmp(mode,"ordered")==0;
    const bool mixed=std::strcmp(mode,"mixed")==0;
    const bool cancel=std::strcmp(mode,"cancel")==0;
    const bool invalidated=std::strcmp(mode,"invalidated")==0;
    const bool guard=std::strcmp(mode,"guard")==0;
    const bool immediate=std::strcmp(mode,"immediate")==0;
    unsigned checks=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s: %s\n",mode,why);std::exit(1);}};
    PS2Runtime rt;
    check(rt.memory().initialize(),"real memory initialized");
    if(gs){
        auto& memory=rt.memory();auto& arbiter=rt.gifArbiter();
        memory.gsControl().configure(true);
        arbiter.setControl(&memory.gsControl());
        arbiter.setProcessPacketFn([](GifPathId,const uint8_t*,uint32_t,uint32_t,const GifPacketDiagnostic*){});
        memory.write32(0x12001010,0x7f00); // Mask IRQs: no scheduler callback can hide a missing notification.
    }
    rrv::guest_time::TemporalProfile profile{};
    profile.terminal_starts=rrv::guest_time::kNoTerminal;
    rt.gate3ConfigureTemporalV1(profile);
    auto& sched=rt.gate3Scheduler;
    // Make genuine blocked lower-priority worker tickets, without host threads.
    // The main producer remains runnable when their waits resolve.
    sched.suspend(1);
    sched.start(2,10);
    auto ticket=sched.wait(2);sched.release(2);
    rrv::guest_time::Scheduler::Ticket other;
    if(mixed || ordered){sched.start(3,10);other=sched.wait(3);sched.release(3);}
    sched.resume(1);
    rrv::guest_time::context_id=1;
    PS2Runtime::GuestExecutionScope scope(&rt);
    auto* ctx=&rt.cpu();ctx->pc=0x100000;
    rt.gate3CheckpointV1(ctx);
    auto& owner=rt.gate3TemporalV1();
    const auto target=owner.starts()+(immediate ? 0 : 1);
    unsigned polls=0;bool otherReady=false;
    if(generic)rt.gate3AddTimedWaitV1(ticket,[&]{++polls;return owner.starts()>=target;});
    else rt.gate3AddStartWaitV1(ticket,target);
    bool sawTypedResolved=false;
    if(mixed)rt.gate3AddTimedWaitV1(other,[&]{return otherReady;});
    if(ordered)rt.gate3AddTimedWaitV1(other,[&]{if(owner.starts()<target)return false;sawTypedResolved=ticket->resolved;return true;});
    if(immediate){
        rt.gate3ResolveTimedWaitsV1();
        check(ticket->resolved,"already-reached start resolves immediately");
        check(ticket->result==0,"success result preserved");
        std::printf("PASS immediate %u checks\n",checks);return 0;
    }
    const auto cycle0=rt.gate3ModeledEeCyclesV1();
    const auto insn0=ctx->insn_count;
    for(unsigned i=0;i<1000;++i){
        ctx->pc=0x100000;
        rt.gate3CheckpointV1(ctx);rt.gate3BeginInstructionV1(ctx);
        SET_GPR_U32(ctx,2,GPR_U32(ctx,2)+1);
    }
    check(!ticket->resolved,"no early wake inside quiet window");
    check(rt.gate3ModeledEeCyclesV1()==cycle0+1000,"all1000 instruction cycles preserved");
    check(ctx->insn_count==insn0+1000,"all1000 instruction counts preserved");
    check(GPR_U32(ctx,2)==1000,"guest computation preserved");
    uint64_t generation=0;
    const auto limit=rt.gate4NativeLimitV1(ctx,generation);
    if(generic || mixed || ordered)check(limit==0,"arbitrary-predicate waits still prohibit lazy window");
    else if(!gs)check(limit>rt.gate3ModeledEeCyclesV1(),"pure future-start wait permits bounded lazy window");
    if(generic)check(polls>=1000,"generic readiness still polled at every instruction");
    if(gs){
        auto mutation=[&](auto change,const char* why){
            rt.gate3CheckpointV1(ctx);rt.gate3CheckpointV1(ctx);
            check(rt.gate3LazyPredicateV1(ctx,1,rt.gate3ModeledEeCyclesV1()),"arm quiet predicate before GS mutation");
            const auto before=rrv::guest_time::quiet_generation.load();
            change();
            check(rrv::guest_time::quiet_generation.load()!=before,why);
            check(rt.gate4NativeLimitV1(ctx,generation)==0,"GS mutation retires old quiet window");
            rt.gate3CheckpointV1(ctx); // Strict verification remains enabled in this scenario.
        };
        auto ad=[&](uint8_t reg){
            const uint64_t words[]={1ULL|(1ULL<<15)|(1ULL<<60),0xe,0,reg};
            rt.gifArbiter().submit(GifPathId::Path3,reinterpret_cast<const uint8_t*>(words),sizeof(words));
            rt.gifArbiter().drain();
        };
        mutation([&]{ad(0x61);},"masked FINISH invalidates the CPU checkpoint");
        check((rt.memory().gs().csr&2)!=0,"FINISH status remains visible");
        mutation([&]{ad(0x60);},"masked SIGNAL invalidates the CPU checkpoint");
        check((rt.memory().gs().csr&1)!=0,"SIGNAL status remains visible");
        mutation([&]{rt.memory().write8(0x12001000,1);},"CSR8 acknowledgement invalidates");
        mutation([&]{rt.memory().write16(0x12001000,2);},"CSR16 acknowledgement invalidates");
        mutation([&]{rt.memory().write32(0xb2001000,3);},"aliased CSR32 invalidates");
        mutation([&]{rt.memory().write64(0x12001010,0x7f00);},"IMR64 write invalidates");
        mutation([&]{rt.memory().write128(0x12001000,_mm_setzero_si128());},"CSR128 write invalidates");
        mutation([&]{rt.memory().writeIORegister(0x12001010,0x7f00);},"direct IMR IO write invalidates");
        mutation([&]{rt.advanceActiveGsBackendField(((rt.memory().gs().csr>>13)&1)^1);},"producer FIELD mirror invalidates");
        ctx->pc=0x2add40;
        rt.memory().write32(0x12001000,3);
        uint64_t csr=0;unsigned reads=0;
        do {
            rt.gate3CheckpointV1(ctx);
            check(rt.gate3MmioReadV1(0x12001000,8,csr),"real CSR MMIO read handled");
            rt.gate3CheckpointV1(ctx); // Would assert if FINISH recovery left a stale window.
        } while(!(csr&2) && ++reads<20001);
        check((csr&2)!=0,"existing FINISH recovery reached and remained verifier-safe");
        std::printf("PASS gs %u checks: masked SIGNAL/FINISH, control writes and FINISH recovery\n",checks);return 0;
    }
    if(invalidated){
        sched.priority(2,11); // deterministic update between the caller's read and verification
        rt.gate3LazyVerifyV1(ctx,rt.gate3ModeledEeCyclesV1());
        check(rt.gate4NativeLimitV1(ctx,generation)==0,"concurrent scheduler update retires stale lazy window");
        const auto before=rt.gate3QuietFullV1()+rt.gate3QuietFastV1();
        rt.gate3CheckpointV1(ctx);
        check(rt.gate3QuietFullV1()+rt.gate3QuietFastV1()>before,"invalidated window services the full checkpoint dispatcher");
        check(sched.quiet(1),"fallback revalidates the scheduler after metadata update");
        check(rt.gate3ModeledEeCyclesV1()==cycle0+1000,"full fallback does not lose or double-charge cycles");
    }
    if(guard){
        // Simulate an unannounced mutation with a stable generation: it must
        // still reach the production assertion. Catch its signal only in this
        // headless fixture, avoiding a macOS crash report/core dump.
        std::signal(SIGABRT,[](int){std::_Exit(86);});
        rt.memory().gs().smode1 ^= 1;
        rt.gate3LazyVerifyV1(ctx,rt.gate3ModeledEeCyclesV1());
        check(false,"stable-generation mismatch must assert");
    }
    if(mixed){
        check(!other->resolved,"unready generic wait not skipped or resolved");
        otherReady=true;rt.gate3ResolveTimedWaitsV1();
        check(other->resolved,"generic predicate resolves normally when ready");
        rt.gate3CheckpointV1(ctx);rt.gate3CheckpointV1(ctx);
        check(rt.gate4NativeLimitV1(ctx,generation)>rt.gate3ModeledEeCyclesV1(),"pure wait can optimize after generic wait retires");
    }
    if(cancel){
        check(sched.cancel(2,-418),"wait can be canceled");
        rt.gate3ResolveTimedWaitsV1();
        check(ticket->resolved && ticket->result==-418,"cancellation result retained");
        rt.gate3ResolveTimedWaitsV1();
        check(ticket->result==-418,"resolved ticket is not resolved again");
        std::printf("PASS cancel %u checks\n",checks);return 0;
    }
    unsigned steps=0;
    while(!ticket->resolved && steps++<1000){
        const auto next=owner.NextDeadline();
        check(next>=rt.gate3ModeledEeCyclesV1(),"hardware deadline does not move backward");
        rt.gate3ChargeHleV1(next-rt.gate3ModeledEeCyclesV1());
        rt.gate3CheckpointV1(ctx);
        if(owner.starts()<target)check(!ticket->resolved,"ticket stays blocked before requested start");
    }
    check(ticket->resolved && ticket->result==0,"wait resolves successfully at video start");
    check(owner.starts()==target,"exact requested start, no extra latency");
    if(ordered)check(other->resolved && sawTypedResolved,"mixed ready waits preserve original ticket order");
    std::printf("RESULT cycle=%llu insns=%llu value=%u starts=%llu now=%llu csr=%llu stat=%u result=%d\n",
        (unsigned long long)rt.gate3ModeledEeCyclesV1(),(unsigned long long)ctx->insn_count,
        GPR_U32(ctx,2),(unsigned long long)owner.starts(),(unsigned long long)owner.now(),
        (unsigned long long)rt.memory().gs().csr,owner.intc().ReadStat(),ticket->result);
    std::printf("PASS %s %u checks polls=%u fast=%llu full=%llu\n",mode,checks,polls,
        (unsigned long long)rt.gate3QuietFastV1(),(unsigned long long)rt.gate3QuietFullV1());
}
