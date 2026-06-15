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
