#pragma once
// Nocturne-only, opt-in presentation companion. No guest instructions, GIF
// control/transfer registers, texture coordinates or depth values are removed.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace magatama::aspect {
constexpr float Original = 4.f / 3.f;
inline std::atomic<float> target{0.f}; // zero: completely inactive
inline std::atomic<bool> followWindow{false};
inline std::atomic<uint64_t> drawable{0};
inline std::mutex selectionMutex;
inline const char* configure(const char* value) {
    const std::lock_guard<std::mutex> lock(selectionMutex);
    float ratio = 0.f; const char* fit = nullptr; followWindow = false;
    const std::string_view s = value ? value : "";
    if (s.empty() || s == "off") { target.store(0); return nullptr; }
    if (s == "auto") { ratio = Original; followWindow = true; fit = "stretch"; }
    else if (s == "4:3") { ratio = Original; fit = "4:3"; }
    else if (s == "16:9") { ratio = 16.f/9.f; fit = "16:9"; }
    else if (s == "16:10") { ratio = 1.6f; fit = "16:10"; }
    else if (s == "21:9") { ratio = 21.f/9.f; fit = "21:9"; }
    else if (s == "32:9") { ratio = 32.f/9.f; fit = "32:9"; }
    else throw std::invalid_argument("MAGATAMA_ASPECT must be off,4:3,16:9,16:10,21:9,32:9 or auto");
    if (followWindow.load(std::memory_order_relaxed)) {
        const auto wh=drawable.load(std::memory_order_acquire);
        if (uint32_t(wh>>32) && uint32_t(wh)) ratio=float(uint32_t(wh>>32))/float(uint32_t(wh));
    }
    target.store(ratio, std::memory_order_relaxed); return fit;
}
inline void surface(uint32_t w, uint32_t h) {
    const std::lock_guard<std::mutex> lock(selectionMutex);
    if (w && h) drawable.store((uint64_t(w)<<32)|h,std::memory_order_release);
    if (followWindow.load(std::memory_order_relaxed) && w && h) target.store(float(w)/float(h), std::memory_order_relaxed);
}
struct Scale { float x, y; };
inline Scale hudScale(float ratio) {
    // Fit the entire original UI plane, centered as a group. Portrait windows
    // use vertical compression before presentation, rather than clipping UI.
    return ratio >= Original ? Scale{Original/ratio, 1.f} : Scale{1.f, ratio/Original};
}
inline float projectionAspect(float original, float ratio) { return original * (ratio/Original); }

class PacketFilter {
    static constexpr uint16_t kLeft=(2048-256)*16, kRight=(2048+256)*16; // the 512-wide picture (FBW 8)
    struct Path { uint32_t left=0, flag=0, nr=0, ri=0; uint64_t regs=0, tag=0; };
    struct Context { uint64_t frame=0, tex=0, test=0, zbuf=0; uint32_t height=224; bool frameKnown=false; };
    struct Vertex { size_t at; bool packed, untextured, deep, sprite; uint64_t tag; uint32_t path, height; uint16_t x,y,u; };
    std::array<Path,4> paths{};
    std::array<Context,2> contexts{};
    uint32_t prim=0, prmode=0; bool ac=true; uint16_t u=0;
    uint64_t nextTag=0, blit=0;
    std::array<uint32_t,8> feedback{}; unsigned feedbackCount=0, feedbackNext=0;
    std::vector<Vertex> vertices;
    std::vector<uint8_t> scratch, column;
    static uint64_t read64(const uint8_t* p) { uint64_t n; std::memcpy(&n,p,8); return n; }
    static uint32_t read32(const uint8_t* p) { uint32_t n; std::memcpy(&n,p,4); return n; }
    bool isFeedback(uint32_t base) const {
        for(unsigned i=0;i<feedbackCount;++i) if(base>=feedback[i] && base-feedback[i]<0xe00u) return true;
        return false;
    }
    void remember(uint32_t base) {
        for(unsigned i=0;i<feedbackCount;++i) if(feedback[i]==base) return;
        feedback[feedbackNext]=base; feedbackNext=(feedbackNext+1)%feedback.size();
        feedbackCount=std::min<unsigned>(feedbackCount+1,feedback.size());
    }
    void reg(uint32_t addr, const uint8_t* p, bool packed, size_t at, uint32_t path) {
        const uint64_t v=read64(p);
        if(addr==0) { prim=v&0x7ff; return; }
        if(addr==3) { u=uint16_t(v&0x3fff); return; }
        if(addr==0x1a) { ac=(v&1)!=0; return; }
        if(addr==0x1b) { prmode=v&0x7f8; return; }
        if(addr==0x4c || addr==0x4d) {
            auto& c=contexts[addr-0x4c]; c.frame=v; c.frameKnown=true;
            if(((v>>16)&63)==8) remember(uint32_t(v&511)*32); return;
        }
        if(addr==6 || addr==7) { contexts[addr-6].tex=v; return; }
        if(addr==0x47 || addr==0x48) { contexts[addr-0x47].test=v; return; }
        if(addr==0x4e || addr==0x4f) { contexts[addr-0x4e].zbuf=v; return; }
        if(addr==0x40 || addr==0x41) {
            auto& c=contexts[addr-0x40]; c.height=uint32_t((v>>48)&2047)+1; return;
        }
        if(addr==0x50) { blit=v; return; }
        if(addr==0x53 && (v&3)==2 && isFeedback(uint32_t(blit&16383))) {
            remember(uint32_t((blit>>32)&16383)); return;
        }
        if(path==1 || !(addr==4 || addr==5 || addr==12 || addr==13)) return;
        const uint32_t effective=ac?prim:(prim&7)|prmode;
        const auto& c=contexts[(effective>>9)&1];
        if(!c.frameKnown || ((c.frame>>16)&63)!=8) return; // offscreen maps stay native
        uint32_t z=packed?read32(p+8):uint32_t(v>>32);
        if(addr==4 || addr==12) z=packed?(z>>4)&0xffffff:z&0xffffff;
        const bool tme=(effective&16)!=0, fst=(effective&256)!=0;
        const bool overlay=(c.test&(1ull<<16))==0 || ((c.test>>17)&3)==1;
        // Original UV/RGBAQ/XYZF2 PSMT4 glyph sprites use enabled GEQUAL
        // depth testing, including Z=0. The locked USA default is 0x517ed;
        // consumed load-menu packets use the later alpha-test state 0x5380b.
        // Match both observed font states and the original glyph register list.
        const uint32_t fontTest=uint32_t(c.test&0x7ffffu);
        const bool font=tme && fst && (effective&7)==6 &&
            (fontTest==0x517edu || fontTest==0x5380bu) &&
            ((c.tex>>20)&63)==20 && paths[path].flag==1 &&
            paths[path].nr==3 && paths[path].regs==0x413;
        // The field main menu (Square) layers its panels and glyphs with real
        // depth: TEST 0x5101B (alpha GEQUAL ref 1, FB_ONLY, Z GEQUAL) at Z=0
        // and mid-range Z. Its highlight bar is an untextured FST strip at
        // TEST 0x30000/0x7101B. World lighting strips never set FST, and no
        // recorded field/battle trace draws direct mid-Z FST sprites otherwise.
        const bool menuSprite=tme && fst && (effective&7)==6 && fontTest==0x5101bu;
        const bool menuBar=!tme && (effective&256) && (effective&7)==4 &&
            (fontTest==0x30000u || fontTest==0x7101bu) && z>0 && z<=0xff0000u;
        // Main 3D projection is bounded by FF0000 (locked USA screen record).
        // Recorded direct UI uses UV/FST at Z=0 as well as far-depth overlays.
        if(!(z>0xff0000u || font || menuSprite || menuBar || (overlay && (!tme || fst)))) return;
        if(tme && ((c.tex>>26)&15)>=9 && isFeedback(uint32_t(c.tex&16383))) return;
        // Native world lighting uses tiled, untextured strips at 0x0FFFFF
        // (PRIM=0x4D, TEST=0x33801), one GIF tag per 64x32 tile, so per-tag
        // full-screen bounds cannot recognize the complete effect. The menus
        // draw their panels and the red Yes/No highlight with the very same
        // state (load screen: a 96x11 fan at x 16..112). These world-depth
        // vertices are marked `deep`; apply() keeps a tag native when every
        // deep vertex sits on the 64-pixel tile grid and fits the rest to
        // the HUD plane like any other UI.
        const bool deep = !tme && z > 0 && z <= 0xff0000u && !menuBar;
        // Z-writing opaque untextured primitives are framebuffer clears.
        if(!tme && !(effective&64) && !(c.zbuf&(1ull<<32))) return;
        uint16_t x,y; std::memcpy(&x,p,2); std::memcpy(&y,p+(packed?4:2),2);
        // Native display coordinates around (2048,2048); reject overflow and
        // offscreen packet data rather than treating it as an overlay.
        if(x<((2048-1024)<<4) || x>((2048+1024)<<4) ||
           y<((2048-1024)<<4) || y>((2048+1024)<<4)) return;
        vertices.push_back({at,packed,!tme,deep,(effective&7)==6,paths[path].tag,path,c.height,x,y,u});
    }
public:
    void reset() { paths={};contexts={};prim=prmode=0;ac=true;u=0;nextTag=blit=0;feedbackCount=feedbackNext=0;vertices.clear();scratch.clear(); }
    const uint8_t* apply(uint32_t path, const uint8_t* bytes, uint32_t size, float ratio) {
        if(!(ratio>0) || !std::isfinite(ratio) || path<1 || path>3 || !bytes || (size&15)) return bytes;
        auto& st=paths[path]; const uint64_t prior=st.left?st.tag:0;
        vertices.clear();
        for(size_t q=0;q<size;) {
            const uint8_t* p=bytes+q;
            if(!st.left) {
                uint64_t lo=read64(p);st.regs=read64(p+8);st.flag=(lo>>58)&3;st.nr=(lo>>60)&15;
                if(!st.nr)st.nr=16;st.ri=0;st.left=uint32_t(lo&32767)*(st.flag<2?st.nr:1);
                st.tag=++nextTag;if((lo>>46)&1)prim=(lo>>47)&2047;q+=16;continue;
            }
            if(st.flag>=2) {uint32_t n=std::min<uint32_t>(st.left,(size-q)/16);q+=16*n;st.left-=n;continue;}
            if(path==1 && st.flag==0) {
                bool simple=true;for(unsigned i=0;i<st.nr;++i){unsigned a=(st.regs>>(4*i))&15;if(a==0||a==6||a==7||a==14)simple=false;}
                if(simple){uint32_t n=std::min<uint32_t>(st.left,(size-q)/16);q+=16*n;st.ri=(st.ri+n)%st.nr;st.left-=n;continue;}
            }
            const unsigned halves=st.flag==0?1:std::min<unsigned>(2,st.left);
            for(unsigned h=0;h<halves;++h) {
                unsigned a=(st.regs>>(4*st.ri))&15;bool packed=st.flag==0;size_t at=q+(packed?0:h*8);
                if(packed && a==14){a=read64(p+8)&255;packed=false;}
                reg(a,bytes+at,packed,at,path);--st.left;st.ri=(st.ri+1)%st.nr;
            }
            q+=16;
        }
        const auto scale=hudScale(ratio); if(scale.x==1.f && scale.y==1.f)return bytes;
        bool copied=false;
        for(size_t first=0;first<vertices.size();) {
            size_t end=first;const auto& head=vertices[first];
            uint16_t x0=65535,x1=0,y0=65535,y1=0;bool untextured=false,grid=true;
            size_t open=SIZE_MAX;column.assign(0,0);
            while(end<vertices.size()&&vertices[end].tag==head.tag){
                const size_t i=end;const auto& v=vertices[end++];column.push_back(0);
                if(v.deep && (v.x<kLeft || (v.x-kLeft)%(64*16)!=0)) grid=false;
                if(!v.untextured){
                    // Sprite corners pair up in order; a sprite at least 4 pixels wide
                    // over at most 2 texels is one stretched texel column (one that
                    // spans the whole width keeps the HUD's proportions).
                    if(!v.sprite){open=SIZE_MAX;continue;}
                    if(open==SIZE_MAX){open=i;continue;}
                    const auto& a=vertices[open];
                    const bool both=std::min(v.x,a.x)<=kLeft+16 && std::max(v.x,a.x)>=kRight-16;
                    if(!both && std::abs(int(v.u)-int(a.u))<=2*16 && std::abs(int(v.x)-int(a.x))>=4*16)
                        column[open-first]=column[i-first]=1;
                    open=SIZE_MAX;continue;
                }
                untextured=true;x0=std::min(x0,v.x);x1=std::max(x1,v.x);y0=std::min(y0,v.y);y1=std::max(y1,v.y);
            }
            // One linear scan per tag; no quadratic work for large UI fans.
            const bool cover=untextured && (head.height==224||head.height==448) &&
                x1-x0>=510*16 && y1-y0>=int(head.height-2)*16;
            const bool partial=head.tag==prior || (st.left&&head.tag==st.tag);
            for(size_t i=first;i<end;++i){const auto& v=vertices[i];
                if(v.untextured&&(cover||partial))continue;
                if(v.deep&&(grid||partial))continue;
                auto mapped=[](uint16_t n,float f){return uint16_t(std::clamp<long>(std::lround(32768.f+(float(n)-32768.f)*f),0,65535));};
                // Bars and panels that touch a side of the 512-wide picture keep
                // that side on the edge of the display, so menu bands, row
                // backgrounds and highlights run edge to edge; their other end
                // keeps the HUD's proportions. Untextured ones always; textured
                // ones only where the game draws the edge piece as one stretched
                // texel column (the party rows: U 233.5..234.5 over 175 pixels),
                // so widening it shows nothing new. A textured piece with real
                // detail at the edge (the Money box, its label in the texture)
                // would be stretched, so it keeps the HUD's proportions.
                const bool atEdge=v.x<=kLeft+16 || v.x>=kRight-16;
                const bool edge=atEdge && (v.untextured || (column[i-first] && !partial));
                uint16_t x=edge?v.x:mapped(v.x,scale.x),y=mapped(v.y,scale.y);
                if(x==v.x && y==v.y)continue;
                if(!copied){scratch.assign(bytes,bytes+size);copied=true;}
                std::memcpy(scratch.data()+v.at,&x,2);std::memcpy(scratch.data()+v.at+(v.packed?4:2),&y,2);
            }
            first=end;
        }
        return copied?scratch.data():bytes;
    }
};
// Sidecar ownership deliberately leaves Backend/PS2Runtime layouts unchanged.
// The caller holds its existing backend lock throughout get/apply/remove.
// Only cache misses take the registry mutex; the packet hot path checks one
// generation counter. A reset/address reuse invalidates every thread's cache.
class Filters {
    std::mutex mutex;
    std::unordered_map<const void*,std::unique_ptr<PacketFilter>> entries;
    std::atomic<uint64_t> generation{1};
public:
    void reset(const void* owner) {
        std::lock_guard<std::mutex> lock(mutex);
        entries[owner]=std::make_unique<PacketFilter>();generation.fetch_add(1,std::memory_order_release);
    }
    void remove(const void* owner) {
        std::lock_guard<std::mutex> lock(mutex);
        if(entries.erase(owner))generation.fetch_add(1,std::memory_order_release);
    }
    PacketFilter& get(const void* owner) {
        struct Cache { const Filters* registry=nullptr;const void* owner=nullptr;uint64_t generation=0;PacketFilter* filter=nullptr; };
        static thread_local Cache cache;
        const auto current=generation.load(std::memory_order_acquire);
        if(cache.registry==this&&cache.owner==owner&&cache.generation==current)return *cache.filter;
        std::lock_guard<std::mutex> lock(mutex);
        auto i=entries.find(owner);if(i==entries.end())throw std::logic_error("Nocturne packet filter used without initialization");
        cache={this,owner,generation.load(std::memory_order_relaxed),i->second.get()};return *cache.filter;
    }
};
inline Filters& filters(){static Filters state;return state;}
} // namespace magatama::aspect
