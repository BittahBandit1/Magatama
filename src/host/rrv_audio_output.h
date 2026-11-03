#pragma once

#include <algorithm>
#include <atomic>

namespace rrv::audio {
// Host output only: keep the native synthesizer, audio clock and ring running.
// Shared by the menu owner and the hardware callback without a mixer lock.
inline std::atomic<int> g_outputVolume{100};
inline std::atomic<bool> g_outputMuted{false};
inline void setOutputVolume(int percent) {
    g_outputVolume.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
}
inline int outputVolume() { return g_outputVolume.load(std::memory_order_relaxed); }
inline void setOutputMuted(bool muted) { g_outputMuted.store(muted, std::memory_order_relaxed); }
inline bool outputMuted() { return g_outputMuted.load(std::memory_order_relaxed); }
} // namespace rrv::audio
