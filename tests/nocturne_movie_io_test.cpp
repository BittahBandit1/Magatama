#include "../src/product/nocturne_movie_io.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
    std::vector<uint8_t> ram(32*1024*1024);
    constexpr uint32_t gp=0x550000, stream=0x343e40, pac=0x600000, packet=0x610000;
    const auto put=[&](uint32_t p,uint32_t v){std::memcpy(ram.data()+p,&v,4);};
    put(gp-0x2008,stream);put(stream+0x1c,pac);put(stream+0x10,0x620000);put(pac+0x50,packet);
    ram[stream]=1;ram[stream+1]=4;ram[stream+3]=1;ram[stream+0x20]=1;
    const auto eligible=[&](uint32_t d,uint32_t n){return magatama::movie_io::isPacketRead(ram.data(),gp,d,n);};
    assert(eligible(packet,0x4000));assert(eligible(packet|0x20000000,0x4000));assert(eligible(packet,1));
    assert(!eligible(packet,0));assert(!eligible(packet,0x4001));assert(!eligible(packet+16,0x4000));
    for (auto [offset,bad]:{std::pair{0u,0u},{1u,0u},{1u,5u},{2u,1u},{3u,0u},{0x20u,0u}}) {
        auto old=ram[stream+offset];ram[stream+offset]=bad;assert(!eligible(packet,0x4000));ram[stream+offset]=old;
    }
    put(stream+0x10,0);assert(!eligible(packet,0x4000));put(stream+0x10,0x620000);
    put(pac+0x50,0);assert(!eligible(packet,0x4000));put(pac+0x50,packet);
    put(stream+0x1c,0x1fffff0);assert(!eligible(packet,0x4000));put(stream+0x1c,pac);
    assert(!magatama::movie_io::isPacketRead(nullptr,gp,packet,0x4000));
    assert(!magatama::movie_io::isPacketRead(ram.data(),0x1000,packet,0x4000));
    assert(!magatama::movie_io::isPacketRead(ram.data(),0x10000000,packet,0x4000));
    std::puts("IPS packet-read eligibility: alias/range/header/open/stopped/unrelated-buffer guards passed");
}
