add_executable(sdr-tests
    main.cpp
    test_framing.cpp
    test_fec.cpp
    test_modem.cpp
    test_crypto.cpp
    test_dsp.cpp
    test_arq.cpp
    test_splitmod.cpp
    test_aggregate.cpp
    test_scramble.cpp
    test_telemetry.cpp
    test_offsets.cpp
)

target_include_directories(sdr-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

# The suites assert their expectations, and a Release build defines NDEBUG --
# which compiles every assert away and makes the whole suite pass vacuously.
# Keep assertions live here regardless of build type.
target_compile_options(sdr-tests PRIVATE -UNDEBUG)

target_link_libraries(sdr-tests PRIVATE sdr_core)

add_test(NAME framing COMMAND sdr-tests framing)
add_test(NAME fec     COMMAND sdr-tests fec)
add_test(NAME modem   COMMAND sdr-tests modem)
add_test(NAME crypto  COMMAND sdr-tests crypto)
add_test(NAME dsp     COMMAND sdr-tests dsp)
add_test(NAME arq     COMMAND sdr-tests arq)
add_test(NAME splitmod COMMAND sdr-tests splitmod)
add_test(NAME aggregate COMMAND sdr-tests aggregate)
add_test(NAME scramble COMMAND sdr-tests scramble)
add_test(NAME telemetry COMMAND sdr-tests telemetry)
add_test(NAME offsets COMMAND sdr-tests offsets)

# Hostile-input harness for the decoders that take data from outside. Separate
# executable, not a suite in sdr-tests, so it can be run for a long time under
# sanitizers without slowing the normal test cycle.
add_executable(sdr-fuzz fuzz_decoders.cpp)
target_include_directories(sdr-fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(sdr-fuzz PRIVATE -UNDEBUG)
target_link_libraries(sdr-fuzz PRIVATE sdr_core)

# A short run in the normal cycle; CI runs it longer under sanitizers.
add_test(NAME fuzz COMMAND sdr-fuzz 2000)

# sdrctl writes the operator's config file, so its edits are tested against the
# real one: a defect here corrupts data rather than misreporting it.
add_test(NAME sdrctl_config
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/sdrctl_config_test.sh
                 $<TARGET_FILE:sdrctl> ${PROJECT_SOURCE_DIR}/config.json)

# A present-but-unparseable config value must be reported, not silently
# replaced by a default or by a partial parse.
add_test(NAME config_strict
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/config_strict_test.sh
                 $<TARGET_FILE:sdr-datalink> ${PROJECT_SOURCE_DIR}/config.json)

# `net set` writes the radio's boot configuration; a value that can inject lines
# there can change the board's network identity.
add_test(NAME sdrctl_net
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/sdrctl_net_test.sh $<TARGET_FILE:sdrctl>)

# The wire format must not depend on the machine that encoded it: golden vectors
# catch accidental changes, and a cross-build to ARMv7 catches assumptions about
# type widths or char signedness. Skips the ARM half where the toolchain is absent.
add_test(NAME wire_conformance
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/arm/wire_conformance_test.sh ${PROJECT_SOURCE_DIR})

# The bridge carries IP or it does not; unit tests exercise the codec, not the
# process. Two instances with real TUNs in separate namespaces, transmit FIFO of
# one wired to the receive FIFO of the other. Needs root for TUNSETIFF and skips
# cleanly without it, so an ordinary ctest run is unaffected.
add_test(NAME bridge_e2e
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bridge_e2e_test.sh ${PROJECT_SOURCE_DIR})
