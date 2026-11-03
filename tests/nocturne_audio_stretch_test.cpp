#include "nocturne_audio_stretch.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <complex>
#include <cstdio>
using namespace magatama::native_sound;
unsigned checks=0;
void check(bool v,const char* what){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}}
struct Stats {uint64_t events,missing,overruns,generated;double low,high,pitch;};
Stats model(const std::vector<double>& times,bool stretched){
 PcmRing ring;PcmStretch dsp;std::array<float,2048> callback{};std::vector<float> heard;
 uint64_t generated=0;size_t field=0;double now=0;double cb=0;const double origin=times[1400];
 while(field+1400<times.size()&&field<2400){
  const double next=times[field+1400]-origin;
  if(next<=cb){
   const uint64_t target=uint64_t(std::llround((field+1)*800.8));
   while(generated<target){const auto v=int16_t(12000*std::sin(generated*2*3.141592653589793*440/48000));
    if(stretched)dsp.push(v,-v,ring);else ring.push(v,-v);++generated;}
   ++field;now=next;
  }else{
   const auto n=PcmRing::pull(&ring,callback.data(),1024);
   if(stretched)for(size_t i=0;i<n;++i){
    check(std::isfinite(callback[2*i])&&std::isfinite(callback[2*i+1]),"finite DSP output");
    check(std::abs(callback[2*i]+callback[2*i+1])<1e-6,"stereo alignment preserved");heard.push_back(callback[2*i]);}
   cb+=1024./48000.;now=cb;
  }
 }
 double pitch=0,best=-1;const size_t count=std::min<size_t>(heard.size(),48000);
 if(stretched&&count){
  const auto begin=(heard.size()-count)/2;
  for(int k=4350;k<=4450;++k){double f=k/10.;std::complex<double> z(1,0),sum(0,0),step=std::polar(1.,-2*3.141592653589793*f/48000);
   for(size_t i=0;i<count;++i){sum+=double(heard[begin+i])*z;z*=step;}
   if(std::norm(sum)>best){best=std::norm(sum);pitch=f;}}
 }
 return {ring.underrunEvents,ring.underrun,ring.overrun,generated,dsp.minTempo,dsp.maxTempo,pitch};
}
int main(int argc,char**argv){
 check(argc==2,"recorded guest-start timings provided");std::ifstream f(argv[1]);check(bool(f),"timing file opened");
 std::vector<double> times(1);std::string line;while(std::getline(f,line)){std::istringstream s(line);uint64_t ordinal,ns;if(s>>ordinal>>ns){if(times.size()<=ordinal)times.resize(ordinal+1);times[ordinal]=ns*1e-9;}}
 check(times.size()>3800,"two-battle timing coverage");const auto raw=model(times,false),stretch=model(times,true);
 std::printf("RECORDED_TIMING_MODEL raw_events=%llu raw_missing=%llu stretch_events=%llu stretch_missing=%llu tempo=%.4f..%.4f pitch=%.1fHz\n",(unsigned long long)raw.events,(unsigned long long)raw.missing,(unsigned long long)stretch.events,(unsigned long long)stretch.missing,stretch.low,stretch.high,stretch.pitch);
 check(raw.events>0,"recorded slow phase starves unprocessed ring");
 check(stretch.events==0&&stretch.overruns==0,"recorded slowdown handled with no gaps or overflow");
 check(stretch.generated==raw.generated,"identical input sample count; no guest-rate adjustment");
 check(stretch.low>=.70&&stretch.high<=1.08,"correction bounded");
 check(std::abs(stretch.pitch-440)<1.1,"tempo changes preserve 440Hz pitch");
 std::printf("PASS pitch-preserving playback: %u checks; synthetic tone + recorded timing, no game/card/device\n",checks);
}
