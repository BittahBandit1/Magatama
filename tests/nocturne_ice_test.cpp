// Actual restored USA ice/light functions and their three real field-light
// dependencies. No simulated effect body, renderer, card, or GUI is used.
#include "ps2_runtime_macros.h"
#include "patches.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern void sub_0012F718_0x12f718(uint8_t*,R5900Context*,PS2Runtime*);
int main(){
 unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}};
 PS2Runtime rt;check(rt.memory().initialize(),"actual runtime memory initializes");
 const uint32_t entries[]={0x192440,0x192a70,0x192a80,0x192a90,0x192a98,0x16be98,0x16beb8,0x16bed0};
 for(auto a:entries)check(!rt.hasFunction(a),"ice/dependency absent before product registration");
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718);registerPatches(rt);
 for(auto a:entries)check(rt.hasFunction(a),"product supplies exact ice/dependency body");
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);auto* ram=rt.memory().getRDRAM();
 auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram+a,&v,4);};
 auto get=[&](uint32_t a){uint32_t v;std::memcpy(&v,ram+a,4);return v;};
 constexpr uint32_t gp=0x554170,flag=0x54f1ec,sp=0x660000,ret=0x175bc0;
 auto run=[&](uint32_t entry,R5900Context& c){c.pc=entry;SET_GPR_U32((&c),28,gp);SET_GPR_U32((&c),29,sp);SET_GPR_U32((&c),31,ret);rt.lookupFunction(entry)(ram,&c,&rt);};
 for(int delta:{-64,-16,0,16,64,128}){
  constexpr uint32_t arena=0x620000,object=arena+96,destination=object+16;
  uint32_t source=destination+delta;std::array<uint8_t,384> expected;
  for(unsigned i=0;i<expected.size();++i)expected[i]=uint8_t(i*71+23);
  std::memcpy(ram+arena,expected.data(),expected.size());std::array<uint8_t,64> matrix;
  std::memcpy(matrix.data(),ram+source,64);std::memcpy(expected.data()+112,matrix.data(),64);
  R5900Context c{};SET_GPR_U32((&c),4,object);SET_GPR_U32((&c),5,source);
  for(unsigned i=0;i<32;++i)c.vu0_vf[i]=_mm_castsi128_ps(_mm_set1_epi32(0x71234567+i));
  auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();run(0x192440,c);
  check(std::memcmp(ram+arena,expected.data(),expected.size())==0,"ice matrix copies all64bytes at object+10, including overlap and canaries");
  check(std::memcmp(c.vu0_vf+28,matrix.data(),64)==0,"matrix produces exact VF28..31 bits");
  check(std::memcmp(c.vu0_vf,before.vu0_vf,28*sizeof(c.vu0_vf[0]))==0,"matrix preserves VF0..27");
  check(GPR_U32((&c),4)==destination&&GPR_U32((&c),5)==source,"original A0 advance and A1 preserved");
  check(c.pc==ret&&!c.in_delay_slot&&c.insn_count==11,"matrix original return and11instructions");
  check(rt.gate3ModeledEeCyclesV1()==cycles+11,"matrix modeled cycles exact");
 }
 for(auto entry:{0x192a70u,0x192a80u,0x192a90u})for(unsigned value:{0u,1u,0x80u,0xffu}){
  std::array<uint8_t,48> expected;expected.fill(0x5a);expected[16]=uint8_t(value);std::memcpy(ram+flag-16,expected.data(),expected.size());
  R5900Context c{};for(unsigned i=1;i<32;++i)c.r[i]=_mm_set1_epi32(0x123400+i);
  auto cycles=rt.gate3ModeledEeCyclesV1();run(entry,c);
  if(entry!=0x192a90)expected[16]=1;
  check(std::memcmp(ram+flag-16,expected.data(),expected.size())==0,"create/copy set only original byte;delete is original empty leaf");
  if(entry!=0x192a90)check(GPR_U64((&c),2)==0&&GPR_U64((&c),3)==1,"create/copy original zero result and V1=1");
  check(c.pc==ret&&GPR_U32((&c),29)==sp&&!c.in_delay_slot,"lifecycle original return and stack");
  unsigned n=entry==0x192a90?2:4;check(c.insn_count==n&&rt.gate3ModeledEeCyclesV1()==cycles+n,"lifecycle exact instruction accounting");
 }
 for(unsigned value:{0u,1u,0x80u,0xffu})for(float factor:{0.f,1.f,7.f,-28.f}){
  constexpr uint32_t object=0x640000,axis=0x5699f0,anim=gp-0x23e4,scale=gp-0x78c0;
  std::array<uint8_t,96> expectedObject;std::array<uint8_t,64> expectedAxis;std::array<uint8_t,48> expectedFlag,expectedAnim;
  expectedObject.fill(0x3c);expectedAxis.fill(0x6b);expectedFlag.fill(0x59);expectedFlag[16]=uint8_t(value);expectedAnim.fill(0x7d);
  std::memcpy(ram+object,expectedObject.data(),expectedObject.size());std::memcpy(ram+axis-16,expectedAxis.data(),expectedAxis.size());
  std::memcpy(ram+flag-16,expectedFlag.data(),expectedFlag.size());std::memcpy(ram+anim-16,expectedAnim.data(),expectedAnim.size());
  put(0x2f5b40,object);std::memcpy(ram+scale,&factor,4);
  std::array<uint8_t,32> input;for(unsigned i=0;i<input.size();++i)input[i]=uint8_t(i*39+0x80);std::memcpy(ram+0x31fa10,input.data(),input.size());
  if(value){
   std::memcpy(expectedObject.data()+16,input.data(),16);std::memcpy(expectedAxis.data()+16,input.data()+16,16);expectedFlag[16]=0;
   uint32_t duration=14,zero=0;float step=factor/14.f;
   std::memcpy(expectedAnim.data()+16,&duration,4);std::memcpy(expectedAnim.data()+20,&zero,4);std::memcpy(expectedAnim.data()+24,&step,4);
  }
  R5900Context c{};c.f[12]=123.f;auto cycles=rt.gate3ModeledEeCyclesV1();run(0x192a98,c);
  check(std::memcmp(ram+object,expectedObject.data(),expectedObject.size())==0,"draw original direction dependency copies16bytes through live field object");
  check(std::memcmp(ram+axis-16,expectedAxis.data(),expectedAxis.size())==0,"draw original axis dependency copies16bytes and preserves guards");
  check(std::memcmp(ram+anim-16,expectedAnim.data(),expectedAnim.size())==0,"draw original animation duration14,frame0,step factor/14 with guards");
  check(std::memcmp(ram+flag-16,expectedFlag.data(),expectedFlag.size())==0,"draw consumes exactly one pending flag;zero path leaves memory untouched");
  check(std::memcmp(ram+0x31fa10,input.data(),input.size())==0&&get(0x2f5b40)==object,"direction/axis sources and field-object pointer remain intact");
  uint64_t savedRA;std::memcpy(&savedRA,ram+sp-16,8);check(savedRA==ret,"draw saves original RA on guest stack");
  check(c.pc==ret&&GPR_U32((&c),29)==sp&&!c.in_delay_slot,"draw returns correctly through three real dependencies");
  unsigned n=value?39:7;check(c.insn_count==n&&rt.gate3ModeledEeCyclesV1()==cycles+n,"draw and real dependency branch/delay/cycle accounting");
  check(c.f[12]==(value?factor/14.f:123.f),"original F12 result or unchanged zero-flag path");
 }
 std::printf("PASS ice/light: %u checks (6overlap,12lifecycle,16draw cases;three real dependencies)\n",checks);
}
