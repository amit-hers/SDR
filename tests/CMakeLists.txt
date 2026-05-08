add_executable(sdr-tests
    main.cpp
    test_framing.cpp
    test_fec.cpp
    test_modem.cpp
    test_crypto.cpp
    test_dsp.cpp
)

target_include_directories(sdr-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(sdr-tests PRIVATE sdr_core)

add_test(NAME framing COMMAND sdr-tests framing)
add_test(NAME fec     COMMAND sdr-tests fec)
add_test(NAME modem   COMMAND sdr-tests modem)
add_test(NAME crypto  COMMAND sdr-tests crypto)
add_test(NAME dsp     COMMAND sdr-tests dsp)
