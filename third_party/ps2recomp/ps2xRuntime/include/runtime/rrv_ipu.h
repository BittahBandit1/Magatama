// SPDX-License-Identifier: GPL-3.0+
// Portable integration of the pinned PCSX2 IPU decoder; see ipu/README.md.
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#pragma push_macro("__fi")
#pragma push_macro("__ri")
#pragma push_macro("__noinline")
#undef __fi
#undef __ri
#undef __noinline
#define __fi inline
#define __ri inline
#define __noinline
#define pxAssert(x) assert(x)
#define pxAssertMsg(x, msg) assert(x)
#define pxAssume(x) assert(x)
#define IPU_LOG(...) ((void)0)
#define MULTI_ISA_COMPILE_ONCE 1
#define MULTI_ISA_UNSHARED_START
#define MULTI_ISA_UNSHARED_END
#define IPU_INT_PROCESS(cycles) ((void)0)
#define jNO_DEFAULT default: assert(false); break;

namespace rrv::ipu {
using u8=uint8_t; using s8=int8_t; using u16=uint16_t; using s16=int16_t;
using u32=uint32_t; using s32=int32_t; using u64=uint64_t; using uint=unsigned;
using uptr=uintptr_t;
union alignas(16) u128 { u8 _u8[16]; u32 _u32[4]; u64 _u64[2]; };
using r128=u128;
inline r128 r128_zero() { return {}; }
inline r128 r128_load(const void* p) { r128 v; std::memcpy(&v,p,16); return v; }
inline void r128_store(void* p,r128 v) { std::memcpy(p,&v,16); }
inline r128 r128_from_u32_dup(u32 x) { r128 v{}; for(auto& w:v._u32) w=x; return v; }
template<class T> T unaligned(const void* p) { T v; std::memcpy(&v,p,sizeof(v)); return v; }
inline void CopyQWC(void* a, const void* b) { std::memcpy(a,b,16); }
inline constexpr u32 IPU_CMD=0x10002000, IPU_CTRL=0x10002010, IPU_BP=0x10002020, IPU_TOP=0x10002030;
inline constexpr unsigned INTC_IPU=8;
inline bool irqPending=false;
inline void hwIntcIrq(unsigned) { irqPending=true; }
struct QuietConsole { template<class... T> void WriteLn(T...) {} template<class... T> void Warning(T...) {} };
inline QuietConsole Console, DevCon;
struct Channel { struct { u32 STR=0; u32 _u32=0; } chcr; u32 qwc=0; };
inline Channel ipu0ch, ipu1ch;
#include "ipu/mpeg2_vlc.inc"
#include "ipu/types.inc"
alignas(16) inline IPU_Fifo ipu_fifo;
inline u32 register32(u32 offset) {
    u32 v=0; std::memcpy(&v,reinterpret_cast<const u8*>(&ipuRegs)+(offset&0x3c),4); return v;
}
inline u64 register64(u32 offset) {
    return u64(register32(offset)) | (u64(register32(offset+4))<<32);
}
void yuv2rgb();
void IPUWorker();
#include "ipu/color.inc"
#include "ipu/dither.inc"
#include "ipu/decoder.inc"
#include "ipu/commands.inc"

// Only one EE owns this hardware, just as the existing GS/VU integration does.
// Service never calls guest handlers while the decoder is on the stack.
inline std::unordered_map<u32,u32>* bus=nullptr;
inline u8* ram=nullptr;
inline constexpr u32 ramMask=0x01ffffff;
inline std::vector<unsigned> completed;
inline uint64_t inputBytes=0, outputBytes=0;
inline void finishDma(u32 base, unsigned cause) {
    (*bus)[base]&=~0x100u;
    completed.push_back(cause);
}
inline void refill() {
    if (!bus || !ram || !((*bus)[0x1000b400]&0x100)) return;
    auto& qwc=(*bus)[0x1000b420];
    auto& madr=(*bus)[0x1000b410];
    while(qwc && g_BP.IFC<8) {
        alignas(16) u32 words[4];
        for(unsigned i=0;i<16;++i) reinterpret_cast<u8*>(words)[i]=ram[(madr+i)&ramMask];
        ipu_fifo.in.write(words,1);
        madr+=16; --qwc; inputBytes+=16;
    }
    if (!qwc) finishDma(0x1000b400,4);
}
inline void IPU_Fifo_Input::clear() {
    std::memset(data,0,sizeof(data)); readpos=writepos=0;
    g_BP.IFC=0; ipuRegs.ctrl.IFC=0;
}
inline int IPU_Fifo_Input::write(const u32* p, int size) {
    int n=std::min(size,8-int(g_BP.IFC));
    for(int i=0;i<n*4;++i) { data[writepos]=p[i]; writepos=(writepos+1)&31; }
    g_BP.IFC+=n; return n;
}
inline int IPU_Fifo_Input::read(void* p) {
    refill();
    if(!g_BP.IFC) return 0;
    std::memcpy(p,data+readpos,16); readpos=(readpos+4)&31; --g_BP.IFC;
    return 1;
}
inline void IPU_Fifo_Output::clear() {
    std::memset(data,0,sizeof(data)); readpos=writepos=0; ipuRegs.ctrl.OFC=0;
}
inline int IPU_Fifo_Output::write(const u32* p, uint size) {
    if(!bus || !ram || !((*bus)[0x1000b000]&0x100)) return 0;
    auto& qwc=(*bus)[0x1000b020]; auto& madr=(*bus)[0x1000b010];
    unsigned n=std::min(size,qwc);
    for(unsigned i=0;i<n*16;++i) ram[(madr+i)&ramMask]=reinterpret_cast<const u8*>(p)[i];
    madr+=n*16; qwc-=n; outputBytes+=n*16;
    ipu0ch.qwc=qwc;
    if(!qwc) { ipu0ch.chcr.STR=0; finishDma(0x1000b000,3); }
    return n;
}
inline void IPU_Fifo_Output::read(void*, uint) { assert(false && "IPU output uses DMA"); }
inline void IPU_Fifo::init() { in.clear(); out.clear(); }
inline void IPU_Fifo::clear() { in.clear(); out.clear(); }
inline void reset() {
    bus=nullptr; ram=nullptr; completed.clear(); irqPending=false;
    inputBytes=outputBytes=0; ipu0ch={}; ipu1ch={};
    ipuReset(); irqPending=false;
}
inline void service(std::unordered_map<u32,u32>& regs, u8* memory) {
    bus=&regs; ram=memory;
    refill();
    // The upstream worker yields between macroblocks. Run until it either
    // needs input or fills the destination DMA, with a bounded work budget.
    for(unsigned i=0;i<4096 && ipuRegs.ctrl.BUSY;++i) {
        ipu0ch.chcr.STR=(regs[0x1000b000]&0x100)!=0;
        ipu0ch.qwc=regs[0x1000b020];
        const auto beforeIn=inputBytes, beforeOut=outputBytes;
        const auto beforeBP=g_BP.BP; const auto beforeIFC=g_BP.IFC;
        IPUWorker(); refill();
        if(beforeIn==inputBytes && beforeOut==outputBytes && beforeBP==g_BP.BP && beforeIFC==g_BP.IFC) break;
    }
    if(irqPending) { regs[0x1000f000]|=1u<<8; irqPending=false; }
    for(unsigned cause:completed) regs[0x1000e010]|=1u<<cause;
    auto& stat=regs[0x1000e010];
    if((stat&0x3ff)&((stat>>16)&0x3ff)) stat|=1u<<31;
    else stat&=~(1u<<31);
}
inline u32 read(u32 address) {
    if((address&0xff)==4) return ipuRegs.cmd.BUSY;
    if((address&0xff)==0x34) return ipuRegs.topbusy;
    return ipuRead32(address);
}
inline void write(u32 address,u32 value) { ipuWrite32(address,value); }
}
#undef W1
#undef W2
#undef W3
#undef W5
#undef W6
#undef W7
#undef IPU_Y_BIAS
#undef IPU_C_BIAS
#undef IPU_Y_COEFF
#undef IPU_GCR_COEFF
#undef IPU_GCB_COEFF
#undef IPU_RCR_COEFF
#undef IPU_BCB_COEFF
#undef pxAssert
#undef pxAssertMsg
#undef pxAssume
#undef IPU_LOG
#undef MULTI_ISA_COMPILE_ONCE
#undef MULTI_ISA_UNSHARED_START
#undef MULTI_ISA_UNSHARED_END
#undef IPU_INT_PROCESS
#undef jNO_DEFAULT
#undef ipumsk
#undef ipucase
#undef BigEndian
#undef BigEndian64
#undef VLC_ALIGNED16
#undef INTRA
#undef QUANT
#undef MC
#undef CODED
#undef FWD
#undef BWD
#undef INTER
#pragma pop_macro("__fi")
#pragma pop_macro("__ri")
#pragma pop_macro("__noinline")
