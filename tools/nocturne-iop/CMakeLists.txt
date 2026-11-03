cmake_minimum_required(VERSION 3.20)
project(magatama_native_iop LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(core "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dq8-iop/lle")
add_library(magatama_dq8_iop STATIC
    "${core}/cpu.cpp" "${core}/irx.cpp" "${core}/iop.cpp"
    "${core}/kernel.cpp" "${core}/spu2.cpp")
target_include_directories(magatama_dq8_iop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dq8-iop")
add_executable(nocturne-iop-probe probe.cpp)
target_link_libraries(nocturne-iop-probe PRIVATE magatama_dq8_iop)
