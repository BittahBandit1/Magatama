#include "ps2_runtime.h"
#include "runtime/magatama_render_pose_hook.h"
#include "nocturne_pose_math.h"
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern bool g_rrvVif1UnpackFastV1;
namespace {
std::array<uint8_t,176> previous{},current{};
unsigned calls=0;
void check(bool okay,const char* message){if(!okay){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
bool replace(uint32_t source,uint32_t command,const uint8_t* data,uint32_t bytes,uint8_t* out){
 ++calls;check(source==0x180008,"exact original RDRAM source address");check(std::memcmp(data,current.data(),bytes)==0,"immutable original payload received");
 if(command==0x6c07c000&&bytes==112)return magatama::pose::modelPacket(previous.data(),data,.5f,out);
 if(command==0x6c0bc000&&bytes==176)return magatama::pose::cameraPacket(previous.data(),data,.5f,out);
 return false;
}
}
int main(){
 PS2Memory memory;check(memory.initialize(),"actual memory initialized");unsigned cases=0;
 using namespace magatama::pose;
 for(unsigned camera=0;camera<2;++camera){
  previous.fill(0);current.fill(0);Matrix a={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},b=a;b[12]=10;
  std::memcpy(previous.data(),a.data(),64);std::memcpy(current.data(),b.data(),64);
  const uint32_t bytes=camera?176:112,command=camera?0x6c0bc000:0x6c07c000;
  if(camera){Matrix projection={2,0,0,0,0,3,0,0,0,0,-1,1,0,0,32,0};auto ap=multiply(a,projection),bp=multiply(b,projection);std::memcpy(previous.data()+64,ap.data(),64);std::memcpy(current.data()+64,bp.data(),64);float eye=-10;std::memcpy(current.data()+160,&eye,4);float one=1;std::memcpy(previous.data()+172,&one,4);std::memcpy(current.data()+172,&one,4);}
  else for(int i=0;i<3;++i){float one=1;std::memcpy(previous.data()+64+i*20,&one,4);std::memcpy(current.data()+64+i*20,&one,4);}
  std::array<uint8_t,176> expected{};check(camera?cameraPacket(previous.data(),current.data(),.5f,expected.data()):modelPacket(previous.data(),current.data(),.5f,expected.data()),"known midpoint math");
  std::vector<uint8_t> input(8+bytes);const uint32_t cycle=0x01000101;std::memcpy(input.data(),&cycle,4);std::memcpy(input.data()+4,&command,4);std::memcpy(input.data()+8,current.data(),bytes);
  for(bool fast:{false,true})for(bool enabled:{false,true}){
   memory.vif1_regs={};memory.vif1_regs.tops=0x120;std::memset(memory.getVU1Data(),0x7f,PS2_VU1_DATA_SIZE);std::memcpy(memory.getRDRAM()+0x180000,input.data(),input.size());
   g_rrvVif1UnpackFastV1=fast;g_magatamaRenderUnpackTransform=enabled?replace:nullptr;const unsigned before=calls;
   memory.processVIF1Data(0x180000,uint32_t(input.size()));
   check(std::memcmp(memory.getRDRAM()+0x180000,input.data(),input.size())==0,"guest source bytes unchanged");
   check(std::memcmp(memory.getVU1Data()+0x120*16,enabled?expected.data():current.data(),bytes)==0,"actual VIF writes exact selected rendering copy");
   check(calls-before==(enabled?1u:0u),"disabled path never invokes interpolation");++cases;
  }
 }
 g_magatamaRenderUnpackTransform=nullptr;std::printf("PASS: %u actual-runtime model/camera UNPACK cases, generic+fast paths, exact source provenance, immutable guest input, OFF byte identity\n",cases);
}
