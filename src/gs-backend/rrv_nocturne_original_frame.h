#pragma once
#include <cstdint>
#include <atomic>

// Opt-in presentation policy. Every guest/GS/render field still executes.
namespace magatama::original_frame {
inline std::atomic<bool> hostMenu{false}, fastForward{false};
struct Delivery {
 uint64_t field=0;
 uint32_t kernel=0, request=0, slot=3, tag=0, mode=0, count=0, cadence=0;
};
struct Snapshot {
 uint64_t completed=0, field=0;
 bool supported=false, ready=false;
};
class PairTracker {
 Delivery first_{};
 bool pending_=false;
 uint64_t completed_=0;
 Snapshot latest_{};
 static bool same(const Delivery& a,const Delivery& b) {
  // The kernel may start its next update between the two draw phases.
  // The original queued request and slot/tag identify the rendered pair.
  return a.request==b.request && a.slot==b.slot && a.tag==b.tag;
 }
public:
 Snapshot deliver(const Delivery& d) {
  latest_={completed_,d.field,false};
  if(d.cadence!=2 || d.slot>=3 || !d.tag || d.mode>1 || d.count!=d.mode) {
   pending_=false;return latest_;
  }
  if(d.mode==0) {
   first_=d;pending_=true;latest_.supported=true;return latest_;
  }
  if(pending_ && same(first_,d) && d.field>first_.field && d.field-first_.field<=2) {
   ++completed_;latest_={completed_,d.field,true,true};
  }
  pending_=false;return latest_;
 }
 Snapshot snapshot() const{return latest_;}
};
struct Decision {bool present; uint32_t fields_per_present;};
class Selector {
 uint64_t seen_=0;
public:
 Decision field(uint64_t field,const Snapshot& s,bool full_frame,bool host_menu,bool fast_forward) {
  // Unsupported or late/ambiguous receipts never suppress a field. Native
  // field mode retains both parities; the host menu retains its own refresh.
  if(!full_frame || host_menu || fast_forward || !s.supported || field<s.field || field-s.field>2) {
   seen_=s.completed;return {true,1};
  }
  const bool fresh=s.ready && s.completed!=seen_;
  if(fresh)seen_=s.completed;
  return {fresh,2};
 }
};
} // namespace magatama::original_frame
