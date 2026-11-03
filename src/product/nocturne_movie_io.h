#pragma once
#include <cstdint>
#include <cstring>

namespace magatama::movie_io {
// Locked USA sdfMovie stream layout. Only the persistent .IPS packet buffer
// is eligible; archive opens, headers, unrelated files and stopped streams
// retain the existing lifetime/publication delay.
inline bool isPacketRead(const uint8_t* ram, uint32_t gp, uint32_t destination, uint32_t bytes) {
    constexpr uint32_t limit = 0x02000000u;
    const auto physical = [](uint32_t p) { return p & 0x1fffffffu; };
    const auto range = [&](uint32_t p, uint32_t n) { return p != 0 && p < limit && n <= limit - p; };
    const auto word = [&](uint32_t p) { uint32_t v; std::memcpy(&v, ram + p, 4); return v; };
    if (!ram || bytes == 0 || bytes > 0x4000u) return false;
    gp = physical(gp);
    if (gp < 0x2008u || !range(gp - 0x2008u, 4)) return false;
    const uint32_t stream = physical(word(gp - 0x2008u));
    if (!range(stream, 0x24u) || ram[stream] != 1 || ram[stream+1] != 4 ||
        ram[stream+2] != 0 || ram[stream+3] != 1 || ram[stream+0x20] != 1 || !word(stream+0x10)) return false;
    const uint32_t pac = physical(word(stream + 0x1cu));
    if (!range(pac, 0x78u)) return false;
    const uint32_t packet = physical(word(pac + 0x50u));
    return range(packet, bytes) && physical(destination) == packet;
}
} // namespace magatama::movie_io
