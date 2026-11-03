#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace magatama::pose_trace {
struct Record {
    uint64_t host_ns=0,field=0;
    uint32_t kind=0,kernel=0,request=0,node=0,root=0,shape=0,packet=0,slot=0,mode=0,destination=0,bytes=0,flags=0;
    std::array<uint8_t,208> data{};
};
static_assert(sizeof(Record)==272);
class Trace {
    static constexpr size_t Cap=16384;
    std::mutex mutex_;
    std::string path_;
    std::vector<Record> rows_;
    std::map<std::pair<uint64_t,uint32_t>,uint32_t> counts_;
    Record phase_{}; // VU owner only, ordered before that draw's DMA work
public:
    static bool window(uint64_t field) {
        // MAGATAMA_RENDER_POSE_WINDOW=<first>-<end> (end exclusive) replaces
        // the three default diagnostic windows.
        static const std::pair<uint64_t,uint64_t> custom=[]{
            const char* v=std::getenv("MAGATAMA_RENDER_POSE_WINDOW");
            unsigned long long a=0,b=0;
            if(v&&std::sscanf(v,"%llu-%llu",&a,&b)==2&&b>a)return std::pair<uint64_t,uint64_t>{a,b};
            return std::pair<uint64_t,uint64_t>{0,0};}();
        if(custom.second)return field>=custom.first&&field<custom.second;
        return (field>=1200&&field<1240)||(field>=2100&&field<2140)||(field>=3300&&field<3340);
    }
    void open(const char* path) { if(path&&*path){path_=path;rows_.reserve(Cap);} }
    bool enabled() const{return !path_.empty();}
    void add(Record r) {
        if(!enabled() || !window(r.field))return;
        const std::lock_guard<std::mutex> lock(mutex_);
        if(rows_.size()>=Cap)return;
        const uint64_t key=r.kind=='U'?r.field:r.kernel;
        const uint32_t limit=r.kind=='P'?96u:r.kind=='U'?64u:8u;
        if(++counts_[{key,r.kind}]>limit)return;
        r.host_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        rows_.push_back(r);
    }
    void begin(Record r){phase_=r;add(r);}
    void unpack(uint32_t source,uint32_t command,uint32_t dest,const uint8_t* data,uint32_t bytes) {
        if(!enabled() || !window(phase_.field) || !data || bytes>208 ||
            ((command&0x7ffffc00u)!=0x6c07c000u && (command&0x7fffffffu)!=0x6c0bc000u))return;
        auto r=phase_;r.kind='U';r.packet=source;r.destination=dest;r.bytes=bytes;
        std::memcpy(r.data.data(),data,bytes);add(r);
    }
    void close() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if(path_.empty())return;
        FILE* f=std::fopen(path_.c_str(),"wb");
        if(!f){std::fprintf(stderr,"[render-pose-trace] cannot save %s\n",path_.c_str());return;}
        const uint32_t header[]={0x31535052u,1u,uint32_t(sizeof(Record)),uint32_t(rows_.size())};
        bool ok=std::fwrite(header,sizeof(header),1,f)==1;
        if(!rows_.empty())ok=std::fwrite(rows_.data(),sizeof(Record),rows_.size(),f)==rows_.size()&&ok;
        ok=std::fclose(f)==0&&ok;
        std::fprintf(stderr,"[render-pose-trace] rows=%zu saved=%d cap=%zu\n",rows_.size(),int(ok),Cap);
        path_.clear();
    }
    ~Trace(){close();}
};
inline Trace& trace(){static Trace t;return t;}
} // namespace magatama::pose_trace
