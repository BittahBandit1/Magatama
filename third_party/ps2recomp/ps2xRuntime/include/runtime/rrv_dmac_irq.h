#pragma once
#include <cstdint>
namespace rrv::dmac_irq {
// Pinned PCSX2 R5900.cpp cpuIntsEnabled(0x800), Hw.cpp dmacInterrupt.
// DMAC is independent of INTC pending state and its 0x400 CPU mask.
inline bool eligible(uint32_t cpu,uint32_t stat,uint32_t control,uint32_t enable) {
    constexpr uint32_t required=1u|(1u<<16)|0x800u;
    return (cpu&required)==required && !(cpu&6u) &&
        (((stat&0xffffu)&(stat>>16)) || (stat&0x8000u)) &&
        (control&1u) && ((enable>>16)&0xffu)!=1u;
}
}
