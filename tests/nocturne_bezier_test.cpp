// Restored original routines exercised through the real product registry.
// Expected positions use an independent double-precision Bernstein formula.
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
 PS2Runtime rt;check(rt.memory().initialize(),"real memory initializes");
 check(!rt.hasFunction(0x17cd90)&&!rt.hasFunction(0x1ac298),"entries initially absent");
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718); // real dependency of product camera registration, not exercised here
 registerPatches(rt);check(rt.hasFunction(0x17cd90)&&rt.hasFunction(0x1ac298),"product restores both entries");
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);auto* ram=rt.memory().getRDRAM();
 auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram+a,&v,4);};auto get=[&](uint32_t a){uint32_t v;std::memcpy(&v,ram+a,4);return v;};
 const uint32_t h=0x600000,dst=0x600200,src=0x600500;
 std::array<uint8_t,48> input;for(unsigned i=0;i<48;++i)input[i]=uint8_t(i*29+7);
 for(unsigned capacity:{9u,12u,18u})for(unsigned current:{0u,3u,capacity-3})for(unsigned count:{0u,capacity-6,capacity-3}){
  std::array<uint8_t,64> header;header.fill(0xa5);std::memcpy(ram+h,header.data(),header.size());
  put(h+8,capacity);put(h+12,count);put(h+16,current);put(h+24,dst);std::memcpy(header.data(),ram+h,header.size());
  std::array<uint8_t,384> expected;expected.fill(0xcc);std::memcpy(ram+dst-32,expected.data(),expected.size());
  std::memcpy(ram+src,input.data(),48);std::memcpy(expected.data()+32+16*current,input.data(),48);
  const bool wrap=current+3==capacity,inc=count<capacity-3;
  if(wrap)std::memcpy(expected.data()+32,input.data(),48);
  uint32_t next=wrap?3:current+3,ncount=inc?count+3:count;
  std::memcpy(header.data()+16,&next,4);std::memcpy(header.data()+12,&ncount,4);
  R5900Context c{};auto* ctx=&c;ctx->pc=0x17cd90;SET_GPR_U32(ctx,4,h);SET_GPR_U32(ctx,5,src);SET_GPR_U32(ctx,29,0x660000);SET_GPR_U32(ctx,31,0x17c534);
  for(unsigned i=16;i<24;++i)ctx->r[i]=_mm_set1_epi32(0x600+i);auto before=c;
  uint64_t cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+h,header.data(),header.size())==0,"history header exact, only current/count changed");
  check(std::memcmp(ram+dst-32,expected.data(),expected.size())==0,"three quadword copies/wrap and all canaries exact");
  check(ctx->pc==0x17c534&&GPR_U32(ctx,29)==0x660000&&!ctx->in_delay_slot,"history returns through original RA");
  unsigned expectedInsns=(wrap?37u:27u)+unsigned(inc);
  check(ctx->insn_count==expectedInsns&&rt.gate3ModeledEeCyclesV1()==cycles+expectedInsns,"history delay/likely-slot instruction accounting");
  check(std::memcmp(ctx->r+16,before.r+16,8*sizeof(ctx->r[0]))==0,"history callee-saved registers intact");
 }
 const uint32_t curve=0x610000,output=0x610200;
 float points[7][3]={{1,2,-3},{3,-4,5},{-7,8,9},{10,-11,12},{13,14,-15},{-16,17,18},{19,20,21}};
 for(unsigned segment:{0u,3u})for(float t:{0.f,.25f,.5f,.75f,1.f,1.25f,-.25f})for(float step:{0.f,.125f,.5f}){
  std::array<uint8_t,0x80> expected{};std::memcpy(expected.data(),points,sizeof(points));std::memcpy(expected.data()+0x54,&segment,4);std::memcpy(expected.data()+0x58,&t,4);std::memcpy(expected.data()+0x5c,&step,4);std::memcpy(ram+curve,expected.data(),expected.size());
  std::memset(ram+output-16,0xaa,48);
  R5900Context c{};auto* ctx=&c;ctx->pc=0x1ac298;SET_GPR_U32(ctx,4,curve);SET_GPR_U32(ctx,5,output);SET_GPR_U32(ctx,29,0x660000);SET_GPR_U32(ctx,31,0x17c228);
  for(unsigned i=16;i<24;++i)ctx->r[i]=_mm_set1_epi32(0x700+i);auto before=c;uint64_t cycles=rt.gate3ModeledEeCyclesV1();
  rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  float p[4];std::memcpy(p,ram+output,16);double x=t,u=1.-x;
  for(unsigned k=0;k<3;++k){double v=u*u*u*points[segment][k]+3*u*u*x*points[segment+1][k]+3*u*x*x*points[segment+2][k]+x*x*x*points[segment+3][k];check(std::fabs(double(p[k])-v)<.0002,"cubic position matches independent polynomial");}
  check(p[3]==1.f,"homogeneous W is exactly one");
  float next=t+step;unsigned idx=segment;bool ended=next>1.f&&segment>=3;
  if(ended)next=1.f;else if(next>1.f){next-=1.f;idx+=3;}
  std::memcpy(expected.data()+0x54,&idx,4);std::memcpy(expected.data()+0x58,&next,4);
  check(std::memcmp(ram+curve,expected.data(),expected.size())==0,"curve progression exact, all other bytes unchanged");
  bool guards=true;for(unsigned i=0;i<16;++i)guards&=ram[output-16+i]==0xaa&&ram[output+16+i]==0xaa;check(guards,"output canaries intact");
  check(GPR_U32(ctx,2)==unsigned(!ended),"completion result exact");
  check(ctx->pc==0x17c228&&GPR_U32(ctx,29)==0x660000&&!ctx->in_delay_slot,"curve restores stack and return address");
  unsigned n=t+step<=1.f?71:ended?72:75;check(ctx->insn_count==n&&rt.gate3ModeledEeCyclesV1()==cycles+n,"curve branch/delay instruction accounting");
  check(std::memcmp(ctx->r+16,before.r+16,8*sizeof(ctx->r[0]))==0,"curve callee-saved registers intact");
 }
 // Reset must overwrite stale history state but preserve the object's pointers.
 check(rt.hasFunction(0x17cb88)&&rt.hasFunction(0x188b40),"reset and rotation entries registered");
 for(unsigned fill:{0u,0x5au,0xffu}){
  std::array<uint8_t,64> expected;expected.fill(uint8_t(fill));std::memcpy(ram+h,expected.data(),expected.size());
  uint32_t color=0x80808080u,count=0,current=3;
  std::memcpy(expected.data(),&color,4);std::memcpy(expected.data()+12,&count,4);std::memcpy(expected.data()+16,&current,4);
  R5900Context c{};auto* ctx=&c;ctx->pc=0x17cb88;SET_GPR_U32(ctx,4,h);SET_GPR_U32(ctx,29,0x660000);SET_GPR_U32(ctx,31,0x175bc0);
  auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+h,expected.data(),expected.size())==0,"reset updates colour/count/current only, preserves pointers and canaries");
  check(GPR_U64(ctx,2)==0xffffffff80808080ull&&GPR_U32(ctx,3)==3,"reset reproduces sign-extended original GPR outputs");
  check(std::memcmp(ctx->r+4,before.r+4,28*sizeof(ctx->r[0]))==0,"reset preserves every other integer register");
  check(ctx->pc==0x175bc0&&ctx->branch_pc==0x17cb9c&&!ctx->in_delay_slot,"reset returns after original delay-slot count store");
  check(ctx->insn_count==7&&rt.gate3ModeledEeCyclesV1()==cycles+7,"reset accounts all7original instructions");
 }
 // All four LQC2 loads precede the stores: source/destination overlap must work.
 for(int delta:{-64,-16,0,16,64,128}){
  constexpr uint32_t arena=0x620000,source=arena+96;uint32_t destination=source+delta;
  std::array<uint8_t,384> expected;for(unsigned j=0;j<expected.size();++j)expected[j]=uint8_t(j*53+19);
  std::memcpy(ram+arena,expected.data(),expected.size());std::array<uint8_t,64> matrix;
  std::memcpy(matrix.data(),expected.data()+96,64);std::memcpy(expected.data()+96+delta,matrix.data(),64);
  R5900Context c{};auto* ctx=&c;ctx->pc=0x188b40;SET_GPR_U32(ctx,4,destination);SET_GPR_U32(ctx,5,source);SET_GPR_U32(ctx,31,0x175bc0);
  for(unsigned j=0;j<32;++j)ctx->vu0_vf[j]=_mm_castsi128_ps(_mm_set1_epi32(0x70000000+j));
  auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+arena,expected.data(),expected.size())==0,"rotation copies64bytes bit-for-bit including overlapping buffers, guards preserved");
  check(std::memcmp(ctx->vu0_vf+28,matrix.data(),64)==0,"rotation updates VF28..31 exactly, including non-finite bit patterns");
  check(std::memcmp(ctx->vu0_vf,before.vu0_vf,28*sizeof(ctx->vu0_vf[0]))==0,"rotation preserves VF0..27");
  check(std::memcmp(ctx->r,before.r,sizeof(ctx->r))==0,"rotation preserves all integer registers");
  check(ctx->pc==0x175bc0&&ctx->branch_pc==0x188b60&&!ctx->in_delay_slot,"rotation returns through original JR/NOP");
  check(ctx->insn_count==10&&rt.gate3ModeledEeCyclesV1()==cycles+10,"rotation accounts10original instructions");
 }
 check(rt.hasFunction(0x1824c0),"indirect ring-matrix entry registered");
 for(int delta:{-64,-16,0,16,64,128}){
  constexpr uint32_t arena=0x620000,source=arena+96,object=0x630000;uint32_t destination=source+delta;
  std::array<uint8_t,384> expected;for(unsigned j=0;j<expected.size();++j)expected[j]=uint8_t(j*71+23);
  std::memcpy(ram+arena,expected.data(),expected.size());std::array<uint8_t,64> matrix;std::memcpy(matrix.data(),expected.data()+96,64);std::memcpy(expected.data()+96+delta,matrix.data(),64);
  std::array<uint8_t,160> header;header.fill(0x7c);std::memcpy(header.data()+0x7c,&destination,4);std::memcpy(ram+object,header.data(),header.size());
  R5900Context c{};auto* ctx=&c;ctx->pc=0x1824c0;SET_GPR_U32(ctx,4,object);SET_GPR_U32(ctx,5,source);SET_GPR_U32(ctx,31,0x175bc0);
  for(unsigned j=0;j<32;++j)ctx->vu0_vf[j]=_mm_castsi128_ps(_mm_set1_epi32(0x71000000+j));
  auto before=c;auto cycles=rt.gate3ModeledEeCyclesV1();rt.lookupFunction(ctx->pc)(ram,ctx,&rt);
  check(std::memcmp(ram+arena,expected.data(),expected.size())==0,"indirect ring rotation copies64bytes with overlapping buffers and guards");
  check(std::memcmp(ram+object,header.data(),header.size())==0,"ring rotation reads object pointer at7c without modifying header");
  check(std::memcmp(ctx->vu0_vf+28,matrix.data(),64)==0&&std::memcmp(ctx->vu0_vf,before.vu0_vf,28*sizeof(ctx->vu0_vf[0]))==0,"ring rotation exact VF28..31 updates and other VF preservation");
  check(GPR_U64(ctx,2)==destination&&std::memcmp(ctx->r+3,before.r+3,29*sizeof(ctx->r[0]))==0,"ring rotation original V0 pointer result and other GPR preservation");
  check(ctx->pc==0x175bc0&&ctx->branch_pc==0x1824e4&&!ctx->in_delay_slot,"ring rotation returns through original JR/NOP");
  check(ctx->insn_count==11&&rt.gate3ModeledEeCyclesV1()==cycles+11,"ring rotation accounts11original instructions");
 }
 std::printf("PASS restored effect routines: %u checks (27history,42curve,3reset,12rotation cases;actual product registry)\n",checks);
}
