// Real product-trampoline test with a controlled sound-command admission
// boundary. No game scripts, scheduler checks or graphics are replaced.
#include "ps2_runtime_macros.h"
#include "patches.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static uint32_t accepted=1,queryResult=0;
static void send(uint8_t*,R5900Context* c,PS2Runtime*){SET_GPR_U32(c,2,accepted);c->pc=GPR_U32(c,31);}
static void query(uint8_t*,R5900Context* c,PS2Runtime*){SET_GPR_U32(c,2,queryResult);c->pc=GPR_U32(c,31);}
int main(){
 setenv("MAGATAMA_SILENT_SOUND","1",1);
 unsigned checks=0;auto check=[&](bool x,const char* why){++checks;if(!x){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}};
 PS2Runtime rt;check(rt.memory().initialize(),"real memory initializes");
 rrv::guest_time::TemporalProfile p{};p.terminal_starts=rrv::guest_time::kNoTerminal;rt.gate3ConfigureTemporalV1(p);
 rt.registerFunction(0x2c10e8,send);rt.registerFunction(0x2c1c20,query);rt.registerFunction(0x2c4d30,send);rt.registerFunction(0x2c5098,send);
 registerPatches(rt);rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);auto* c=&rt.cpu();
 auto* ram=rt.memory().getRDRAM();
 auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram+a,&v,4);};
 auto prepare=[&](uint16_t bank){put(0x552120,0x10000);put(0x593f20,0x100);put(0x593f24,1);put(0x593f28,0x10000);put(0x10000,10);put(0x10004,0x100);put(0x10008,0x4944494d);put(0x1000c,0xd0);put(0x10028,uint32_t(bank)<<16);};
 auto request=[&](uint32_t cmd=0x1b0,uint32_t bytes=4){c->pc=0x2c10e8;SET_GPR_U32(c,31,0x700000);SET_GPR_U32(c,4,cmd);SET_GPR_U32(c,6,0x552120);SET_GPR_U32(c,7,bytes);rt.lookupFunction(c->pc)(ram,c,&rt);check(GPR_U32(c,2)==accepted,"original send result preserved");check(c->pc==0x700000,"original send return preserved");};
 auto status=[&](uint16_t bank){c->pc=0x2c1c20;SET_GPR_U32(c,31,0x700000);SET_GPR_U32(c,4,uint32_t(bank)<<16);rt.lookupFunction(c->pc)(ram,c,&rt);return GPR_U32(c,2);};
 auto next=[&]{const auto target=rt.gate3TemporalV1().starts()+1;while(rt.gate3TemporalV1().starts()<target){auto d=rt.gate3TemporalV1().NextDeadline();rt.gate3ChargeHleV1(d-rt.gate3ModeledEeCyclesV1());rt.gate3CheckpointV1(c);}};
 prepare(0x1891);request();check(status(0x1891)==0,"no same-frame completion");next();check(status(0x1891)==1,"accepted memory bank completes next frame");check(status(0x1892)==0,"unrequested bank remains unready");
 prepare(0x1892);accepted=0;request();accepted=1;next();check(status(0x1892)==0,"rejected sound command never becomes ready");
 prepare(0x1893);put(0x10000,9);request();next();check(status(0x1893)==0,"ordinary PCM is not marked as resident MIDI");
 prepare(0x1894);put(0x593f28,0x20000);request();next();check(status(0x1894)==0,"unrelated buffer request never becomes ready");
 prepare(0x1895);request(0x1b1);next();check(status(0x1895)==0,"unrelated command never completes a memory bank");
 prepare(0x1896);request(0x1b0,8);next();check(status(0x1896)==0,"wrong payload width never completes a bank");
 queryResult=2;check(status(0x5555)==2,"unknown-bank original query result preserved");queryResult=0;
 const char* path="/sound/MIDI003A.SMG";std::memcpy(ram+0x552120,path,std::strlen(path)+1);request(0xa0,20);check(status(0x3a)==0,"filename request keeps next-frame delay");next();check(status(0x3a)==1,"existing filename completion preserved");
 std::printf("PASS memory sound admission: %u checks; real product trampoline, controlled sender acceptance\n",checks);
}
