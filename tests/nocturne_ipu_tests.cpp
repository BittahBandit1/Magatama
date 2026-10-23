// SPDX-License-Identifier: GPL-3.0+
#include "runtime/rrv_ipu.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
using namespace rrv::ipu;
int main(int argc,char** argv) {
    std::vector<u8> memory(32*1024*1024);
    std::unordered_map<u32,u32> regs;
    reset();
    auto tick=[&] { service(regs,memory.data()); completed.clear(); };
    // FDEC must peek actual DMA bytes, advance only FB bits, and preserve
    // the busy flag when its next word is unavailable.
    memory[0x100]=0x12; memory[0x101]=0x34; memory[0x102]=0x56; memory[0x103]=0x78; memory[0x104]=0x9a;
    regs[0x1000b400]=0x101; regs[0x1000b410]=0x100; regs[0x1000b420]=1;
    tick(); write(IPU_CMD,0x40000000); tick(); assert(read(IPU_CMD)==0x12345678);
    write(IPU_CMD,0x40000008); tick(); assert(read(IPU_CMD)==0x3456789a);
    assert(regs[0x1000b420]==0 && !(regs[0x1000b400]&0x100));
    reset(); regs.clear();
    write(IPU_CMD,0x40000000); tick(); assert(read(IPU_CTRL)&0x80000000);
    regs[0x1000b400]=0x101; regs[0x1000b410]=0x100; regs[0x1000b420]=1;
    tick(); assert(!(read(IPU_CTRL)&0x80000000)); assert(read(IPU_CMD)==0x12345678);
    // A black YCbCr macroblock converts to actual RGB output with PS2 alpha.
    reset(); regs.clear();
    std::fill(memory.begin()+0x100,memory.begin()+0x200,16);
    std::fill(memory.begin()+0x200,memory.begin()+0x280,128);
    regs[0x1000b400]=0x101; regs[0x1000b410]=0x100; regs[0x1000b420]=24;
    regs[0x1000b000]=0x100; regs[0x1000b010]=0x1000; regs[0x1000b020]=64;
    tick(); write(IPU_CMD,0x70000001); tick();
    assert(outputBytes==1024 && regs[0x1000b020]==0 && !(regs[0x1000b000]&0x100));
    for(unsigned i=0;i<256;++i) {
        const auto* p=memory.data()+0x1000+i*4;
        assert(p[0]==0 && p[1]==0 && p[2]==0 && p[3]==128);
    }
    assert(!(read(IPU_CTRL)&0x80000000));
    std::puts("IPU FIFO, FDEC starvation/resume, DMA completion and CSC tests passed");
    if(argc<2) return 0;
    // Optional local-asset smoke test. No game content is checked into tests.
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<u8> movie((std::istreambuf_iterator<char>(f)),{});
    assert(movie.size()>16+1024*1024 && std::memcmp(movie.data(),"ipum",4)==0);
    reset(); regs.clear();
    const u8 quant[64]={8,16,19,22,26,27,29,34,16,16,22,24,27,29,34,37,19,22,26,27,29,34,34,38,22,22,26,27,29,34,37,40,22,26,27,29,32,35,40,48,26,27,29,32,35,40,48,58,26,27,29,34,38,46,56,69,27,29,35,38,46,56,69,83};
    std::memcpy(decoder.iq,quant,64); std::memset(decoder.niq,16,64);
    std::copy(movie.begin()+16,movie.begin()+16+1024*1024,memory.begin()+0x100000);
    unsigned inputCursor=0x100000;
    auto streamTick=[&] {
        service(regs,memory.data()); completed.clear();
        if(!(regs[0x1000b400]&0x100)) {
            regs[0x1000b410]=inputCursor; regs[0x1000b420]=512;
            regs[0x1000b400]=0x101; inputCursor+=8192;
            service(regs,memory.data()); completed.clear();
        }
    };
    for(unsigned frame=0;frame<10;++frame) {
        if(frame) { write(IPU_CMD,0x40000020); streamTick(); }
        write(IPU_CMD,0x40000000); streamTick();
        const auto flags=read(IPU_CMD);
        write(IPU_CMD,0x40000008); streamTick();
        write(IPU_CTRL,(flags>>8)&0x00fb0000);
        regs[0x1000b000]=0x100; regs[0x1000b010]=0x400000; regs[0x1000b020]=57344;
        write(IPU_CMD,0x10010000|((flags&0x04000000)?0x01000000:0));
        for(unsigned i=0;i<65536 && ipuRegs.ctrl.BUSY;++i) streamTick();
        std::printf("movie frame %u: decoded=%llu bytes, input=%llu, ctrl=%08x top=%08x remaining=%u\n",frame,(unsigned long long)outputBytes,(unsigned long long)inputBytes,read(IPU_CTRL),read(IPU_TOP),regs[0x1000b020]);
        assert(outputBytes==(frame+1)*512*448*4);
        assert(!(read(IPU_CTRL)&0x80000000));
    }
}
