#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace magatama::native_sound {
// SPSC: original guest-clock producer, SDL-only consumer. No waits, allocation,
// resampling, synthesis or guest execution in the playback callback.
struct PcmRing {
    static constexpr size_t capacity = 16384;
    std::array<float, capacity * 2> pcm{};
    std::atomic<uint64_t> read{0}, write{0}, underrun{0}, overrun{0};
    std::atomic<uint64_t> underrunEvents{0}, primingFrames{0};
    size_t prefill = 4096; // 85.3 ms; two extra 1024-frame callbacks of headroom
    bool priming = true; // SDL thread only
    explicit PcmRing(size_t start = 4096) : prefill(std::clamp<size_t>(start,1024,8192)) {}
    static size_t prefillFromEnvironment() {
        const char* value=std::getenv("MAGATAMA_AUDIO_BUFFER_MS");
        if (!value || !*value) return 4096;
        char* end=nullptr;const auto ms=std::strtoul(value,&end,10);
        return end!=value && !*end && ms>=22 && ms<=170 ? ms*48 : 4096;
    }
    void push(int16_t l, int16_t r) { pushFloat(l/32768.f,r/32768.f); }
    void pushFloat(float l, float r) {
        const auto w=write.load(std::memory_order_relaxed);
        if(w-read.load(std::memory_order_acquire)==capacity){++overrun;return;}
        pcm[(w%capacity)*2]=l;pcm[(w%capacity)*2+1]=r;
        write.store(w+1,std::memory_order_release);
    }
    // Returns delivered frames; caller zeroes the unused tail as before.
    static size_t pull(void* user,float* dst,size_t frames) {
        auto& s=*static_cast<PcmRing*>(user);
        auto r=s.read.load(std::memory_order_relaxed),w=s.write.load(std::memory_order_acquire);
        if(s.priming && w-r<s.prefill){s.primingFrames+=frames;return 0;}
        s.priming=false;
        const size_t n=std::min<uint64_t>(frames,w-r);
        for(size_t i=0;i<n;++i){dst[2*i]=s.pcm[((r+i)%capacity)*2];dst[2*i+1]=s.pcm[((r+i)%capacity)*2+1];}
        s.read.store(r+n,std::memory_order_release);
        if(n<frames){s.underrun+=frames-n;++s.underrunEvents;s.priming=true;}
        return n;
    }
};
}
