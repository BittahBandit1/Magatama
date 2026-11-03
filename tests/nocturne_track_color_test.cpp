// Real restored 0x1667F0: independently check paired trail colors against a
// linear alpha ramp. Non-power-of-two ramps allow one quantization unit for
// repeated single-precision subtraction; RGB and all guards remain exact.
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
 PS2Runtime rt;check(rt.memory().initialize(),"actual runtime memory initializes");check(!rt.hasFunction(0x1667f0),"trail entry absent initially");
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718);registerPatches(rt);check(rt.hasFunction(0x1667f0),"product restores complete original trail routine");
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);auto* ram=rt.memory().getRDRAM();
 auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram+a,&v,4);};auto get=[&](uint32_t a){uint32_t v;std::memcpy(&v,ram+a,4);return v;};
 // Original USA ELF constant at3175D0 is the zero vector, not an inferred hue.
 std::memset(ram+0x3175d0,0,16);
 constexpr uint32_t h=0x600000,desc=0x601000,arena=0x610000,sp=0x660000,ret=0x1678cc;
 unsigned cases=0;
 for(unsigned rows:{0u,1u,3u})for(unsigned n:{0u,1u,2u,3u,4u,5u,8u,16u})for(uint32_t color:{0u,0xff80ff01u,0x9e140062u,0x80808080u,0xffffffffu,0x01020304u,0x7f424242u,0x800000ffu})for(unsigned odd:{0u,1u}){
  ++cases;std::array<uint8_t,64> header;header.fill(0x53);std::memcpy(ram+h,header.data(),header.size());
  put(h+4,rows);put(h+8,2*n+odd);put(h+20,desc);std::memcpy(header.data(),ram+h,header.size());
  std::array<uint8_t,96> descriptors;descriptors.fill(0x6b);std::memcpy(ram+desc,descriptors.data(),descriptors.size());
  std::array<uint8_t,1024> guards;guards.fill(0xa5);std::memcpy(ram+arena,guards.data(),guards.size());
  for(unsigned row=0;row<rows;++row)put(desc+20*row+4,arena+row*320+32);
  std::memcpy(descriptors.data(),ram+desc,descriptors.size());
  R5900Context c{};SET_GPR_U32((&c),4,h);SET_GPR_U32((&c),5,color);SET_GPR_U32((&c),29,sp);SET_GPR_U32((&c),31,ret);
  for(unsigned i=16;i<28;++i)c.r[i]=_mm_set1_epi32(0x6000+i);
  for(unsigned i=1;i<32;++i)c.vu0_vf[i]=_mm_castsi128_ps(_mm_set1_epi32(0x71000000+i));c.vu0_vf[0]=_mm_set_ps(1.f,0.f,0.f,0.f);
  auto before=c;uint64_t cycles=rt.gate3ModeledEeCyclesV1();c.pc=0x1667f0;rt.lookupFunction(c.pc)(ram,&c,&rt);
  check(std::memcmp(ram+h,header.data(),header.size())==0,"trail header and its counters/pointers remain read-only");
  check(std::memcmp(ram+desc,descriptors.data(),descriptors.size())==0,"all descriptor words including20byte stride remain read-only");
  bool colors=true,exactRGB=true,pairs=true,monotonic=true;int tolerance=(n&&(n&(n-1)))?1:0;
  for(unsigned row=0;row<rows;++row){int previous=256;
   for(unsigned i=0;i<n;++i){uint32_t a=get(arena+row*320+32+8*i),b=get(arena+row*320+36+8*i);int alpha=int(a>>24);
    int expected=int(double(color>>24)*(double(n-i)/double(n)));
    colors&=std::abs(alpha-expected)<=tolerance;exactRGB&=(a&0xffffff)==(color&0xffffff);pairs&=a==b;monotonic&=alpha<=previous;previous=alpha;
   }
  }
  bool intact=true;for(unsigned i=0;i<guards.size();++i){bool writable=false;
   for(unsigned row=0;row<rows;++row)writable|=i>=row*320+32&&i<row*320+32+n*8;
   if(!writable)intact&=ram[arena+i]==guards[i];
  }
  check(colors&&exactRGB,"alpha follows independent ramp within justified quantization;RGB is exact");check(pairs&&monotonic,"each pair matches and trail alpha never increases");
  check(intact,"zero loops,descriptor strides,odd count floor,and every non-output byte preserved");
  check(c.pc==ret&&GPR_U32((&c),29)==sp&&!c.in_delay_slot,"original leaf return and48byte guest stack restored");
  check(std::memcmp(c.r+16,before.r+16,12*sizeof(c.r[0]))==0,"callee-saved integer registers preserved");
  bool vu=true;for(unsigned i=0;i<32;++i)if(i!=2&&i!=3&&i!=10&&i!=11&&i!=12)vu&=std::memcmp(c.vu0_vf+i,before.vu0_vf+i,sizeof(c.vu0_vf[0]))==0;
  check(vu,"only original VF2/3/10/11/12 may change,including constant VF0 preservation");
  // The backwards branch targets1668E8 (VMOVE), not1668EC: that move
  // executes on every inner iteration, in addition to the15following ops.
  unsigned expectedInstructions=!rows?42:46+rows*(n?16+16*n:15);
  if(c.insn_count!=expectedInstructions||rt.gate3ModeledEeCyclesV1()!=cycles+expectedInstructions)
   std::fprintf(stderr,"accounting rows=%u n=%u odd=%u color=%08x actual=%llu modeled=%llu expected=%u\n",rows,n,odd,color,(unsigned long long)c.insn_count,(unsigned long long)(rt.gate3ModeledEeCyclesV1()-cycles),expectedInstructions);
  check(c.insn_count==expectedInstructions&&rt.gate3ModeledEeCyclesV1()==cycles+expectedInstructions,"all original loop,branch,delay and VU instructions accounted");
 }
 std::printf("PASS acute trail colors: %u checks (%ucases;actual product registry)\n",checks,cases);
}
