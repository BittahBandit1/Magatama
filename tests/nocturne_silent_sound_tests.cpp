#include "../src/product/nocturne_silent_sound.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    NocturneSilentSoundBanks banks;
    check(!banks.ready(0, 0), "bank zero must not be resident before a request");
    banks.request(0x3a, 60);
    check(!banks.ready(0x3a, 60), "completion must not occur in the enqueue frame");
    check(banks.ready(0x3a, 61), "completion occurs on the next guest frame");
    check(!banks.ready(0x3b, 61), "a request must not complete a different bank");
    banks.request(0x3a, 62);
    check(!banks.ready(0x3a, 62), "a repeated request has its own completion time");
    check(banks.ready(0x3a, 63), "repeated request completes");
    for (uint16_t i = 1; i <= 7; ++i) banks.request(i, 70);
    check(!banks.ready(0x3a, 71), "seven-slot residency is bounded");
    for (uint16_t i = 1; i <= 7; ++i) check(banks.ready(i, 71), "new resident bank retained");
    std::vector<uint8_t> ram(32u*1024u*1024u);
    constexpr uint32_t payload=0x552120,buffer=0x1367600;
    const auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram.data()+a,&v,4);};
    put(payload,buffer);put(0x593f20,0x2c80);put(0x593f24,1);put(0x593f28,buffer);
    put(buffer,10);put(buffer+4,0x2be0);put(buffer+8,0x4944494d);
    put(buffer+12,0x2bb0);put(buffer+0x28,0x18910000);
    const auto parse=[&]{return nocturneSilentMemoryMidiBank(ram.data(),ram.size(),payload,4);};
    check(parse()==0x1891,"captured in-memory MIDI bank decoded exactly");
    check(nocturneSilentMemoryMidiBank(ram.data(),ram.size(),payload|0xa0000000u,4)==0x1891,"valid uncached payload alias accepted");
    put(payload,buffer|0x80000000u);
    check(parse()==0x1891,"valid cached buffer alias accepted");put(payload,buffer);
    check(!nocturneSilentMemoryMidiBank(nullptr,ram.size(),payload,4),"null RAM rejected");
    check(!nocturneSilentMemoryMidiBank(ram.data(),ram.size(),payload,3),"wrong command payload size rejected");
    check(!nocturneSilentMemoryMidiBank(ram.data(),ram.size(),0x1ffffff,4),"straddling payload rejected");
    put(0x593f24,0);check(!parse(),"no pending memory request rejected");put(0x593f24,1);
    put(0x593f28,buffer+16);check(!parse(),"unrelated buffer rejected");put(0x593f28,buffer);
    put(buffer,9);check(!parse(),"non-MIDI PCM type rejected");put(buffer,10);
    put(buffer+8,0);check(!parse(),"missing MIDI magic rejected");put(buffer+8,0x4944494d);
    put(0x593f20,0x20);check(!parse(),"truncated header rejected");put(0x593f20,0x2c80);
    put(buffer+4,0x2d00);check(!parse(),"container larger than request rejected");put(buffer+4,0x2be0);
    put(buffer+12,0xffffffff);check(!parse(),"invalid/overflowing MIDI chunk rejected");put(buffer+12,0x2bb0);
    put(payload,0x70000000);put(0x593f28,0x70000000);check(!parse(),"scratchpad pointer rejected");
    put(payload,0x1ffffe0);put(0x593f28,0x1ffffe0);check(!parse(),"RAM-end overflow rejected");
    put(payload,buffer);put(0x593f28,buffer);put(0x593f20,0xffffffff);check(!parse(),"negative signed length rejected");
    std::puts("Nocturne silent sound bank tests passed: original14 and16 memory-bank checks");
}
