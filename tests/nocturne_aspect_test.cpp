#include "ps2_runtime_macros.h"
#include "patches.h"
#include "rrv_nocturne_aspect.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern void sub_0012F718_0x12f718(uint8_t*,R5900Context*,PS2Runtime*);
extern void FUN_002ba070_0x2ba070(uint8_t*,R5900Context*,PS2Runtime*);
extern void sub_0025D948_0x25d948(uint8_t*,R5900Context*,PS2Runtime*);
using namespace magatama::aspect;
int main(){
 unsigned checks=0;auto check=[&](bool b,const char* why){++checks;if(!b){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}};
 configure("auto");surface(3440,1440);check(std::abs(target.load()-3440.f/1440.f)<1e-6,"auto uses drawable pixels");surface(0,0);check(target.load()>2,"minimize does not create zero aspect");
 bool bad=false;try{configure("garbage");}catch(const std::invalid_argument&){bad=true;}check(bad,"invalid mode rejected");
 for(float ratio:{4.f/3.f,16.f/9.f,1.6f,21.f/9.f,32.f/9.f,9.f/16.f}){
  const auto f=hudScale(ratio);check(std::abs(f.x*ratio/f.y-Original)<1e-6,"HUD retains physical4:3 shape including portrait");check(f.x<=1&&f.y<=1&&f.x>0&&f.y>0,"entire HUD fits within frame");
 }
 PS2Runtime rt;check(rt.memory().initialize(),"actual runtime memory initializes");auto* ram=rt.memory().getRDRAM();
 auto elf=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"disc/SLUS_209.11";
 std::ifstream in(elf,std::ios::binary);std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),{});check(data.size()>52&&std::memcmp(data.data(),"\x7f" "ELF",4)==0,"read-only original ELF available");
 auto u16=[&](size_t o){uint16_t v;std::memcpy(&v,data.data()+o,2);return v;};auto u32=[&](size_t o){uint32_t v;std::memcpy(&v,data.data()+o,4);return v;};
 for(unsigned i=0;i<u16(44);++i){size_t p=u32(28)+i*u16(42);if(u32(p)!=1)continue;uint32_t dst=u32(p+8),size=u32(p+16),off=u32(p+4);check(uint64_t(dst)+size<=32u*1024*1024&&uint64_t(off)+size<=data.size(),"ELF load segment bounds");std::memcpy(ram+dst,data.data()+off,size);}
 rt.registerFunction(0x12f718,sub_0012F718_0x12f718);rt.registerFunction(0x2ba070,FUN_002ba070_0x2ba070);rt.registerFunction(0x25d948,sub_0025D948_0x25d948);configure("16:9");registerPatches(rt);
 rrv::guest_time::context_id=1;PS2Runtime::GuestExecutionScope scope(&rt);
 for(uint32_t a:{0x2f59b0u,0x2f5d50u,0x2f5b80u}){
  std::array<uint8_t,160> original;std::memcpy(original.data(),ram+a,160);
  for(float fovy:{0.4f,0.6283185f,1.0471976f})for(const char* mode:{"4:3","16:9","16:10","21:9","32:9","auto"}){
   auto start=original;std::memcpy(start.data()+12,&fovy,4);
   auto run=[&](bool wrapped){std::memcpy(ram+a,start.data(),160);R5900Context c{};c.vu0_vf[0]=_mm_set_ps(1,0,0,0);SET_GPR_U32((&c),4,a);SET_GPR_U32((&c),28,0x554170);SET_GPR_U32((&c),29,0x680000);SET_GPR_U32((&c),31,0x123400);c.pc=0x2ba070;
    if(wrapped)rt.lookupFunction(c.pc)(ram,&c,&rt);else FUN_002ba070_0x2ba070(ram,&c,&rt);
    check(c.pc==0x123400&&GPR_U32((&c),29)==0x680000,"real projection builder restores return/stack");return c.insn_count;};
   auto n=run(false);std::array<uint8_t,160> stock;std::memcpy(stock.data(),ram+a,160);
   configure(mode);if(std::string_view(mode)=="auto")surface(3440,1440);auto m=run(true);
   check(n==m,"enhancement preserves original guest instruction accounting");
   float x,y;std::memcpy(&x,stock.data()+48,4);std::memcpy(&y,ram+a+48,4);float want=a==0x2f59b0?x*(Original/target.load()):x;
   check(std::isfinite(x)&&std::isfinite(y)&&std::abs(y-want)<=std::max(1e-5f,std::abs(want)*2e-6f),"real original projection gains correct horizontal view only for main camera");
   std::memcpy(stock.data()+48,&y,4);check(std::memcmp(ram+a,stock.data(),160)==0,"all other camera/depth/viewport inputs and matrix entries unchanged;aspect restored");
   auto once=stock;run(true);check(std::memcmp(ram+a,once.data(),160)==0,"repeated frames do not compound aspect scaling");
  }
 }
 std::printf("PASS Nocturne aspect projection: %u checks (real USA builder, no GUI/card)\n",checks);
}
