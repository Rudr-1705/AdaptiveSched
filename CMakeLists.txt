cmake_minimum_required(VERSION 3.16)
project(AdaptiveSchedPP VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Language & compiler settings
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    else()
        add_compile_options(-O2)
    endif()
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Od /Zi)
    else()
        add_compile_options(/O2)
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Threads (required on all platforms)
# ─────────────────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Source files
# ─────────────────────────────────────────────────────────────────────────────
set(CORE_SOURCES
    src/core/clock.cpp
    src/core/process.cpp
    src/core/ready_queue.cpp
)

set(SCHEDULER_SOURCES
    src/scheduler/scheduler.cpp
    src/scheduler/fcfs.cpp
    src/scheduler/sjf.cpp
    src/scheduler/rr.cpp
    src/scheduler/priority.cpp
    src/scheduler/mlfq.cpp
    src/scheduler/adaptive.cpp
)

set(EXECUTION_SOURCES
    src/execution/cpu.cpp
)

set(METRICS_SOURCES
    src/metrics/statistics.cpp
)

set(SIMULATION_SOURCES
    src/simulation/workload.cpp
    src/simulation/process_generator.cpp
    src/simulation/simulation_controller.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
# Static library: adaptive_sched_lib
# ─────────────────────────────────────────────────────────────────────────────
add_library(adaptive_sched_lib STATIC
    ${CORE_SOURCES}
    ${SCHEDULER_SOURCES}
    ${EXECUTION_SOURCES}
    ${METRICS_SOURCES}
    ${SIMULATION_SOURCES}
)

target_include_directories(adaptive_sched_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(adaptive_sched_lib PUBLIC
    Threads::Threads
)

# ─────────────────────────────────────────────────────────────────────────────
# Executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(adaptive_sched main.cpp)

target_link_libraries(adaptive_sched PRIVATE
    adaptive_sched_lib
)

# ─────────────────────────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS adaptive_sched
    RUNTIME DESTINATION bin
)

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard  : C++${CMAKE_CXX_STANDARD}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
