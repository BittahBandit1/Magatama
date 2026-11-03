// Actual product-registry checks for the newly observed battle AI/particle omissions.
#include "ps2_runtime_macros.h"
#include "patches.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern void sub_0012F718_0x12f718(uint8_t*, R5900Context*, PS2Runtime*);
int main() {
 unsigned checks=0,aiCases=0,scaleCases=0;
 auto check=[&](bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}};
 PS2Runtime rt;check(rt.memory().initialize(),"real guest memory initializes");
 check(!rt.hasFunction(0x2862d0)&&!rt.hasFunction(0x162448),"entries initially absent");
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718); // real unrelated camera-registration dependency
 check(!rt.hasFunction(0xf00dc0),"synthetic missing probe address initially absent");
#ifdef _WIN32
 _putenv_s("RRV_FN_HITS","f00dc0,2862d0");
#else
 setenv("RRV_FN_HITS","f00dc0,2862d0",1);
#endif
 registerPatches(rt);check(rt.hasFunction(0x2862d0)&&rt.hasFunction(0x162448),"product restores actual entries");
 check(!rt.hasFunction(0xf00dc0),"profiling a missing entry never registers a no-op callable body");
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);auto* ram=rt.memory().getRDRAM();
 auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram+a,&v,4);};
 constexpr uint32_t ai=0x647900,control=0x640000,records=0x650000;
 const int16_t ids[8]={-32768,-1,0,1,31,32,33,32767};
 for(uint32_t mode:{0u,0x7fu,0x80u,0x81u})for(unsigned pattern=0;pattern<64;++pattern)for(uint32_t initial:{0u,0x80000001u}) {
  ++aiCases;std::array<uint8_t,512> expected;expected.fill(0xa5);std::memcpy(ram+ai,expected.data(),expected.size());
  put(ai,control);put(ai+0x190,initial);std::memcpy(expected.data(),ram+ai,expected.size());
  std::array<uint8_t,128> controller;controller.fill(0x5a);std::memcpy(controller.data()+0x60,&mode,4);uint32_t ptr=records;std::memcpy(controller.data()+0x10,&ptr,4);std::memcpy(ram+control,controller.data(),controller.size());
  std::array<uint8_t,512> units;units.fill(0x6b);uint32_t mask=initial;unsigned insns=(mode&0x80)?10:7;
  for(unsigned i=0;i<4;++i) {
   uint16_t flags=uint16_t((pattern+i*17)&63);int16_t id=ids[(pattern+i)%8];
   std::memcpy(units.data()+i*128,&flags,2);std::memcpy(units.data()+i*128+2,&id,2);
   bool eligible=(flags&1)&&!(flags&0x38);
   if(mode&0x80) {
    if(eligible)mask|=uint32_t(1)<<(uint16_t(id)%32); // independent eligible-actor set union
    insns+=!(flags&1)?8:(flags&8)?9:(flags&0x10)?11:(flags&0x20)?13:18;
   }
  }
  std::memcpy(expected.data()+0x190,&mask,4);std::memcpy(ram+records,units.data(),units.size());
  R5900Context c{};auto* ctx=&c;ctx->pc=0x2862d0;SET_GPR_U32(ctx,4,ai);SET_GPR_U32(ctx,29,0x5561c0);SET_GPR_U32(ctx,31,0x286770);
  for(unsigned j=16;j<24;++j)ctx->r[j]=_mm_set1_epi32(0x700+j);auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+ai,expected.data(),expected.size())==0,"AI merges only eligible actor bits into the original mask;all canaries preserved");
  check(std::memcmp(ram+control,controller.data(),controller.size())==0&&std::memcmp(ram+records,units.data(),units.size())==0,"AI control and actor records remain read-only");
  check(GPR_U32(ctx,2)==bool(mode&0x80),"AI returns actual enabled/disabled condition,not a constant success");
  check(ctx->pc==0x286770&&!ctx->in_delay_slot&&ctx->branch_pc==((mode&0x80)?0x286344u:0x286350u),"AI return and branch coordinates exact");
  check(GPR_U32(ctx,29)==0x5561c0&&std::memcmp(ctx->r+16,before.r+16,8*sizeof(ctx->r[0]))==0,"AI preserves stack and callee-saved integer registers");
  check(GPR_U32(ctx,4)==ai&&GPR_U32(ctx,8)==1,"AI preserves input pointer and executes initial branch delay slot");
  check(!(mode&0x80)||(GPR_U32(ctx,6)==records+512&&GPR_S64(ctx,7)==-1),"AI processes exactly four 128-byte actor records");
  check(ctx->insn_count==insns&&rt.gate3ModeledEeCyclesV1()==cycles+insns,"AI exact taken/not-taken and likely-delay instruction accounting");
  check(std::memcmp(ctx->f,before.f,sizeof(ctx->f))==0&&std::memcmp(ctx->vu0_vf,before.vu0_vf,sizeof(ctx->vu0_vf))==0,"AI preserves FPU and VU registers");
 }
 const unsigned offsets[6]={0x10,0x14,0x18,0x15c,0x164,0x168};constexpr uint32_t particle=0x660000;
 for(float scale:{0.f,-0.f,1.f,-1.f,.5f,2.f,1.25f})for(unsigned dataset=0;dataset<4;++dataset) {
  ++scaleCases;std::array<uint8_t,384> expected;expected.fill(0xa5);
  for(unsigned i=0;i<6;++i) {float value=float((int(i)-2)*int(dataset+1))+.125f;std::memcpy(expected.data()+offsets[i],&value,4);}
  std::memcpy(ram+particle,expected.data(),expected.size());
  for(unsigned i=0;i<6;++i) {float value;std::memcpy(&value,expected.data()+offsets[i],4);float result=float(double(value)*double(scale));std::memcpy(expected.data()+offsets[i],&result,4);}
  R5900Context c{};auto* ctx=&c;ctx->pc=0x162448;SET_GPR_U32(ctx,4,particle);SET_GPR_U32(ctx,29,0x5561d0);SET_GPR_U32(ctx,31,0x164ea0);
  for(unsigned j=0;j<32;++j)ctx->f[j]=float(j)+.25f;ctx->f[12]=scale;auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+particle,expected.data(),expected.size())==0,"particle scales exactly six original fields;gaps and all canaries unchanged");
  check(std::memcmp(ctx->r,before.r,sizeof(ctx->r))==0,"particle preserves all integer registers and stack");
  check(std::memcmp(ctx->f+6,before.f+6,26*sizeof(ctx->f[0]))==0,"particle preserves F6..31 including input F12 and callee-saved FPRs");
  check(std::memcmp(ctx->vu0_vf,before.vu0_vf,sizeof(ctx->vu0_vf))==0,"particle preserves all VU registers");
  check(ctx->pc==0x164ea0&&!ctx->in_delay_slot&&ctx->branch_pc==0x16248c,"particle returns after the final scale store in the delay slot");
  check(ctx->insn_count==19&&rt.gate3ModeledEeCyclesV1()==cycles+19,"particle accounts all nineteen original instructions");
 }
 std::printf("PASS AI/particle: %u checks (%u AI mask/branch cases,%u scale cases;actual product registry)\n",checks,aiCases,scaleCases);
}
