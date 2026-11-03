#pragma once
#include <cstdlib>
#include <cstdio>
#include <cstring>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
namespace rrv {
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
#endif
}
}
