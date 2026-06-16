cmake_minimum_required(VERSION 3.16)
project(mme_simulator CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Platform-specific settings ────────────────────────────────
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
    set(PLATFORM_LIBS ws2_32)   # Winsock2 on Windows
else()
    set(PLATFORM_LIBS "")
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ── Common sources (shared by both binaries) ──────────────────
set(COMMON_SRC
    src/common/pcap_writer.cpp
    src/common/nas_eps.cpp
    src/common/s1ap_codec.cpp
)

# ── 4G EPC simulator ─────────────────────────────────────────
add_executable(mme_sim
    src/main.cpp
    src/enb/enb_node.cpp
    src/mme/mme_node.cpp
    src/hss/hss_node.cpp
    src/sgw/sgw_node.cpp
    src/pgw/pgw_node.cpp
    src/pcrf/pcrf_node.cpp
    ${COMMON_SRC}
)
target_include_directories(mme_sim PRIVATE src)
target_link_libraries(mme_sim Threads::Threads ${PLATFORM_LIBS})

# IMS/VoLTE (mme_ims, ims_server, ue_sim) moved to ../ims-simulator/

# ── Scenario Runner: YAML-driven CLI automation ───────────────
add_executable(scenario_runner src/scenario_runner.cpp)
target_include_directories(scenario_runner PRIVATE src)
target_link_libraries(scenario_runner Threads::Threads ${PLATFORM_LIBS})

# ── O-RAN E2 Interface Stub ───────────────────────────────────
# Two standalone binaries demonstrating O-RAN xApp ↔ eNB E2 flow:
#   ./e2_agent  (start first — eNB E2 termination point, port 36421)
#   ./xapp_sim  (connects to e2_agent, sends RIC Control Request)
add_executable(e2_agent
    src/o_ran/e2_agent.cpp
    src/common/pcap_writer.cpp
)
target_include_directories(e2_agent PRIVATE src)
target_link_libraries(e2_agent Threads::Threads ${PLATFORM_LIBS})

add_executable(xapp_sim
    src/o_ran/xapp_main.cpp
    src/common/pcap_writer.cpp
)
target_include_directories(xapp_sim PRIVATE src)
target_link_libraries(xapp_sim Threads::Threads ${PLATFORM_LIBS})
