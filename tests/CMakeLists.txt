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

# Hostile-input harness for the decoders that take data from outside. Separate
# executable, not a suite in sdr-tests, so it can be run for a long time under
# sanitizers without slowing the normal test cycle.
add_executable(sdr-fuzz fuzz_decoders.cpp)
target_include_directories(sdr-fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(sdr-fuzz PRIVATE -UNDEBUG)
target_link_libraries(sdr-fuzz PRIVATE sdr_core)

# A short run in the normal cycle; CI runs it longer under sanitizers.
add_test(NAME fuzz COMMAND sdr-fuzz 2000)
