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
