cmake_minimum_required(VERSION 3.16)
project(ims_simulator CXX)
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

# ── Common sources (shared by all binaries) ────────────────────
set(COMMON_SRC
    src/common/pcap_writer.cpp
)

# ── IMS single-process demo (legacy, hardcoded narration) ──────
add_executable(mme_ims
    src/ims_main.cpp
    src/ims/pcscf_node.cpp
    src/ims/scscf_node.cpp
    src/ims/ims_hss.cpp
    ${COMMON_SRC}
)
target_include_directories(mme_ims PRIVATE src)
target_link_libraries(mme_ims Threads::Threads ${PLATFORM_LIBS})

# ── IMS SERVER — multi-terminal mode (P-CSCF/S-CSCF/HSS as threads) ──
add_executable(ims_server
    src/ims_server_main.cpp
    src/ims/pcscf_node.cpp
    src/ims/scscf_node.cpp
    src/ims/ims_hss.cpp
    ${COMMON_SRC}
)
target_include_directories(ims_server PRIVATE src)
target_link_libraries(ims_server Threads::Threads ${PLATFORM_LIBS})

# ── UE SIMULATOR — ./ue_sim A|B|C ───────────────────────────────
add_executable(ue_sim
    src/ue_sim.cpp
    ${COMMON_SRC}
)
target_include_directories(ue_sim PRIVATE src)
target_link_libraries(ue_sim Threads::Threads ${PLATFORM_LIBS})

# ── PERFORMANCE SCENARIOS — 5 interview prep scenarios ──────────
add_executable(perf_scenarios
    src/perf_scenarios_main.cpp
    src/perf_scenarios.cpp
)
target_include_directories(perf_scenarios PRIVATE src)
target_link_libraries(perf_scenarios Threads::Threads ${PLATFORM_LIBS})
