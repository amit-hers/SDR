# sdr-live-stats: read real IQ from the radio and write the stats JSON, with no
# TAP device and therefore no root. That makes it the diagnostic to reach for
# when the question is "is the radio producing sensible IQ at all", separately
# from whether the bridge works.
#
# It was in the tree but not in any CMakeLists, so it silently was not built and
# every reference to `build/live_stats` was stale.
add_executable(sdr-live-stats live_stats.cpp)

target_include_directories(sdr-live-stats
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sdr-live-stats PRIVATE sdr_core)

install(TARGETS sdr-live-stats DESTINATION bin)

# sdrctl: one place to read and change what the node is doing -- the config
# file, a running daemon, and the radio's own network settings.
add_executable(sdrctl sdrctl.cpp)
target_include_directories(sdrctl PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sdrctl PRIVATE sdr_core)
install(TARGETS sdrctl DESTINATION bin)

# sdr-agent: the on-board management daemon (authenticated, read-only HTTP).
# Plain C with no dependencies beyond libc, so the release cross-builds it
# DYNAMICALLY against the board's shared glibc: ~45 KiB, which fits the jffs2
# partition. Its static C++ predecessor was ~810 KiB and never persisted.
add_executable(sdr-agent ../../fpga/tools/sdr_agent.c)
install(TARGETS sdr-agent DESTINATION bin)
