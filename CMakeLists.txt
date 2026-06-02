cmake_minimum_required(VERSION 3.16)
project(mme_simulator CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(mme_sim
    src/main.cpp
    src/enb/enb_node.cpp
    src/mme/mme_node.cpp
    src/hss/hss_node.cpp
    src/sgw/sgw_node.cpp
    src/pgw/pgw_node.cpp
    src/pcrf/pcrf_node.cpp
)
target_include_directories(mme_sim PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(mme_sim Threads::Threads)

# IMS / VoLTE simulator (separate binary)
add_executable(mme_ims
    src/ims_main.cpp
    src/ims/pcscf_node.cpp
    src/ims/scscf_node.cpp
    src/ims/ims_hss.cpp
)
target_include_directories(mme_ims PRIVATE src)
target_link_libraries(mme_ims Threads::Threads)
