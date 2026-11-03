// SPDX-License-Identifier: GPL-3.0+
#include "runtime/rrv_ipu.h"
#include <cstdio>
using namespace rrv::ipu;
int main() {
    std::vector<u8> memory(32*1024*1024);
    std::unordered_map<u32,u32> regs;
    std::vector<std::function<void()>> events;
    unsigned irq=0;
    auto bind=[&]{
        workerSchedule=[&](uint64_t cycles,std::function<void()> fn){assert(cycles==64);events.push_back(std::move(fn));};
        workerInterrupt=[&](unsigned){++irq;};
    };
    reset();bind();service(regs,memory.data());
    write(IPU_CMD,0x10010000);service(regs,memory.data());
    assert(workerPending && events.size()==1 && outputBytes==0 && (read(IPU_CTRL)&0x80000000));
    for(unsigned i=0;i<20;++i)service(regs,memory.data());
    assert(events.size()==1 && outputBytes==0); // register polling cannot run scheduled work early
    write(IPU_CTRL,1u<<30);events[0]();
    assert(!workerPending && !ipuRegs.ctrl.BUSY && irq==0 && outputBytes==0);
    events.clear();write(IPU_CMD,0x10010000);service(regs,memory.data());
    assert(events.size()==1);reset();events[0]();
    assert(!bus && !workerPending && !ipuRegs.ctrl.BUSY && irq==0);
    std::puts("PASS original64cycle IPU worker admission, no early execution/duplicate event, CTRL/reset cancellation");
}
