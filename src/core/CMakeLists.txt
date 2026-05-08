add_library(sdr_core STATIC
    hardware/PlutoSDR.cpp
    modem/Modem.cpp
    modem/AdaptiveModem.cpp
    framing/Framer.cpp
    framing/Deframer.cpp
    dsp/RRCFilter.cpp
    dsp/AGC.cpp
    dsp/TimingSync.cpp
    dsp/CostasLoop.cpp
    dsp/FFTSpectrum.cpp
    fec/ReedSolomon.cpp
    crypto/AESCipher.cpp
    transport/TUNTAPDevice.cpp
    transport/UDPSocket.cpp
    stats/LinkStats.cpp
)

target_include_directories(sdr_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${LIBIIO_INCLUDE_DIRS}
        ${LIQUID_INCLUDE_DIRS}
)

target_link_libraries(sdr_core
    PUBLIC
        LibIIO::LibIIO
        LiquidDSP::LiquidDSP
        OpenSSL::Crypto
        Threads::Threads
)

target_compile_features(sdr_core PUBLIC cxx_std_17)
