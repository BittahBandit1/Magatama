#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace rrv::audio {
// Diagnostic only: allocate before the device starts, copy on the callback,
// write after it joins. Four two-second windows, under 4.3MB total memory.
// No callback allocation, file IO, logging or change to sample consumption.
class OutputProbe {
public:
    struct Record {
        uint64_t hostNs=0,streamFrame=0;
        uint32_t frames=0;
        std::array<float,2048> samples{};
    };
    static constexpr unsigned capacity=512;
private:
    std::unique_ptr<Record[]> records=std::make_unique<Record[]>(capacity);
    uint64_t startNs,streamFrame=0;
    unsigned used=0;
public:
    explicit OutputProbe(uint64_t start):startNs(start){}
    void observe(const float* samples,size_t frames,uint64_t now) noexcept {
        const double seconds=now>=startNs ? (now-startNs)/1e9 : -1;
        const bool selected=(seconds>=16 && seconds<18)||(seconds>=38 && seconds<40)||
                            (seconds>=88 && seconds<90)||(seconds>=103 && seconds<105);
        if(selected && used<capacity) {
            auto& r=records[used++];r.hostNs=now;r.streamFrame=streamFrame;
            r.frames=static_cast<uint32_t>(std::min<size_t>(frames,1024));
            std::memcpy(r.samples.data(),samples,r.frames*2*sizeof(float));
        }
        streamFrame+=frames;
    }
    unsigned count()const {return used;}
    float firstSample()const {return used ? records[0].samples[0] : 0;}
    bool save(const char* path)const {
        std::FILE* f=std::fopen(path,"wb");if(!f)return false;
        const uint32_t header[4]={0x31504f41u,48000,2,used}; // AOP1, little-endian host
        bool ok=std::fwrite(header,sizeof(header),1,f)==1;
        for(unsigned i=0;i<used && ok;++i) {
            const auto& r=records[i];
            ok=std::fwrite(&r.hostNs,8,1,f)==1 && std::fwrite(&r.streamFrame,8,1,f)==1 &&
               std::fwrite(&r.frames,4,1,f)==1 && std::fwrite(r.samples.data(),8,r.frames,f)==r.frames;
        }
        return std::fclose(f)==0 && ok;
    }
};
}
