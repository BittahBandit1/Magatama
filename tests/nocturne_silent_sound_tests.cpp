#include "../src/product/nocturne_silent_sound.h"
#include <cstdio>
#include <cstdlib>
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
    std::puts("Nocturne silent sound bank tests passed");
}
