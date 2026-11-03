// Selective generation using the product's own decoder/emitter sources.
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2_runtime_calls.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace ps2recomp;
// Same implementation as ps2_recompiler.cpp. No full ELF analyzer is needed
// for independently verified exact function extents.
StubTarget PS2Recompiler::resolveStubTarget(const std::string& name){
 if(!ps2_runtime_calls::resolveSyscallName(name).empty())return StubTarget::Syscall;
 if(!ps2_runtime_calls::resolveStubName(name).empty())return StubTarget::Stub;
 return StubTarget::Unknown;
}
int main(int argc,char**argv){
 if(argc!=3)return 2;
 std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> elf((std::istreambuf_iterator<char>(file)),{});
 auto u16=[&](size_t at){uint16_t n;std::memcpy(&n,elf.data()+at,2);return n;};
 auto u32=[&](size_t at){uint32_t n;std::memcpy(&n,elf.data()+at,4);return n;};
 if(elf.size()<52||std::memcmp(elf.data(),"\x7f" "ELF",4))return 2;
 auto read=[&](uint32_t address){for(unsigned i=0;i<u16(44);++i){auto p=u32(28)+i*u16(42);if(u32(p)==1&&address>=u32(p+8)&&uint64_t(address)+4<=uint64_t(u32(p+8))+u32(p+16))return u32(u32(p+4)+address-u32(p+8));}throw std::runtime_error("address outside ELF load segment");};
 std::vector<Section> sections;
 std::vector<Symbol> symbols={{"nocturneBezierTrackSetVC",0x17cd90,0x98,true,false,false},{"nocturneBezierEval",0x1ac298,0x13c,true,false,false},{"nocturneBezierTrackReset",0x17cb88,0x1c,true,false,false},{"nocturneSphereRotateMatrix",0x188b40,0x28,true,false,false},{"nocturneRingRotateMatrix",0x1824c0,0x2c,true,false,false},{"nocturneAiPowerMask",0x2862d0,0x88,true,false,false},{"nocturneSparkOffsetScale",0x162448,0x4c,true,false,false},{"nocturneBufuIceRotateMatrix",0x192440,0x2c,true,false,false},{"nocturneBufuLightCreate",0x192a70,0x10,true,false,false},{"nocturneBufuLightCopy",0x192a80,0x10,true,false,false},{"nocturneBufuLightDelete",0x192a90,0x8,true,false,false},{"nocturneBufuLightDraw",0x192a98,0x44,true,false,false},{"nocturneFieldLightDirection",0x16be98,0x1c,true,false,false},{"nocturneFieldLightAxis",0x16beb8,0x18,true,false,false},{"nocturneFieldLightAnimation",0x16bed0,0x24,true,false,false},{"nocturneTrackColorAcute",0x1667f0,0x150,true,false,false}};
 R5900Decoder decoder;CodeGenerator generator(symbols,sections);std::ofstream out(argv[2]);
 out<<"// Restored omitted USA SLUS_209.11 battle/effect routines. Generated from the locked\n// original ELF with the product's vendored decoder/emitter; see tools/nocturne-effects.\n";
 out<<"#include \"ps2_runtime_macros.h\"\n#include \"ps2_runtime.h\"\n";
 for(const auto& symbol:symbols)out<<"void "<<symbol.name<<"(uint8_t*, R5900Context*, PS2Runtime*);\n";
 for(auto [start,size,name]:{std::tuple{0x17cd90u,0x98u,"nocturneBezierTrackSetVC"},std::tuple{0x1ac298u,0x13cu,"nocturneBezierEval"},std::tuple{0x17cb88u,0x1cu,"nocturneBezierTrackReset"},std::tuple{0x188b40u,0x28u,"nocturneSphereRotateMatrix"},std::tuple{0x1824c0u,0x2cu,"nocturneRingRotateMatrix"},std::tuple{0x2862d0u,0x88u,"nocturneAiPowerMask"},std::tuple{0x162448u,0x4cu,"nocturneSparkOffsetScale"},std::tuple{0x192440u,0x2cu,"nocturneBufuIceRotateMatrix"},std::tuple{0x192a70u,0x10u,"nocturneBufuLightCreate"},std::tuple{0x192a80u,0x10u,"nocturneBufuLightCopy"},std::tuple{0x192a90u,0x8u,"nocturneBufuLightDelete"},std::tuple{0x192a98u,0x44u,"nocturneBufuLightDraw"},std::tuple{0x16be98u,0x1cu,"nocturneFieldLightDirection"},std::tuple{0x16beb8u,0x18u,"nocturneFieldLightAxis"},std::tuple{0x16bed0u,0x24u,"nocturneFieldLightAnimation"},std::tuple{0x1667f0u,0x150u,"nocturneTrackColorAcute"}}){
  Function fn{};fn.name=name;fn.start=start;fn.end=start+size;
  for(uint32_t a=start;a<fn.end;a+=4)fn.instructions.push_back(decoder.decodeInstruction(a,read(a)));
  out<<generator.generateFunction(fn,fn.instructions,true)<<"\n";
 }
 return out?0:1;
}
