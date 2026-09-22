cmake_minimum_required(VERSION 3.10)
project(hpsdr-recorder)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

find_package(Threads REQUIRED)
find_library(M_LIB m)

set(SOURCES
    hpsdr-recorder.c
    config.c
    hpsdr-protocol2.c
    buffers.c
    streaming.c
    output.c
    stats.c
)

add_executable(hpsdr-recorder ${SOURCES})
target_link_libraries(hpsdr-recorder Threads::Threads ${M_LIB})
