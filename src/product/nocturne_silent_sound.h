#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <optional>

// Compatibility state only: no sample data is decoded or played. Nocturne's
// EE status routine exposes seven resident-bank slots. A request becomes
// virtually ready on the next guest frame, never at the enqueue instant.
class NocturneSilentSoundBanks {
public:
    void request(uint16_t bank, uint64_t frame) {
        for (auto& slot : slots_) {
            if (slot.valid && slot.bank == bank) {
                slot.readyFrame = frame + 1;
                return;
            }
        }
        slots_[next_] = {bank, frame + 1, true};
        next_ = (next_ + 1) % slots_.size();
    }
    bool ready(uint16_t bank, uint64_t frame) const {
        for (const auto& slot : slots_)
            if (slot.valid && slot.bank == bank && frame >= slot.readyFrame) return true;
        return false;
    }
private:
    struct Slot { uint16_t bank = 0; uint64_t readyFrame = 0; bool valid = false; };
    std::array<Slot, 7> slots_{};
    unsigned next_ = 0;
};

// Memory-loaded SMG banks use command0x1B0, not the filename command0xA0.
// The captured USA bank has type10, a bounded MIDI chunk and the bank ID in
// the high half of its sequence word at0x28. Do not complete arbitrary PCM,
// malformed buffers, or a buffer unrelated to the accepted memory request.
inline std::optional<uint16_t> nocturneSilentMemoryMidiBank(
    const uint8_t* ram, size_t ramSize, uint32_t payload, uint32_t payloadBytes) {
    const auto physical=[](uint32_t p) {
        return p>=0x80000000u && p<0xc0000000u ? p&0x1fffffffu : p;
    };
    const auto fits=[ramSize](uint32_t p,size_t n) {
        return p<=ramSize && n<=ramSize-p;
    };
    const auto word=[ram](uint32_t p){uint32_t v;std::memcpy(&v,ram+p,4);return v;};
    payload=physical(payload);
    if (!ram || payloadBytes!=4 || !fits(payload,4) || !fits(0x593f20u,12))
        return std::nullopt;
    const uint32_t buffer=physical(word(payload));
    const uint32_t bytes=word(0x593f20u);
    if (word(0x593f24u)!=1 || physical(word(0x593f28u))!=buffer ||
        bytes<0x30 || bytes>0x7fffffffu || !fits(buffer,bytes))
        return std::nullopt;
    const uint32_t container=word(buffer+4);
    if (word(buffer)!=10 || word(buffer+8)!=0x4944494du ||
        container<0x30 || container>bytes || word(buffer+12)!=container-0x30)
        return std::nullopt;
    return static_cast<uint16_t>(word(buffer+0x28)>>16);
}
