#include "rrv_audio_output_probe.h"
#include <cassert>
#include <fstream>
#include <cstdio>
int main() {
    rrv::audio::OutputProbe probe(0);
    std::array<float,2048> input{};input.fill(0.25f);
    probe.observe(input.data(),1024,15999999999ull);assert(probe.count()==0);
    probe.observe(input.data(),1024,16000000000ull);assert(probe.count()==1);
    probe.observe(input.data(),1024,17999999999ull);assert(probe.count()==2);
    probe.observe(input.data(),1024,18000000000ull);assert(probe.count()==2);
    probe.observe(input.data(),1024,103000000000ull);assert(probe.count()==3);
    probe.observe(input.data(),1024,105000000000ull);assert(probe.count()==3);
    for(unsigned i=0;i<600;++i)probe.observe(input.data(),1024,88000000000ull);
    assert(probe.count()==512 && probe.firstSample()==0.25f);
    for(float x:input)assert(x==0.25f);
    const char* path="/private/tmp/magatama-output-probe-fixture.bin";
    assert(probe.save(path));
    std::ifstream f(path,std::ios::binary);uint32_t header[4];f.read((char*)header,sizeof(header));
    assert(header[0]==0x31504f41 && header[1]==48000 && header[2]==2 && header[3]==512);
    uint64_t timestamp,frame;uint32_t frames;float sample;
    f.read((char*)&timestamp,8);f.read((char*)&frame,8);f.read((char*)&frames,4);f.read((char*)&sample,4);
    assert(timestamp==16000000000ull && frame==1024 && frames==1024 && sample==0.25f);
    f.close();std::remove(path);
    std::puts("PASS bounded output probe: exact windows, immutable samples, record cap, callback timestamps/frame positions, serialization");
}
