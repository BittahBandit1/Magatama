#pragma once
#include <cstdint>
#include <functional>
namespace rrv::ipu_worker_timing {
using Schedule=std::function<void(uint64_t,std::function<void()>)>;
void configure(Schedule schedule,std::function<void(unsigned)> interrupt);
}
