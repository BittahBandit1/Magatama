#include "rrv_nocturne_aspect.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <chrono>
using namespace magatama::aspect;
using Bytes=std::vector<uint8_t>;
void qword(Bytes& b,uint64_t a,uint64_t c=0){size_t n=b.size();b.resize(n+16);std::memcpy(b.data()+n,&a,8);std::memcpy(b.data()+n+8,&c,8);}
Bytes ad(std::initializer_list<std::pair<uint64_t,uint64_t>> rs){Bytes b;qword(b,rs.size()|(1ull<<60),14);for(auto [v,r]:rs)qword(b,v,r);return b;}
uint64_t xyz(unsigned x,unsigned y,uint32_t z=0){return uint64_t(x*16)|(uint64_t(y*16)<<16)|(uint64_t(z)<<32);}
uint16_t xy(const uint8_t* p){uint16_t n;std::memcpy(&n,p,2);return n;}
int main(int argc,char**argv){
 unsigned checks=0;auto check=[&](bool b,const char* why){++checks;if(!b){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}};
 auto setup=ad({{8ull<<16,0x4c},{1ull<<32,0x4e},{0,0x47},{0x3000ull|(8ull<<26),6},{223ull<<48,0x40},{1,0x1a},{0x156,0}});
 auto sprite=ad({{0x0123456789abcdefull,3},{xyz(1792,1936),5},{xyz(2304,2160),5},{0x778899,0x61},{0x11223344,0x60}});
 for(float ratio:{Original,16.f/9,1.6f,21.f/9,32.f/9,9.f/16}){
  PacketFilter f;f.apply(3,setup.data(),setup.size(),ratio);auto input=sprite;auto* p=f.apply(2,input.data(),input.size(),ratio);auto scale=hudScale(ratio);
  auto map=[](int x,float s){return uint16_t(std::lround(32768+(x*16-32768)*s));};
  check(xy(p+32)==map(1792,scale.x)&&xy(p+34)==map(1936,scale.y),"HUD top-left preserves centered physical proportions");
  check(xy(p+48)==map(2304,scale.x)&&xy(p+50)==map(2160,scale.y),"HUD bottom-right preserves centered physical proportions");
  check(input==sprite,"source packet never mutated");
  for(size_t i=0;i<input.size();++i)if(!((i>=32&&i<36)||(i>=48&&i<52)))check(p[i]==input[i],"all tag,UV,depth,control and padding bytes preserved");
 }
 {
  PacketFilter f;auto b=sprite;check(f.apply(2,b.data(),b.size(),0)==b.data()&&b==sprite,"off mode is exact no-op, no copy");
  f.apply(3,setup.data(),setup.size(),16.f/9);check(f.apply(1,b.data(),b.size(),16.f/9)==b.data(),"PATH1 world vertices unchanged");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);
  // PACKED UV + XYZF with PRE; arbitrary reserved bits/ADC remain untouched.
  Bytes b;qword(b,1ull|(1ull<<46)|(uint64_t(0x156)<<47)|(2ull<<60),0x43);
  qword(b,0x1234005678ull);qword(b,uint64_t(1800*16)|(uint64_t(1936*16)<<32),0x80000000abcdef00ull);
  auto original=b;auto* p=f.apply(2,b.data(),b.size(),16.f/9);
  check(xy(p+32)==1862*16&&xy(p+36)==1936*16,"PACKED XYZF coordinate layout correct");
  for(size_t i=0;i<b.size();++i)if(!(i>=32&&i<34))check(p[i]==original[i],"PACKED UV,Z,fog,ADC untouched");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);
  // REGLIST odd register count: trailing half-qword is padding.
  Bytes b;qword(b,1ull|(1ull<<46)|(uint64_t(0x156)<<47)|(1ull<<58)|(3ull<<60),0x535);
  qword(b,xyz(1792,1936),0x12345678);qword(b,xyz(2304,2160),0xdeadbeefcafebabeull);
  auto* p=f.apply(2,b.data(),b.size(),16.f/9);
  check(xy(p+16)==1856*16&&xy(p+32)==2240*16,"REGLIST vertex order/odd padding correct");
  uint64_t padding;std::memcpy(&padding,p+40,8);check(padding==0xdeadbeefcafebabeull,"REGLIST padding unmodified");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);
  auto a=f.apply(2,sprite.data(),32,16.f/9);check(std::memcmp(a,sprite.data(),32)==0,"fragmented tag header and UV unchanged");
  auto p=f.apply(2,sprite.data()+32,sprite.size()-32,16.f/9);check(xy(p)==1856*16&&xy(p+16)==2240*16,"textured HUD tag continues across submits");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);auto world=ad({{1ull<<16|2ull<<17,0x47},{xyz(1792,1936,0x123456),5},{xyz(2304,2160,0x123456),5}});
  check(f.apply(2,world.data(),world.size(),16.f/9)==world.data(),"depth-tested world billboards unchanged");
  auto offscreen=ad({{2ull<<16,0x4c},{0,0x47}});f.apply(2,offscreen.data(),offscreen.size(),16.f/9);
  check(f.apply(2,sprite.data(),sprite.size(),16.f/9)==sprite.data(),"offscreen128px render targets unchanged");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);auto feedback=ad({{9ull<<26,6}});f.apply(2,feedback.data(),feedback.size(),16.f/9);
  check(f.apply(2,sprite.data(),sprite.size(),16.f/9)==sprite.data(),"framebuffer feedback samples stay full width");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);auto untextured=ad({{0x46,0},{xyz(1792,1936),5},{xyz(2304,2160),5}});
  check(f.apply(2,untextured.data(),untextured.size(),16.f/9)==untextured.data(),"full-screen fade coverage retained");
  auto panel=ad({{0x46,0},{xyz(1800,2090),5},{xyz(2290,2140),5}});auto* p=f.apply(2,panel.data(),panel.size(),16.f/9);check(xy(p+32)==1862*16,"partial untextured HUD panel centered");
  f.reset();f.apply(3,setup.data(),setup.size(),16.f/9);check(f.apply(2,untextured.data(),48,16.f/9)==untextured.data(),"partial untextured tag conservatively unchanged");
  check(f.apply(2,untextured.data()+48,16,16.f/9)==untextured.data()+48,"continuation of untextured cover unchanged");
 }
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);Bytes image;qword(image,2ull|(2ull<<58));qword(image,0x5555555555555555ull,0xeeeeeeeeeeeeeeeeull);qword(image,~0ull,~0ull);
  check(f.apply(2,image.data(),image.size(),16.f/9)==image.data(),"IMAGE pixels never parsed as drawing commands");
  auto malformed=sprite;malformed.pop_back();check(f.apply(2,malformed.data(),malformed.size(),16.f/9)==malformed.data(),"non-qword input not transformed");
 }
 // Original USA frCreateDrawEnvPacket + frCreateDrawFontPacket: enabled
 // GEQUAL depth test, PSMT4 atlas, zero-depth UV/RGBAQ/XYZF2 REGLIST glyphs.
 // These were missed even though ordinary panels were corrected.
 for(uint64_t test:{0x517edull,0x5380bull}) for(float ratio:{Original,16.f/9,1.6f,21.f/9,32.f/9,9.f/16}) {
  PacketFilter f;auto fontState=ad({{8ull<<16,0x4c},{1ull<<32,0x4e},{test,0x47},
      {0x3000ull|(20ull<<20)|(9ull<<26)|(8ull<<30),6},{223ull<<48,0x40},{1,0x1a},{0x15e,0}});
  f.apply(3,fontState.data(),fontState.size(),ratio);
  Bytes glyph;qword(glyph,0x3400000000008002ull,0x413);
  qword(glyph,0x00100010,0x3f800000ffffffffull);
  qword(glyph,xyz(1800,1950),0x00f000f0);
  qword(glyph,0x3f800000ffffffffull,xyz(1816,1966));
  const auto original=glyph;const auto* p=f.apply(2,glyph.data(),glyph.size(),ratio);
  const auto scale=hudScale(ratio);
  auto map=[](int n,float s){return uint16_t(std::lround(32768+(n*16-32768)*s));};
  check(xy(p+32)==map(1800,scale.x)&&xy(p+34)==map(1950,scale.y),"original zero-depth font top-left keeps shape");
  check(xy(p+56)==map(1816,scale.x)&&xy(p+58)==map(1966,scale.y),"original zero-depth font bottom-right keeps shape");
  check(glyph==original,"original font input remains immutable");
  for(size_t i=0;i<glyph.size();++i)if(!((i>=32&&i<36)||(i>=56&&i<60)))
   check(p[i]==original[i],"font UV,color,Q,depth,GIF headers remain exact");
  PacketFilter fragments;fragments.apply(3,fontState.data(),fontState.size(),ratio);
  p=fragments.apply(2,glyph.data(),32,ratio);
  check(std::memcmp(p,glyph.data(),32)==0,"font tag/UV/color prefix stays exact across submits");
  p=fragments.apply(2,glyph.data()+32,glyph.size()-32,ratio);
  check(xy(p)==map(1800,scale.x)&&xy(p+24)==map(1816,scale.x),"split original font glyph coordinates keep proportions");
  // A non-font textured effect with the same depth test remains untouched.
  auto effectState=ad({{0x3000ull|(0ull<<20)|(9ull<<26),6}});
  f.apply(3,effectState.data(),effectState.size(),ratio);
  check(f.apply(2,glyph.data(),glyph.size(),ratio)==glyph.data(),"32-bit screen-space textures are not treated as fonts");
 }
 // Native lighting tiles captured in run-20261024-034903.gs: PRIM 0x4D,
 // TEST 0x33801, Z 0x0FFFFF, 64x32 tiles covering the 512x224 viewport.
 // They must stay full-frame independently of tag/submission boundaries.
 for (float ratio : {16.f/9, 21.f/9, 32.f/9, 9.f/16}) {
  PacketFilter f;auto state=ad({{8ull<<16,0x4c},{1ull<<32,0x4e},{0x33801,0x47},{223ull<<48,0x40},{0x4d,0}});
  f.apply(3,state.data(),state.size(),ratio);
  for(unsigned y=1936;y<2160;y+=32)for(unsigned x=1792;x<2304;x+=64){
   auto tile=ad({{0x80808080,1},{xyz(x,y,0xfffff),5},{xyz(x+64,y,0xfffff),5},{xyz(x,y+32,0xfffff),5},{xyz(x+64,y+32,0xfffff),5}});
   auto* p=f.apply(2,tile.data(),tile.size(),ratio);
   check(std::memcmp(p,tile.data(),tile.size())==0,"world lighting tile retains full-window coverage");
   PacketFilter fragments;fragments.apply(3,state.data(),state.size(),ratio);
   p=fragments.apply(2,tile.data(),48,ratio);check(std::memcmp(p,tile.data(),48)==0,"fragmented light tile first chunk unchanged");
   p=fragments.apply(2,tile.data()+48,tile.size()-48,ratio);check(std::memcmp(p,tile.data()+48,tile.size()-48)==0,"fragmented light tile remainder unchanged");
  }
  auto panel=ad({{0x0,0x47},{xyz(1800,2060,0xfffff0),5},{xyz(1900,2060,0xfffff0),5},{xyz(1800,2100,0xfffff0),5},{xyz(1900,2100,0xfffff0),5}});
  check(f.apply(2,panel.data(),panel.size(),ratio)!=panel.data(),"far-depth HUD panel still keeps original proportions");
 }
 // A field-menu party row (run-20261117-202057 draw 125384): a 1:1 body sprite, then
 // one texel column stretched to the right edge. The edge vertex stays on the
 // display edge, everything else keeps the HUD's proportions.
 {
  PacketFilter f;f.apply(3,setup.data(),setup.size(),16.f/9);auto scale=hudScale(16.f/9);
  auto map=[](int x,float s){return uint16_t(std::lround(32768+(x*16-32768)*s));};
  auto uv=[](unsigned u){return std::pair<uint64_t,uint64_t>{uint64_t(u*16+8),3};};
  auto row=ad({uv(1),{xyz(1999,1960),5},uv(159),{xyz(2157,1971),5},uv(158),{xyz(2157,1960),5},uv(159),{xyz(2304,1971),5}});
  auto* p=f.apply(2,row.data(),row.size(),16.f/9);
  check(xy(p+32)==map(1999,scale.x)&&xy(p+64)==map(2157,scale.x)&&xy(p+96)==map(2157,scale.x),"row body keeps HUD proportions");
  check(xy(p+128)==2304*16,"row edge column reaches the display edge");
  // The Money box (draw 125373): one 1:1 sprite from the left edge, its label in the
  // texture. Widening it would stretch the label, so it keeps the HUD's proportions.
  auto money=ad({uv(1),{xyz(1792,2138),5},uv(194),{xyz(1985,2150),5}});
  p=f.apply(2,money.data(),money.size(),16.f/9);
  check(xy(p+32)==map(1792,scale.x)&&xy(p+64)==map(1985,scale.x),"textured edge detail is not stretched");
 }
 // The load screen's red Yes/No highlight (run-20261117-192816 draw 7616): the
 // lighting state, a fan at x 16..112, y 148..159. It follows the HUD text; a
 // menu band from edge to edge (draw 7606) keeps both ends on the edges.
 for (float ratio : {16.f/9, 21.f/9}) {
  PacketFilter f;auto state=ad({{8ull<<16,0x4c},{1ull<<32,0x4e},{0x33801,0x47},{223ull<<48,0x40},{0x4d,0}});
  f.apply(3,state.data(),state.size(),ratio);auto scale=hudScale(ratio);
  auto map=[](int x,float s){return uint16_t(std::lround(32768+(x*16-32768)*s));};
  auto fan=ad({{0x92070140,1},{xyz(1808,2084,0xfffff),5},{xyz(1856,2084,0xfffff),5},{xyz(1856,2095,0xfffff),5},{xyz(1904,2095,0xfffff),5}});
  auto* p=f.apply(2,fan.data(),fan.size(),ratio);
  check(xy(p+32)==map(1808,scale.x)&&xy(p+48)==map(1856,scale.x)&&xy(p+80)==map(1904,scale.x),"menu highlight follows the HUD text");
  auto band=ad({{0x40302b,1},{xyz(1792,1983,0xfffff),5},{xyz(1839,1983,0xfffff),5},{xyz(2304,1983,0xfffff),5},{xyz(1792,2140,0xfffff),5}});
  p=f.apply(2,band.data(),band.size(),ratio);
  check(xy(p+32)==1792*16&&xy(p+64)==2304*16&&xy(p+80)==1792*16,"menu band keeps both ends on the display edges");
  check(xy(p+48)==map(1839,scale.x),"menu band interior vertex keeps HUD proportions");
 }
 // Replay actual pre-existing traces offline, without a renderer/window. The
 {
  Filters f;int a,b;f.reset(&a);f.reset(&b);
  auto& one=f.get(&a);one.apply(3,setup.data(),setup.size(),16.f/9);
  check(f.get(&b).apply(2,sprite.data(),sprite.size(),16.f/9)==sprite.data(),"different backends never share GS state");
  check(f.get(&a).apply(2,sprite.data(),sprite.size(),16.f/9)!=sprite.data(),"thread cache switches back to correct owner");
  f.reset(&a);check(f.get(&a).apply(2,sprite.data(),sprite.size(),16.f/9)==sprite.data(),"reset invalidates cached filter and register state");
  f.remove(&a);bool missing=false;try{f.get(&a);}catch(const std::logic_error&){missing=true;}check(missing,"removed owner cannot use stale cached state");
  f.reset(&a);check(f.get(&a).apply(2,sprite.data(),sprite.size(),16.f/9)==sprite.data(),"owner address reuse starts clean");
 }
 // Replay actual pre-existing traces offline, without a renderer/window. The
 // original inputs remain read-only and every submit retains its exact size.
 for(int arg=1;arg<argc;++arg){
  std::ifstream in(argv[arg],std::ios::binary);std::array<uint8_t,192> h{};in.read((char*)h.data(),h.size());uint32_t vram;std::memcpy(&vram,h.data()+8,4);in.seekg(vram,std::ios::cur);PacketFilter f;unsigned packets=0,changed=0;
  std::array<uint8_t,16> eh{};
  while(in.read((char*)eh.data(),16)){
   uint32_t n;std::memcpy(&n,eh.data()+4,4);Bytes b(n);in.read((char*)b.data(),n);check(bool(in),"trace event has declared payload");if(eh[0]!=1)continue;
   auto before=b;const auto* p=f.apply(eh[1],b.data(),b.size(),16.f/9);++packets;
   check(before==b,"real trace input packet unchanged");
   if(std::memcmp(p,b.data(),b.size())!=0){++changed;check(eh[1]!=1,"real world PATH1 retained exactly");}
  }
  std::printf("TRACE %s packets=%u transformed=%u\n",argv[arg],packets,changed);
 }
 std::printf("PASS aspect packets: %u checks\n",checks);
}
