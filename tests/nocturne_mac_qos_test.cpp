#include "rrv_mac_qos.h"
#include <thread>
int main(int argc,char**){
#if defined(__APPLE__)
 const bool enabled=argc>1; if(enabled)setenv("MAGATAMA_MAC_QOS","1",1);else unsetenv("MAGATAMA_MAC_QOS");
 bool ok=false;std::thread worker([&]{
  if(pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0)!=0)return;
  rrv::macInteractiveQos();rrv::macInteractiveQos();
  qos_class_t actual=QOS_CLASS_UNSPECIFIED;
  if(pthread_get_qos_class_np(pthread_self(),&actual,nullptr)!=0)return;
  ok=actual==(enabled?QOS_CLASS_USER_INTERACTIVE:QOS_CLASS_UTILITY);
 });worker.join();if(!ok)return 1;
 std::printf("PASS macOS actual pthread QoS %s; default unchanged, idempotent when enabled\n",enabled?"on":"off");
#endif
}
