#pragma once
// Magatama experimental 60 FPS: in-between poses for EE-skinned models.
//
// Nocturne skins characters on the EE (sdfCorePacketGen, USA 0x2B9178): bone
// matrices (node->tmPacket) and sdf3DScreenTm are baked into finished GS
// packets, so the second draw of a frame has no matrix left to blend. For each
// generated packet the wrapper (patches.cpp) runs the original generator once
// more, off the guest clock, with every bone and the screen matrix set halfway
// between the previous built frame and this one, then restores guest memory
// and runs the original for real. The halfway bytes are kept host-side and are
// substituted into the VIF1 chain only for the first draw of that frame, and
// only while the guest packet still holds exactly the endpoint bytes built
// alongside them. Guest state, timing and the second draw stay original.
#include "nocturne_pose_math.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

uint64_t rrv_gate3_realtime_waits();
uint64_t rrv_gate3_realtime_starts(); // paced field starts so far (the real-time pacer)

namespace magatama::skin {

struct Matrix128 { std::array<uint8_t,128> b{}; };

class Interpolator {
    struct History { uint64_t gen=0, prevGen=0; Matrix128 cur{}, prev{}; uint64_t midGen=0; Matrix128 mid{}; bool midOk=false; };
    struct Entry { uint64_t gen=0; uint32_t root=0; std::vector<uint8_t> endpoint, mid; bool used=false; };
    std::mutex mutex_;
    bool configured_=false;
    uint64_t gen_=0; int32_t lastFrame_=-1;
    std::unordered_map<uint32_t,History> nodes_;
    std::unordered_map<uint32_t,History> screens_;
    std::unordered_map<uint32_t,Entry> entries_;
    std::unordered_map<uint32_t,uint64_t> badRoots_; // model root -> frame with a packet left at its endpoint
    std::atomic<bool> firstDraw_{false};
    std::atomic<uint64_t> drawGen_{0};
    uint64_t generated_=0, registered_=0, sizeMismatch_=0, noHistory_=0, rejected_=0, substituted_=0, stale_=0, partial_=0, resized_=0;

    static bool elementwise(const uint8_t* a,const uint8_t* b,uint8_t* out){
        // Fallback for matrices the TRS decomposition refuses (shear, odd
        // scale): plain halfway, only for small changes.
        float x[16],y[16],m[16];std::memcpy(x,a,64);std::memcpy(y,b,64);
        float diff=0,mag=1e-6f;
        for(int i=0;i<16;++i){if(!std::isfinite(x[i])||!std::isfinite(y[i]))return false;diff=std::max(diff,std::fabs(x[i]-y[i]));mag=std::max(mag,std::fabs(y[i]));m[i]=.5f*(x[i]+y[i]);}
        if(diff>.25f*mag)return false;
        std::memcpy(out,b,128);std::memcpy(out,m,64);
        for(unsigned j=0;j<3;++j){float n=std::sqrt(m[j]*m[j]+m[4+j]*m[4+j]+m[8+j]*m[8+j]);if(n<1e-6f)return false;for(unsigned i=0;i<3;++i){float v=m[i*4+j]/n;std::memcpy(out+64+16*i+4*j,&v,4);}}
        return true;
    }
    // Advance a history to this generation from the guest's current bytes.
    // False when the same key shows different bytes twice in one frame.
    bool observe(History& h,const uint8_t* now,size_t bytes){
        if(h.gen==gen_)return std::memcmp(h.cur.b.data(),now,bytes)==0;
        if(h.gen){h.prev=h.cur;h.prevGen=h.gen;}
        h.cur.b.fill(0);std::memcpy(h.cur.b.data(),now,bytes);h.gen=gen_;h.midGen=0;return true;
    }
public:
    int lastWhy=0;
    bool debugChain=false; // diagnostics: 1 no history, 2 discontinuity, 3 changed within a frame
    void configure(){configured_=true;}
    bool enabled() const{return configured_;}
    std::mutex& mutex(){return mutex_;}

    // Called (under mutex) at the top of each generator call. `frame` is the
    // guest's double-buffer index; a change starts a new built frame.
    void frame(int32_t f){if(f!=lastFrame_){lastFrame_=f;++gen_;}}
    uint64_t generation() const{return gen_;}

    // Real-time budget for the halfway passes (host microseconds per built
    // frame: the halfway pass plus its bookkeeping). The extra work must never make the game fall behind: when a
    // built frame takes longer than a 2-field frame, the budget shrinks;
    // while frames keep time it grows back. Decided per model per frame.
    //
    // Frames are whole fields: a built frame that runs even slightly past its 2 fields costs
    // a third, so the budget admits a model only when its cost (measured on its previous
    // frames) still fits, grows slowly while frames keep time and halves on a late frame.
    struct Budget {
        uint64_t gen=0; std::chrono::steady_clock::time_point start{};
        double budgetUs=1000, spentUs=0, reservedUs=0; std::unordered_map<uint32_t,bool> allowed;
        std::unordered_map<uint32_t,double> cost, frameCost; // per model root: smoothed us per frame; this frame's
        uint64_t skippedModels=0, frames=0, slowFrames=0, waits=0;
        static constexpr uint32_t kWindow=16; // built frames (32 fields, ~0.5 s)
        struct Sample{double us=0,fields=0;}; std::array<Sample,kWindow> window{}; uint32_t windowAt=0, windowFill=0; uint64_t startsAt=0;
    } budget;
    bool allow(uint32_t root){
        auto& b=budget;const auto now=std::chrono::steady_clock::now();
        if(b.gen!=gen_){
            // Behind real time = the built frames of the last ~half second took longer than their fields. Single
            // frames jitter with presentation (whole 120 Hz refreshes), and a frame the display holds shows no
            // pacer wait even on time, so neither one frame's length nor the pacer's waits tell (b42 on the Nova,
            // the Tower of Kagutsuchi: every frame "waited", the budget stayed at its 20 ms cap and the game ran
            // at 87 % speed). On a window: shrink the halfway work while the game is behind, grow it back slowly
            // while it keeps time; after shrinking, let the window refill before judging again.
            if(b.gen&&b.gen+1==gen_){
                ++b.frames;
                const uint64_t starts=rrv_gate3_realtime_starts();
                const double frameUs=std::chrono::duration<double,std::micro>(now-b.start).count();
                if(frameUs<100000&&starts>b.startsAt){
                    auto& w=b.window[b.windowAt++%Budget::kWindow];w.us=frameUs;w.fields=double(starts-b.startsAt);
                    b.windowFill=std::min<uint32_t>(b.windowFill+1,Budget::kWindow);
                }else b.windowFill=0; // a hitch (loading) or no paced fields: start the window over
                if(b.windowFill==Budget::kWindow){
                    constexpr double kFieldUs=1e6*1001/60000; // one NTSC field
                    double us=0,fields=0;for(const auto& w:b.window){us+=w.us;fields+=w.fields;}
                    const double ratio=us/(fields*kFieldUs);
                    if(ratio>1.02){++b.slowFrames;b.budgetUs=std::min(b.budgetUs,b.spentUs)*0.5;b.windowFill=0;}
                    else if(ratio<1.01)b.budgetUs=std::min(20000.0,b.budgetUs+250);
                }
            }
            b.startsAt=rrv_gate3_realtime_starts();
            for(const auto& [r,us]:b.frameCost){auto i=b.cost.find(r);if(i==b.cost.end())b.cost.emplace(r,us);else i->second=.75*i->second+.25*us;}
            if(b.cost.size()>4096)b.cost.clear();
            b.frameCost.clear();
            b.gen=gen_;b.start=now;b.spentUs=0;b.reservedUs=0;b.allowed.clear();
        }
        auto i=b.allowed.find(root);
        if(i==b.allowed.end()){
            auto c=b.cost.find(root);const double predicted=c==b.cost.end()?500.0:c->second;
            const bool ok=std::max(b.spentUs,b.reservedUs)+predicted<=b.budgetUs;
            if(ok)b.reservedUs=std::max(b.spentUs,b.reservedUs)+predicted;
            i=b.allowed.emplace(root,ok).first;
        }
        if(!i->second)++b.skippedModels;
        return i->second;
    }
    void spent(uint32_t root,double us){budget.spentUs+=us;if(root&&budget.allowed.count(root)&&budget.allowed[root])budget.frameCost[root]+=us;}

    // Bone/node halfway TM (128 bytes, VU1 TM packet layout). False: no
    // usable history or a discontinuity (teleport, cut, large turn).
    bool nodeMid(uint32_t node,const uint8_t* now,uint8_t* out){
        auto& h=nodes_[node];if(!observe(h,now,128)){++rejected_;lastWhy=3;return false;}
        if(h.midGen!=gen_){
            h.midGen=gen_;h.midOk=false;
            if(h.prevGen+1==gen_){
                h.mid.b=h.cur.b;
                if(std::memcmp(h.prev.b.data(),h.cur.b.data(),112)==0)h.midOk=true;
                else h.midOk=pose::modelPacket(h.prev.b.data(),h.cur.b.data(),.5f,h.mid.b.data())||
                             elementwise(h.prev.b.data(),h.cur.b.data(),h.mid.b.data());
            }
        }
        if(!h.midOk){if(h.prevGen+1!=gen_){++noHistory_;lastWhy=1;}else{++rejected_;lastWhy=2;}return false;}
        std::memcpy(out,h.mid.b.data(),128);return true;
    }
    // The main camera's view packet (176 bytes: view, view*projection, ...)
    // as the game builds it each frame (EE, before the frame's models). With
    // it the halfway screen matrix is the exact one the VU1 path draws rigid
    // parts with (pose::cameraPacket), so eyes and weapons stay on the body.
    struct Camera { std::array<uint8_t,176> data{}; int32_t frame=-1; uint64_t gen=0; bool valid=false; };
    Camera camCur_, camPrev_;
    uint64_t cameraMatched_=0, cameraFallback_=0;
    void camera(int32_t frame,const uint8_t* view){
        std::lock_guard<std::mutex> lock(mutex_);
        if(camCur_.valid&&camCur_.frame==frame&&camCur_.gen+1>=gen_){std::memcpy(camCur_.data.data(),view,176);return;}
        camPrev_=camCur_;camCur_.frame=frame;camCur_.gen=gen_;camCur_.valid=true;std::memcpy(camCur_.data.data(),view,176);
    }
    // A model the budget skips this frame: keep its histories current (a copy,
    // no decomposition) so it can blend again as soon as it is allowed.
    void observeNode(uint32_t node,const uint8_t* now){if(!observe(nodes_[node],now,128))++rejected_;}
    void observeScreen(uint32_t root,const uint8_t* now){observe(screens_[root],now,64);}
    void forgetNode(uint32_t node){nodes_.erase(node);++staleNodes_;}
    uint64_t staleNodes_=0;
    // Screen matrix (64 bytes) as used with one model root: halfway element
    // by element; refuse cuts. Keyed by root so each view keeps its history.
    bool screenMid(uint32_t root,const uint8_t* now,uint8_t* out){
        // Main camera: interpolate exactly like the VU1 path does.
        if(camCur_.valid&&camPrev_.valid&&camCur_.frame==lastFrame_&&(camCur_.gen+1==gen_||camCur_.gen==gen_)&&
           camPrev_.gen+1==camCur_.gen&&std::memcmp(now,camCur_.data.data()+64,64)==0){
            uint8_t o[176];
            auto& h=screens_[root];observe(h,now,64); // keep the fallback history current
            if(!pose::cameraPacket(camPrev_.data.data(),camCur_.data.data(),.5f,o)){++rejected_;return false;}
            ++cameraMatched_;std::memcpy(out,o+64,64);return true;
        }
        ++cameraFallback_;
        auto& h=screens_[root];if(!observe(h,now,64)){++rejected_;return false;}
        if(h.prevGen+1!=gen_){++noHistory_;return false;}
        float a[16],b[16],m[16],diff=0,mag=1e-6f;std::memcpy(a,h.prev.b.data(),64);std::memcpy(b,h.cur.b.data(),64);
        for(int i=0;i<16;++i){if(!std::isfinite(a[i])||!std::isfinite(b[i]))return false;diff+=(a[i]-b[i])*(a[i]-b[i]);mag+=b[i]*b[i];m[i]=.5f*(a[i]+b[i]);}
        if(diff>0.04f*mag){++rejected_;return false;} // >20% change: a camera cut
        std::memcpy(out,m,64);return true;
    }
    // A generated packet: endpoint and halfway payloads at the same guest address.
    // The halfway packet may hold a different number of faces (culling and
    // clipping follow the pose); it replaces the endpoint's data in the VIF1
    // stream whole, whatever its length.
    void record(uint32_t root,uint32_t address,const uint8_t* endpoint,const uint8_t* mid,uint32_t bytes,uint32_t midBytes){
        ++generated_;
        // An empty halfway packet (every face culled at the halfway pose) is
        // valid: that part draws nothing in the in-between field.
        if(!bytes||midBytes>4*bytes+4096){++sizeMismatch_;entries_.erase(address);bad(root);return;}
        if(midBytes!=bytes)++resized_;
        auto& e=entries_[address];e.gen=gen_;e.root=root;e.used=false;
        e.endpoint.assign(endpoint,endpoint+bytes);e.mid.assign(mid,mid+midBytes);++registered_;
        if(badRoots_.size()>4096)for(auto i=badRoots_.begin();i!=badRoots_.end();)if(i->second+2<gen_)i=badRoots_.erase(i);else ++i;
        if(entries_.size()>8192)for(auto i=entries_.begin();i!=entries_.end();)if(i->second.gen+2<gen_)i=entries_.erase(i);else ++i;
    }
    void forget(uint32_t address){entries_.erase(address);}
    // A model keeps one pose per draw: if any of its packets stays at the
    // endpoint this frame, none of them takes the halfway pose.
    void bad(uint32_t root){if(root)badRoots_[root]=gen_;}
    // Rigid parts of a model drawn through VU1 (eyes, weapons) must follow
    // its skinned body: true when this model kept its endpoint pose in the
    // frame just built (or the one being built).
    bool rootHeldBack(uint32_t root){
        std::lock_guard<std::mutex> lock(mutex_);
        // The frame on screen is the one built when its draw started.
        auto i=badRoots_.find(root);return i!=badRoots_.end()&&i->second==drawGen_.load(std::memory_order_acquire);
    }
    // Drop histories of models not drawn for a while (freed nodes, new scenes).
    void prune(){
        if(nodes_.size()<16384&&screens_.size()<1024)return;
        for(auto i=nodes_.begin();i!=nodes_.end();)if(i->second.gen+2<gen_)i=nodes_.erase(i);else ++i;
        for(auto i=screens_.begin();i!=screens_.end();)if(i->second.gen+2<gen_)i=screens_.erase(i);else ++i;
    }

    // sdfDeliverDrawStart: is the draw about to be kicked the first of a pair?
    void drawStart(bool first){firstDraw_.store(first,std::memory_order_release);if(first)drawGen_.store(gen_,std::memory_order_release);}
    // VIF1 chain gather (EE thread, inside sceDmaSend): substitute in place.
    void chain(uint32_t source,std::vector<uint8_t>& stream,size_t offset,uint32_t bytes){
        if(!firstDraw_.load(std::memory_order_acquire))return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto i=entries_.find(source&0x01ffffffu);if(i==entries_.end())return;
        auto& e=i->second;
        if(offset+bytes!=stream.size()){++stale_;return;}
        const uint8_t* data=stream.data()+offset;
        // Only the frame on screen (built in this generation or the one before) may
        // substitute: an older entry whose bytes happen to match would put a stale
        // in-between pose on a part (a packet the game kept from an earlier frame).
        if(e.gen+1<gen_){++stale_;entries_.erase(i);return;}
        if(e.used||e.endpoint.size()!=bytes||std::memcmp(e.endpoint.data(),data,bytes)!=0){++stale_;if(debugChain)std::fprintf(stderr,"[skin-debug] chain-stale root=%06x used=%d size=%zu/%u\n",e.root,int(e.used),e.endpoint.size(),bytes);return;}
        if(auto r=badRoots_.find(e.root);r!=badRoots_.end()&&r->second==e.gen){++partial_;if(debugChain)std::fprintf(stderr,"[skin-debug] chain-held root=%06x\n",e.root);return;}
        if(debugChain)std::fprintf(stderr,"[skin-debug] chain-sub root=%06x at=%06x entry-gen=%llu gen=%llu bytes=%u mid=%zu\n",e.root,source&0x01ffffffu,(unsigned long long)e.gen,(unsigned long long)gen_,bytes,e.mid.size());
        stream.resize(offset);stream.insert(stream.end(),e.mid.begin(),e.mid.end());e.used=true;++substituted_;
    }
    ~Interpolator(){
        if(configured_)std::fprintf(stderr,"[skin-interpolation] budget frames=%llu slow=%llu skipped-model-calls=%llu final-budget-us=%.0f camera-matched=%llu camera-fallback=%llu stale-node-slots=%llu\n",(unsigned long long)budget.frames,(unsigned long long)budget.slowFrames,(unsigned long long)budget.skippedModels,budget.budgetUs,(unsigned long long)cameraMatched_,(unsigned long long)cameraFallback_,(unsigned long long)staleNodes_);
        if(configured_)std::fprintf(stderr,"[skin-interpolation] packets=%llu halfway=%llu resized=%llu size-mismatch=%llu no-history=%llu rejected=%llu substituted=%llu stale=%llu whole-model-fallback=%llu\n",
            (unsigned long long)generated_,(unsigned long long)registered_,(unsigned long long)resized_,(unsigned long long)sizeMismatch_,(unsigned long long)noHistory_,
            (unsigned long long)rejected_,(unsigned long long)substituted_,(unsigned long long)stale_,(unsigned long long)partial_);
    }
};
inline Interpolator& interpolator(){static Interpolator value;return value;}
} // namespace magatama::skin
