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
