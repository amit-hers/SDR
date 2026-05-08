cmake_minimum_required(VERSION 3.20)

project(sdr-datalink
    VERSION 1.0.0
    DESCRIPTION "SDR transparent wireless bridge for ADALM-PlutoSDR"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Build type default ───────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compiler warnings ────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
)


# ── Dependencies ─────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/FindLibIIO.cmake)
include(cmake/FindLiquidDSP.cmake)

if(NOT LibIIO_FOUND)
    message(FATAL_ERROR "libiio not found. Install: sudo apt install libiio-dev")
endif()
if(NOT LiquidDSP_FOUND)
    message(FATAL_ERROR "liquid-dsp not found. Install: sudo apt install libliquid-dev")
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ── Subdirectories ───────────────────────────────────────────────────────────
add_subdirectory(src/core)
add_subdirectory(src/daemon)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install monitor + config ─────────────────────────────────────────────────
install(DIRECTORY src/monitor/
    DESTINATION share/sdr-datalink/monitor
    FILES_MATCHING PATTERN "*.py" PATTERN "*.html" PATTERN "*.js" PATTERN "*.css")
install(FILES config.json
    DESTINATION share/sdr-datalink)

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "SDR Datalink ${PROJECT_VERSION}")
message(STATUS "  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  libiio      : ${LIBIIO_LIBRARY}")
message(STATUS "  liquid-dsp  : ${LIQUID_LIBRARY}")
message(STATUS "  OpenSSL     : ${OPENSSL_VERSION}")
message(STATUS "  Tests       : ${BUILD_TESTS}")
message(STATUS "")
message(STATUS "Build commands:")
message(STATUS "  cmake -B build -DCMAKE_BUILD_TYPE=Release")
message(STATUS "  cmake --build build -j\$(nproc)")
message(STATUS "  ctest --test-dir build --output-on-failure")
message(STATUS "")
