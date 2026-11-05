#pragma once
#include "nocturne_pose_math.h"
#include "nocturne_render_pose_trace.h"
#include "rrv_nocturne_original_frame.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>

namespace magatama::pose {
// Owner-only render history. Pose generations come from completed original
// draw pairs and consumed packet bytes, never from the preemptible kernel counter.
class Interpolator {
 using Record=pose_trace::Record;
 using Key=std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>;
 std::map<Key,Record> previous_,current_;
 std::map<uint32_t,Record> packets_;
 Record first_{},phase_{};
 uint64_t previousField_=0;
 bool configured_=false,pending_=false,pair_=false,cameraSafe_=false;
 // The current viewport's endpoint and halfway view matrices (camera packet
 // bytes 0..63), valid while cameraSafe_ and only when the view moved.
 Matrix camEnd_{},camMid_{};bool camMoved_=false;
 uint64_t heldShifted_=0;
 size_t pruneAt_=16384;
 uint64_t wipes_=0,phases_=0,model_=0,camera_=0,changed_=0,mismatch_=0,rejected_=0,complete_=0;
 // Per-phase diagnostics: seen, unknown-packet, mismatch, no-history, camera-unsafe, rejected, interpolated, endpoint.
 std::array<uint32_t,8> why_{};
 void summarize(){if(why_[0]){auto r=phase_;r.kind='R';r.bytes=32;std::memcpy(r.data.data(),why_.data(),32);pose_trace::trace().add(r);}why_={};}
 // Camera history ignores the battle director state (mode/flags); a state
 // change is checked against camera motion instead (see unpack).
 Key key(const Record& r) const{return r.kind=='C'?Key{r.kind,r.node,r.root,r.shape,r.slot,0u,0u}:Key{r.kind,r.node,r.root,r.shape,r.slot,r.mode,r.flags};}
 void invalidate(){previous_.clear();current_.clear();pending_=pair_=cameraSafe_=false;}
public:
 void configure(){configured_=true;}
 // Optional: a model whose skinned body stays at its endpoint this frame
 // keeps its rigid VU1 parts there too, so they never separate.
 bool (*rootHeldBack)(uint32_t root)=nullptr;
 void reset(){packets_.clear();invalidate();}
 bool enabled() const{return configured_;}
 void publish(const Record& r){
  if(!configured_)return;
  if(r.kind=='F'){
   for(auto i=previous_.begin();i!=previous_.end();)if(std::get<1>(i->first)==r.node)i=previous_.erase(i);else ++i;
   for(auto i=packets_.begin();i!=packets_.end();)if(i->second.node==r.node)i=packets_.erase(i);else ++i;
   current_.clear();return;
  }
  if((r.kind!='P'&&r.kind!='C')||!r.node||!r.root||(r.kind=='P'&&(!r.shape||r.flags!=1)))return;
  if(r.kind=='C'&&(r.root!=0x2f59b0u||!r.shape||!r.slot))return;
  // Temporary packets get new addresses every frame: drop entries not
  // republished recently instead of wiping everything (which cost a frame or
  // two of interpolation every few seconds in the field).
  if(packets_.size()>pruneAt_){
   ++wipes_;const uint64_t now=r.field;
   for(auto i=packets_.begin();i!=packets_.end();)if(i->second.field+8<now)i=packets_.erase(i);else ++i;
   if(packets_.size()>65536){packets_.clear();invalidate();}
   pruneAt_=std::max<size_t>(16384,packets_.size()*2);
  }
  packets_[r.packet]=r;
 }
 void begin(const Record& r){
  summarize();phase_=r;cameraSafe_=false;++phases_;
  if(r.flags!=2||r.mode>1||r.slot>1||!r.packet){invalidate();return;}
  if(r.mode==0){
   if(pending_)previous_.clear();
   first_=r;pending_=true;pair_=false;current_.clear();
   if(r.field<previousField_||r.field-previousField_>2)previous_.clear();
  }else{
   pair_=pending_&&first_.root==r.root&&first_.packet==r.packet&&first_.slot==r.slot&&r.field>first_.field&&r.field-first_.field<=2;
   if(!pair_)invalidate();
  }
 }
 void finish(uint32_t mode,uint32_t count,uint64_t field){
  if(mode!=1)return;
  if(count==1&&pair_&&phase_.mode==1&&field>=phase_.field&&field-phase_.field<=1){
   previous_=std::move(current_);current_.clear();previousField_=field;++complete_;
  }else previous_.clear();
  pending_=pair_=cameraSafe_=false;
 }
 bool unpack(uint32_t source,uint32_t command,const uint8_t* data,uint32_t bytes,uint8_t* out){
  if(!configured_||!pending_||original_frame::hostMenu.load(std::memory_order_acquire)||original_frame::fastForward.load(std::memory_order_acquire))return false;
  // Each viewport packet must establish its own continuity. A later unknown,
  // changed or stale view must not leave models authorized by an earlier view.
  command&=0x7fffffffu;
  if(command==0x6c0bc000u)cameraSafe_=false;
  // Model TMs: UNPACK V4-32 x7 with USN|FLG at any VU address. One-skin
  // models (sdfModelSetOneSkinDrawPacket) send each bone to VU address 1,8,15...
  const bool modelTm=(command&~0x3ffu)==0x6c07c000u&&bytes==112;
  if(!modelTm&&(command!=0x6c0bc000u||bytes!=176))return false;
  ++why_[0];auto it=packets_.find(source);if(it==packets_.end()){++why_[1];return false;}auto now=it->second;
  if(now.kind=='C')cameraSafe_=false;
  // Packet identity and VIF layout must agree before math reads either payload.
  if((now.kind=='C' && bytes!=176)||(now.kind=='P' && bytes!=112))return false;
  if(phase_.field<now.field||phase_.field-now.field>4||std::memcmp(data,now.data.data(),bytes)!=0){++mismatch_;++why_[2];return false;}
  now.field=phase_.field;std::memcpy(now.data.data(),data,bytes);
  const auto id=key(now);
  if(phase_.mode==1){current_[id]=now;++why_[7];return false;} // authoritative endpoint, byte-for-byte original
  auto old=previous_.find(id);if(old==previous_.end()){++why_[3];return false;}
  if(now.kind=='P'&&!cameraSafe_){++why_[4];return false;}
  if(now.kind=='P'&&rootHeldBack&&rootHeldBack(now.root)){
   ++why_[5];
   // Its skinned body stays at the endpoint pose, seen through the endpoint
   // camera (baked into the EE packets). Keep this rigid part with it: endpoint
   // pose, and cancel the halfway camera for it: TM' = TM * Vend * inverse(Vmid),
   // so TM' * Vmid = TM * Vend (row vectors, translation in row 3).
   if(!camMoved_)return false;
   Matrix tm,inv;std::memcpy(tm.data(),data,64);
   if(!inverseAffine(camMid_,inv))return false;
   const Matrix shifted=multiply(multiply(tm,camEnd_),inv);
   if(!finite(shifted))return false;
   std::memcpy(out,data,bytes);std::memcpy(out,shifted.data(),64);++heldShifted_;
   return true;
  }
  // A director state change with a still or nearly still camera is not a cut:
  // blend only when the eye moved <=20 units and the view turned <=10 degrees.
  if(now.kind=='C'&&(old->second.mode!=now.mode||old->second.flags!=now.flags)&&
     !cameraNear(old->second.data.data(),data,20.f,0.9961947f)){++why_[5];++rejected_;return false;}
  const bool valid=now.kind=='C'?cameraPacket(old->second.data.data(),data,.5f,out):modelPacket(old->second.data.data(),data,.5f,out);
  if(now.kind=='C'){
   cameraSafe_=valid;camMoved_=false;
   if(valid&&std::memcmp(data,out,64)!=0){std::memcpy(camEnd_.data(),data,64);std::memcpy(camMid_.data(),out,64);camMoved_=true;}
  }
  if(!valid){++rejected_;++why_[5];return false;}
  ++why_[6];
  // Diagnostic only: MAGATAMA_INTERPOLATE_PROBE=1 shifts interpolated models
  // far sideways so a capture shows whether these bytes reach the renderer.
  static const bool probe=[]{const char* v=std::getenv("MAGATAMA_INTERPOLATE_PROBE");return v&&v[0]=='1';}();
  if(probe&&now.kind=='P'){float tx;std::memcpy(&tx,out+48,4);tx+=200.f;std::memcpy(out+48,&tx,4);}
  if(bytes==112)++model_;else ++camera_;if(std::memcmp(data,out,bytes)!=0)++changed_;
  auto trace=now;trace.kind='I';trace.mode=0;trace.request=phase_.request;trace.bytes=bytes;std::memcpy(trace.data.data(),out,bytes);pose_trace::trace().add(trace);
  return true;
 }
 ~Interpolator(){if(configured_)std::fprintf(stderr,"[pose-interpolation] experimental60 phases=%llu complete=%llu model=%llu camera=%llu changed=%llu mismatch-or-stale=%llu rejected-cut-or-shape=%llu prunes=%llu held-with-body=%llu\n",(unsigned long long)phases_,(unsigned long long)complete_,(unsigned long long)model_,(unsigned long long)camera_,(unsigned long long)changed_,(unsigned long long)mismatch_,(unsigned long long)rejected_,(unsigned long long)wipes_,(unsigned long long)heldShifted_);}
};
inline Interpolator& interpolator(){static Interpolator value;return value;}
} // namespace magatama::pose
