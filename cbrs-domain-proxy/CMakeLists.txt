cmake_minimum_required(VERSION 3.16)
project(cbrs_domain_proxy CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ── SAS Stub — simulated Spectrum Access System server (port 8800) ──
add_executable(sas_stub src/sas/sas_stub.cpp)
target_include_directories(sas_stub PRIVATE src)
target_link_libraries(sas_stub Threads::Threads)

# ── Domain Proxy — bridges CBSDs and SAS (port 8700 ↔ 8800) ────────
add_executable(domain_proxy src/proxy/domain_proxy.cpp)
target_include_directories(domain_proxy PRIVATE src)
target_link_libraries(domain_proxy Threads::Threads)

# ── CBSD Agent — simulated radio device (connects to DP on 8700) ────
add_executable(cbsd_agent src/cbsd/cbsd_agent.cpp)
target_include_directories(cbsd_agent PRIVATE src)
target_link_libraries(cbsd_agent Threads::Threads)
