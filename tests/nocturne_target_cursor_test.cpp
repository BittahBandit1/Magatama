// Original USA target-cursor + rectangle builder, followed by the production
// HUD packet transform. Only process lookup/allocation/submission are faked.
// No renderer, device, game loop, live process or memory card is opened.
#include "ps2_runtime_macros.h"
#include "patches.h"
#include "rrv_nocturne_aspect.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
extern void sub_0025D948_0x25d948(uint8_t*,R5900Context*,PS2Runtime*);
extern void sub_0012F718_0x12f718(uint8_t*,R5900Context*,PS2Runtime*);
extern void FUN_002ba070_0x2ba070(uint8_t*,R5900Context*,PS2Runtime*);
extern void FUN_00279a88_0x279a88(uint8_t*,R5900Context*,PS2Runtime*);
using namespace magatama::aspect;
namespace {
unsigned checks=0;uint32_t nextRect=0;std::vector<std::array<uint8_t,80>> rectangles;
void check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void ret(R5900Context* c,uint32_t value=0){SET_GPR_U32(c,2,value);c->pc=GPR_U32(c,31);}
void put(uint8_t* p,uint32_t v){std::memcpy(p,&v,4);}
uint32_t get(const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;}
void qword(std::vector<uint8_t>& b,uint64_t lo,uint64_t hi){auto n=b.size();b.resize(n+16);std::memcpy(b.data()+n,&lo,8);std::memcpy(b.data()+n+8,&hi,8);}
std::vector<uint8_t> ad(std::initializer_list<std::pair<uint64_t,uint64_t>> regs){std::vector<uint8_t>b;qword(b,regs.size()|(1ull<<60),14);for(auto [v,a]:regs)qword(b,v,a);return b;}
}
int main(){
 PS2Runtime rt;check(rt.memory().initialize(),"real runtime initializes");auto* ram=rt.memory().getRDRAM();
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718);
 configure("16:9");rt.registerFunction(0x25d948,sub_0025D948_0x25d948);rt.registerFunction(0x2ba070,FUN_002ba070_0x2ba070);registerPatches(rt);
 rt.registerFunction(0x279a88,FUN_00279a88_0x279a88);
 rt.registerFunction(0x25d690,[](uint8_t*,R5900Context* c,PS2Runtime*){ret(c,0x610000);});
 rt.registerFunction(0x2b9dd0,[](uint8_t*,R5900Context* c,PS2Runtime*){ret(c,0x690000+nextRect++*128);});
 rt.registerFunction(0x2b9d78,[](uint8_t*,R5900Context* c,PS2Runtime*){ret(c,GPR_U32(c,4));});
 rt.registerFunction(0x2acb50,[](uint8_t*,R5900Context* c,PS2Runtime*){ret(c,0x6a0000);});
 for(uint32_t a:{0x2acb90u,0x2b9cc8u,0x123f00u})rt.registerFunction(a,[](uint8_t*,R5900Context* c,PS2Runtime*){ret(c);});
 rt.registerFunction(0x2acbb8,[](uint8_t* ram,R5900Context* c,PS2Runtime*){
  std::array<uint8_t,80> rectangle;std::memcpy(rectangle.data(),ram+GPR_U32(c,5),rectangle.size());rectangles.push_back(rectangle);ret(c);
 });
 put(ram+0x2f5f28,0x123f00);put(ram+0x554170-0x3008,0);put(ram+0x620000+0x12c,0x710000);
 put(ram+0x610000+0x34,96);put(ram+0x610000+0x38,160);
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);
 float worstError=0,worstOldError=0;unsigned cases=0;
 for(const char* mode:{"off","4:3","16:9","16:10","21:9","32:9","auto","auto-ultrawide","auto-portrait"})
 for(int x:{-32,0,128,256,384,512,544})for(int y:{0,112,224,336,448})for(float pulse:{0.5f,1.f,1.5f}){
  const std::string_view selected(mode);configure(selected.substr(0,4)=="auto"?"auto":mode);
  if(selected=="auto")surface(2560,1440);
  if(selected=="auto-ultrawide")surface(3440,1440);
  if(selected=="auto-portrait")surface(1080,1920);
  const float ratio=target.load(),effective=ratio>0?ratio:Original;const auto scale=hudScale(effective);
  std::memcpy(ram+0x610000+0x3c,&pulse,4);const float other=pulse*0.8f;std::memcpy(ram+0x610000+0x40,&other,4);
  auto run=[&](bool wrapped){nextRect=0;rectangles.clear();R5900Context c{};SET_GPR_U32((&c),4,0x620000);SET_GPR_S32((&c),5,x);SET_GPR_S32((&c),6,y);SET_GPR_U32((&c),28,0x554170);SET_GPR_U32((&c),29,0x680000);SET_GPR_U32((&c),31,0x123400);c.pc=0x25d948;
   for(int i=16;i<=23;++i)SET_GPR_U64((&c),i,0x1234567800000000ull+i);
   c.f[20]=123.25f;
   if(wrapped)rt.lookupFunction(0x25d948)(ram,&c,&rt);else sub_0025D948_0x25d948(ram,&c,&rt);
   check(c.pc==0x123400&&GPR_U32((&c),29)==0x680000,"original draw return and stack preserved");
   for(int i=16;i<=23;++i)check(GPR_U64((&c),i)==0x1234567800000000ull+i,"callee-saved registers preserved");
   check(c.f[20]==123.25f,"original animation FPU register preserved");check(rectangles.size()==2,"both original animation layers still submitted");return c.insn_count;
  };
  const auto beforeCount=run(false);const auto original=rectangles;const auto afterCount=run(true);check(beforeCount==afterCount,"original guest instruction accounting unchanged");
  for(unsigned layer=0;layer<2;++layer){
   const auto& a=original[layer];const auto& b=rectangles[layer];
   const int x0=int(get(b.data()+0x20)),x1=int(get(b.data()+0x40));const int y0=int(get(b.data()+0x24)),y1=int(get(b.data()+0x44));
   check(x1-x0==int(get(a.data()+0x40)-get(a.data()+0x20))&&y1-y0==int(get(a.data()+0x44)-get(a.data()+0x24)),"original animated sprite dimensions unchanged");
   for(unsigned i=0;i<80;++i)if(!((i>=0x20&&i<0x28)||(i>=0x40&&i<0x48)))check(a[i]==b[i],"UV/alpha/color/texture and non-position fields unchanged");
   if(effective==Original)check(a==b,"off and 4:3 rectangles byte-identical");
   PacketFilter f;auto setup=ad({{8ull<<16,0x4c},{1ull<<32,0x4e},{0,0x47},{0x3000ull|(8ull<<26),6},{223ull<<48,0x40},{1,0x1a},{0x156,0}});f.apply(3,setup.data(),setup.size(),effective);
   auto packet=ad({{uint64_t(uint16_t(x0))|(uint64_t(uint16_t(y0))<<16),5},{uint64_t(uint16_t(x1))|(uint64_t(uint16_t(y1))<<16),5}});
   const auto* p=f.apply(2,packet.data(),packet.size(),effective);auto coord=[&](size_t at){uint16_t n;std::memcpy(&n,p+at,2);return int(n);};
   const float cx=(coord(16)+coord(32))*0.5f,cy=(coord(18)+coord(34))*0.5f;
   const int width=x1-x0,height=y1-y0;
   const float wantX=28672.f+x*16.f+(width%32?8.f*scale.x:0.f);
   const float wantY=30976.f+y*8.f+(height%16?4.f*scale.y:0.f);
   const float error=std::max(std::abs(cx-wantX)/16.f,std::abs(cy-wantY)/8.f);worstError=std::max(error,worstError);
   if(error>0.55f)std::fprintf(stderr,"DETAIL mode=%s xy=%d,%d pulse=%.2f layer=%u raw=(%d,%d,%d,%d) mappedCenter=%.2f,%.2f expected=%.2f,%.2f scale=%.4f,%.4f error=%.4f\n",mode,x,y,pulse,layer,x0,y0,x1,y1,cx,cy,wantX,wantY,scale.x,scale.y,error);
   check(error<=0.55f,"final filtered cursor center follows world anchor within native half-pixel rounding");
   const float oldX=32768.f+((get(a.data()+0x20)+get(a.data()+0x40))*0.5f-32768.f)*scale.x;
   const float oldY=32768.f+((get(a.data()+0x24)+get(a.data()+0x44))*0.5f-32768.f)*scale.y;
   const float oldError=std::max(std::abs(oldX-wantX)/16.f,std::abs(oldY-wantY)/8.f);
   worstOldError=std::max(worstOldError,oldError);
   if(effective==16.f/9.f&&x==384&&y==224&&pulse==1.f&&layer==0)
       check(oldError>=31.9f&&error<0.5f,"same right-side enemy anchor reproduces former squeeze and is corrected");
   check(std::abs((coord(32)-coord(16))-width*scale.x)<=1.1f&&std::abs((coord(34)-coord(18))-height*scale.y)<=1.1f,"final cursor shape retains HUD proportions");
  }
  check(get(ram+0x620000+0x12c)==0x710000,"unit texture handle unchanged");++cases;
 }
 std::printf("PASS original target cursor: %u checks, %u cases, old/new worst center error=%.4f/%.4f native UI pixels; no GUI/card\n",checks,cases,worstOldError,worstError);
}
