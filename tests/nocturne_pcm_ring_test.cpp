#include "nocturne_pcm_ring.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <cmath>
using magatama::native_sound::PcmRing;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
uint64_t jitter(size_t prefill){
 PcmRing ring(prefill);std::vector<float> out(2048);
 size_t field=0;uint64_t generated=0;
 // 59.94-Hz guest PCM in bursts vs 48-kHz 1024-frame callbacks. A 40-ms
 // host production pause once a second catches up without skipping samples.
 for(uint64_t host=0;host<480000;host+=1024){
  for(;;){uint64_t nominal=uint64_t(std::llround(field*800.8));uint64_t phase=nominal%48000;
   uint64_t release=phase>=30000&&phase<31920 ? nominal-phase+31920:nominal;
   if(release>host)break;
   const auto target=uint64_t(std::llround((field+1)*800.8));
   while(generated<target){ring.push(1000,-1000);++generated;}++field;
  }
  PcmRing::pull(&ring,out.data(),1024);
 }
 check(ring.overrun==0,"jitter model never skips produced samples");return ring.underrun.load();
}
int main(){
 check(PcmRing::prefillFromEnvironment()==4096,"85.3ms default headroom");
 PcmRing ring;std::vector<float> out(PcmRing::capacity*2,123);
 for(unsigned i=0;i<4095;++i)ring.push(int16_t(i),-int16_t(i));
 check(PcmRing::pull(&ring,out.data(),1024)==0,"startup waits for safe headroom");
 check(ring.read==0&&ring.primingFrames==1024,"prefill never consumes PCM");
 ring.push(4095,-4095);
 check(PcmRing::pull(&ring,out.data(),1024)==1024,"startup begins at threshold");
 for(unsigned i=0;i<1024;++i)check(out[i*2]==i/32768.f&&out[i*2+1]==-int(i)/32768.f,"PCM stereo amplitudes/order unchanged");
 for(unsigned i=0;i<PcmRing::capacity+500;++i)ring.push(2222,-3333);
 check(ring.overrun>0,"capacity stays bounded");
 const auto before=ring.read.load();const auto n=PcmRing::pull(&ring,out.data(),PcmRing::capacity);
 check(n==PcmRing::capacity&&ring.read==before+n,"wrapped ring drains exact available frames");
 check(PcmRing::pull(&ring,out.data(),1024)==0&&ring.underrunEvents==1,"shortage records one event and reprimes");
 const auto old=jitter(2048),now=jitter(4096);
 std::printf("JITTER old_underrun_frames=%llu new_underrun_frames=%llu\n",(unsigned long long)old,(unsigned long long)now);
 check(old>0&&now==0,"bounded 40ms production jitter handled without underruns");
 PcmRing concurrent;constexpr unsigned total=131072;
 std::thread producer([&]{for(unsigned i=0;i<total;++i){while(concurrent.write.load()-concurrent.read.load()==PcmRing::capacity)std::this_thread::yield();concurrent.push(int16_t(i%16000),-int16_t(i%16000));}});
 unsigned read=0;while(read<total){if(concurrent.write.load()-concurrent.read.load()<(read?512:4096)){std::this_thread::yield();continue;}
  auto n=PcmRing::pull(&concurrent,out.data(),512);for(unsigned i=0;i<n;++i)check(out[2*i]==((read+i)%16000)/32768.f,"concurrent wrap preserves every sample");read+=n;}
 producer.join();check(concurrent.overrun==0&&concurrent.underrun==0,"concurrent SPSC exact with no overruns");
 std::printf("PASS native PCM ring: %u checks, no device/game/card\n",checks);
}
