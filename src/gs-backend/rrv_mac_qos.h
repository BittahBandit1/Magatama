#pragma once
#include <cstdlib>
#include <cstdio>
#include <cstring>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#include <string>
extern "C" void rrv_set_field_work_hook(void (*hook)(long long work_ns)); // gate3 real-time pacer
#endif
namespace rrv {
#if defined(__ANDROID__)
// Android performance hints (ADPF, Android 13+; MAGATAMA_PERF_HINT=1, the launcher's default). The game, VU1 and GS
// threads take turns within a field (the game waits on the VU1/GS pipeline at readbacks), so none of them looks busy
// to the CPU governor, which then lowers the clocks and makes every field longer. A hint session per thread, told
// each field how long the game took against a 16.7 ms target, lets the system raise the clocks when fields run long.
struct AndroidPerfHint {
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using Report = int (*)(void*, int64_t);
    GetManager getManager = nullptr; CreateSession createSession = nullptr; Report report = nullptr;
    void* manager = nullptr;
    std::mutex mutex; std::vector<void*> sessions;
    static AndroidPerfHint& get() { static AndroidPerfHint hint; return hint; }
    AndroidPerfHint() {
        const char* p = std::getenv("MAGATAMA_PERF_HINT");
        if (!(p && std::strcmp(p, "1") == 0)) return;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return;
        getManager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        createSession = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        report = reinterpret_cast<Report>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        manager = getManager && createSession && report ? getManager() : nullptr;
        std::fprintf(stderr, "[perf-hint] ADPF %s\n", manager ? "available" : "not available (Android 13+ only)");
        if (manager) rrv_set_field_work_hook(&AndroidPerfHint::field);
    }
    void addThread(const char* name) {
        if (!manager) return;
        const int32_t tid = static_cast<int32_t>(gettid());
        void* session = createSession(manager, &tid, 1, 16683333);
        std::fprintf(stderr, "[perf-hint] thread=%s session=%s\n", name, session ? "ok" : "refused");
        if (!session) return;
        std::lock_guard<std::mutex> lock(mutex); sessions.push_back(session);
    }
    static void field(long long work_ns) {
        auto& h = get();
        if (work_ns <= 0 || work_ns > 1000000000ll) return; // pauses and loads say nothing about a field
        std::lock_guard<std::mutex> lock(h.mutex);
        for (void* session : h.sessions) h.report(session, work_ns);
    }
};
#endif
// Optional scheduling experiment, never a CPU affinity/real-time policy.
// Call only on game/GS/VU worker entry, never on CoreAudio's I/O thread.
inline void macInteractiveQos() {
#if defined(__APPLE__)
    static const bool enabled=[] {const char* p=std::getenv("MAGATAMA_MAC_QOS");return p&&std::strcmp(p,"1")==0;}();
    if(!enabled)return;
    static thread_local bool attempted=false;
    if(attempted)return;attempted=true;
    qos_class_t current=QOS_CLASS_UNSPECIFIED;
    if(pthread_get_qos_class_np(pthread_self(),&current,nullptr)==0 && current==QOS_CLASS_USER_INTERACTIVE)return;
    const int error=pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    char name[64]{};pthread_getname_np(pthread_self(),name,sizeof(name));
    std::fprintf(stderr,"[mac-qos] thread=%s user-interactive result=%d\n",name,error);
#elif defined(__ANDROID__)
    // Android (MAGATAMA_PERF_CORES=1, the launcher's default): keep the game, GS and VU threads off the efficiency
    // cores and raise their priority, so the scheduler cannot park a frame's work on a slow core. Measured on a Retroid
    // Pocket Nova: 60 FPS interpolation missed frames at scene changes. The set is every core except the slowest
    // cluster, and that one only when it is clearly an efficiency cluster (top clock under 70% of the fastest core's):
    //   Snapdragon 8 Gen 2 (Nova, Odin 2, Thor, RP6): 3.2 + 2.8 GHz cores, not the 2.0 GHz ones (5 cores)
    //   Snapdragon 8 Elite (Odin 3): all 8 (4.3 and 3.5 GHz: no efficiency cluster)
    //   Snapdragon 865 (RP5, Mini, Flip 2): 2.84 + 2.42 GHz, not the 1.8 GHz ones (4 cores)
    // Fewer than 3 cores left (an unusual layout): no affinity, only the priority.
    static const bool enabled=[] {const char* p=std::getenv("MAGATAMA_PERF_CORES");return p&&std::strcmp(p,"1")==0;}();
    if(!enabled)return;
    static thread_local bool attempted=false;
    if(attempted)return;attempted=true;
    static const cpu_set_t perf=[] {
        cpu_set_t set;CPU_ZERO(&set);
        long freq[CPU_SETSIZE]{};long top=0;
        const long count=sysconf(_SC_NPROCESSORS_CONF);
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu) {
            char path[96];std::snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq",cpu);
            if(FILE* f=std::fopen(path,"r")) {if(std::fscanf(f,"%ld",&freq[cpu])!=1)freq[cpu]=0;std::fclose(f);}
            if(freq[cpu]>top)top=freq[cpu];
        }
        long slowest=top;
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu)
            if(freq[cpu]>0&&freq[cpu]<slowest)slowest=freq[cpu];
        const bool efficiency=top>0&&slowest*100<top*70;
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu)
            if(freq[cpu]>0&&!(efficiency&&freq[cpu]==slowest))CPU_SET(cpu,&set);
        if(CPU_COUNT(&set)<3)CPU_ZERO(&set);
        char list[128]="";
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu)
            if(CPU_ISSET(cpu,&set)){char item[16];std::snprintf(item,sizeof item,"%s%ld",list[0]?",":"",cpu);std::strncat(list,item,sizeof list-std::strlen(list)-1);}
        std::fprintf(stderr,"[perf-cores] performance cores: %s (top %.2f GHz)\n",list[0]?list:"none",top/1e6);
        return set;
    }();
    // MAGATAMA_PRIME_THREADS (comma-separated thread names, default none): those threads run only on the fastest
    // cores (the Snapdragon 8 Gen 2's single 3.2 GHz core), and with MAGATAMA_PRIME_EXCLUSIVE=1 every other thread
    // here keeps off them. In heavy scenes the game thread and rrv-vu1 take turns each frame, so the slower of the
    // two on a 2.8 GHz core sets the frame rate.
    static const cpu_set_t prime=[] {
        cpu_set_t set;CPU_ZERO(&set);long freq[CPU_SETSIZE]{};long top=0;
        const long count=sysconf(_SC_NPROCESSORS_CONF);
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu) {
            char path[96];std::snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq",cpu);
            if(FILE* f=std::fopen(path,"r")) {if(std::fscanf(f,"%ld",&freq[cpu])!=1)freq[cpu]=0;std::fclose(f);}
            if(freq[cpu]>top)top=freq[cpu];
        }
        for(long cpu=0;cpu<count&&cpu<CPU_SETSIZE;++cpu)if(top>0&&freq[cpu]==top)CPU_SET(cpu,&set);
        return set;
    }();
    char name[64]{};pthread_getname_np(pthread_self(),name,sizeof(name));
    cpu_set_t chosen=perf;
    {
        const char* list=std::getenv("MAGATAMA_PRIME_THREADS");
        bool listed=false;
        if(list&&*list) {
            const std::string all=std::string(",")+list+",";
            listed=all.find(std::string(",")+name+",")!=std::string::npos;
        }
        const char* exclusive=std::getenv("MAGATAMA_PRIME_EXCLUSIVE");
        if(listed&&CPU_COUNT(&prime))chosen=prime;
        else if(list&&*list&&exclusive&&exclusive[0]=='1'&&CPU_COUNT(&perf)>CPU_COUNT(&prime)+1) {
            for(int cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&prime))CPU_CLR(cpu,&chosen);
        }
        if(listed)std::fprintf(stderr,"[perf-cores] thread=%s on the fastest cores only\n",name);
    }
    const int affinity=CPU_COUNT(&chosen)?sched_setaffinity(0,sizeof chosen,&chosen):-1;
    const int priority=setpriority(PRIO_PROCESS,0,-10);
    std::fprintf(stderr,"[perf-cores] thread=%s affinity=%s nice-10=%s\n",name,affinity==0?"ok":"failed",
                 priority==0?"ok":"failed");
    AndroidPerfHint::get().addThread(name);
#endif
}
}
