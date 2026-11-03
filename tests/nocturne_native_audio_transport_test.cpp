#include "ps2_runtime_macros.h"
#include "../src/audio/nocturne_native_sound.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(){
 unsigned checks=0;
 auto check=[&](bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}};
 setenv("MAGATAMA_SOUND_NATIVE","1",1);
 PS2Runtime rt;check(rt.memory().initialize(),"real memory initialization");
 rrv::guest_time::TemporalProfile profile{};profile.terminal_starts=rrv::guest_time::kNoTerminal;
 rt.gate3ConfigureTemporalV1(profile);rrv::guest_time::context_id=1;
 PS2Runtime::GuestExecutionScope execution(&rt);
 auto* c=&rt.cpu();auto* ram=rt.memory().getRDRAM();c->pc=0x100000;rt.gate3CheckpointV1(c);
 auto original=[](uint8_t*,R5900Context* c,PS2Runtime*){c->pc=GPR_U32(c,31);};
 for(uint32_t a:{0x2e2b98u,0x2e2c08u,0x2e2d00u,0x2dc260u,0x2dc270u,0x2de920u,0x2de2b0u,0x2de2f0u,0x2df010u,0x2df428u,0x2c0ff8u})rt.registerFunction(a,original);
 check(magatama::native_sound::install(rt),"native transport installed without starting modules or SDL");
 auto call=[&](uint32_t pc,uint32_t a,uint32_t b=0,uint32_t d=0,uint32_t e=0){
  c->pc=pc;SET_GPR_U32(c,31,0x123000u);SET_GPR_U32(c,4,a);SET_GPR_U32(c,5,b);SET_GPR_U32(c,6,d);SET_GPR_U32(c,7,e);
  rt.lookupFunction(pc)(ram,c,&rt);check(c->pc==0x123000u,"SIF return address preserved");return GPR_U32(c,2);
 };
 for(auto bytes:{0u,0xffffffffu,0x200001u})check(call(0x2e2b98u,bytes)==0,"oversized/zero allocation rejected");
 auto address=call(0x2e2b98u,128);check(address>=0x10000u&&address+128<=0x200000u,"physical IOP address allocated");
 std::memset(ram+address,0x77,128);
 std::array<uint8_t,128> pattern{};for(unsigned n=0;n<pattern.size();++n)pattern[n]=uint8_t(n*17);
 std::memcpy(ram+0x600000,pattern.data(),pattern.size());
 uint32_t desc[8]={0x600000,address,128,0,0x600000,0x200000,128,0};std::memcpy(ram+0x600400,desc,sizeof(desc));
 check(call(0x2dc260,0x600400,1)>0,"valid EE-to-IOP DMA accepted");
 for(unsigned n=0;n<128;++n)check(ram[address+n]==0x77,"IOP transfer does not alias low EE RAM");
 check(call(0x2de920,0,address,0x600200,128)==0,"IOP-to-EE readback accepted");
 check(std::memcmp(ram+0x600200,pattern.data(),128)==0,"DMA payload round-trip exact");
 std::memset(ram+0x600000,0xee,128);
 check(call(0x2dc260,0x600400,2)==0,"mixed invalid DMA list rejected atomically");
 check(call(0x2de920,0,address,0x600200,128)==0,"readback after rejected DMA");
 check(std::memcmp(ram+0x600200,pattern.data(),128)==0,"no partial DMA mutation");
 check(call(0x2de920,0,address,0x01ffffc0,128)==0xffffffffu,"EE backing boundary crossing rejected");
 check(call(0x2e2d00,address)==0,"owned EE allocation released");
 check(call(0x2e2d00,address)==0xffffffffu,"double free rejected");
 std::memset(ram+0x600200,0x55,128);
 check(call(0x2de920,0,address,0x600200,128)==0xffffffffu,"read of released IOP memory rejected");
 for(unsigned n=0;n<128;++n)check(ram[0x600200+n]==0x55,"failed read preserves destination");
 std::printf("PASS native audio transport: %u real-runtime checks; no game/card/audio device\n",checks);
}
