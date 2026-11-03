#pragma once
#include "nocturne_pcm_ring.h"
#include <SoundTouch.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>
namespace magatama::native_sound {
// Single serialized PCM producer only. SDL still just reads PcmRing: no DSP,
// guest execution, allocation, wait or logging is added to its callback.
class PcmStretch {
    static constexpr unsigned chunk=256;
    soundtouch::SoundTouch processor;
    std::array<float,chunk*2> input{};
    std::array<float,1024*2> output{};
    std::array<double,8> fullness{};
    unsigned filled=0,averageIndex=0,averageCount=0;
    void drain(PcmRing& ring) {
        while(const auto n=processor.receiveSamples(output.data(),1024)) {
            for(unsigned i=0;i<n;++i)ring.pushFloat(output[2*i],output[2*i+1]);
            outputFrames+=n;
        }
    }
    void adjust(PcmRing& ring) {
        // Queue feedback, as in the pinned PCSX2 AudioStream: average the
        // fullness and damp with sqrt. Bound corrections for this fallback.
        // Before actual consumption begins, startup remains at unity tempo.
        const auto r=ring.read.load(std::memory_order_acquire);
        if(!r)return;
        const auto w=ring.write.load(std::memory_order_acquire);
        fullness[averageIndex]=double(w-r)/ring.prefill;
        averageIndex=(averageIndex+1)%fullness.size();
        averageCount=std::min<unsigned>(averageCount+1,fullness.size());
        double sum=0;for(unsigned i=0;i<averageCount;++i)sum+=fullness[i];
        double target=std::clamp(std::sqrt(sum/averageCount),0.70,1.08);
        if(target>0.97&&target<1.03)target=1.0;
        tempo=target;minTempo=std::min(minTempo,tempo);maxTempo=std::max(maxTempo,tempo);
        processor.setTempo(tempo); // rate and pitch both remain exactly 1
    }
public:
    double tempo=1,minTempo=1,maxTempo=1;
    uint64_t inputFrames=0,outputFrames=0;
    static bool requested() {
        const char* value=std::getenv("MAGATAMA_AUDIO_STRETCH");
        return !value||std::strcmp(value,"0")!=0;
    }
    PcmStretch() {
        static_assert(std::is_same_v<soundtouch::SAMPLETYPE,float>);
        processor.setSampleRate(48000);processor.setChannels(2);
        processor.setRate(1);processor.setPitch(1);processor.setTempo(1);
        processor.setSetting(SETTING_SEQUENCE_MS,20);
        processor.setSetting(SETTING_SEEKWINDOW_MS,10);
        processor.setSetting(SETTING_OVERLAP_MS,8);
    }
    void push(int16_t l,int16_t r,PcmRing& ring) {
        input[2*filled]=l/32768.f;input[2*filled+1]=r/32768.f;
        ++filled;++inputFrames;
        if(filled==chunk){processor.putSamples(input.data(),filled);filled=0;drain(ring);adjust(ring);}
    }
    // Finite-stream/headless tests only; normal playback never flushes or
    // discards queued samples at a scene or battle boundary.
    void finish(PcmRing& ring) {
        if(filled){processor.putSamples(input.data(),filled);filled=0;}
        processor.flush();drain(ring);
    }
};
}
