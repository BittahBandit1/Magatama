#pragma once
#include <array>
#include <cstdint>

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
