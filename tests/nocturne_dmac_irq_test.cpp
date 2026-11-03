#include "runtime/rrv_dmac_irq.h"
#include <cassert>
#include <cstdio>
int main() {
    unsigned checks=0;
    for(unsigned bits=0;bits<1024;++bits) {
        const bool ie=bits&1,eie=bits&2,im=bits&4,exl=bits&8,erl=bits&16;
        const bool pending=bits&32,mask=bits&64,dma=bits&128,suspend=bits&256,intc=bits&512;
        const uint32_t cpu=ie|(eie<<16)|(im<<11)|(exl<<1)|(erl<<2)|(intc<<10);
        const uint32_t stat=(pending<<4)|(mask<<20);
        assert(rrv::dmac_irq::eligible(cpu,stat,dma,suspend<<16)==(ie&&eie&&im&&!exl&&!erl&&pending&&mask&&dma&&!suspend));
        ++checks;
    }
    std::printf("PASS %u DMAC CPU/mask/pending/DMAE/suspend combinations; INTC mask independent\n",checks);
}
