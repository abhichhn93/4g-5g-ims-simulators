cmake_minimum_required(VERSION 3.16)
project(g5_simulator CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ── gNB (+ simulated UE) — interactive CLI, N2 client ─────────
add_executable(gnb_sim src/gnb/gnb_main.cpp)
target_include_directories(gnb_sim PRIVATE src)
target_link_libraries(gnb_sim Threads::Threads)

# ── UDM — SBI (HTTP/JSON) server ─────────────────────────────
add_executable(udm_sim src/udm/udm_main.cpp)
target_include_directories(udm_sim PRIVATE src)
target_link_libraries(udm_sim Threads::Threads)

# ── NRF — SBI (HTTP/JSON) server, NF register/discover ───────
add_executable(nrf_sim src/nrf/nrf_main.cpp)
target_include_directories(nrf_sim PRIVATE src)
target_link_libraries(nrf_sim Threads::Threads)

# ── AMF — N2 server + SBI client to UDM, writes 5g_capture.pcap ─
add_executable(amf_sim
    src/amf/amf_main.cpp
    src/common/pcap_writer.cpp
)
target_include_directories(amf_sim PRIVATE src)
target_link_libraries(amf_sim Threads::Threads)

# ── SMF — SBI HTTP server :29502, N4 PFCP client to UPF ──────
add_executable(smf_sim
    src/smf/smf_main.cpp
    src/common/pcap_writer.cpp
)
target_include_directories(smf_sim PRIVATE src)
target_link_libraries(smf_sim Threads::Threads)

# ── UPF — N4 PFCP UDP server :8805, programs dataplane ───────
add_executable(upf_sim
    src/upf/upf_main.cpp
    src/common/pcap_writer.cpp
)
target_include_directories(upf_sim PRIVATE src)
target_link_libraries(upf_sim Threads::Threads)
