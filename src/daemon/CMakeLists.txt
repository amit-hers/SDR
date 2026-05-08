add_executable(sdr-datalink
    main.cpp
    Config.cpp
    Controller.cpp
    StatsExporter.cpp
    modes/BridgeMode.cpp
    modes/MeshMode.cpp
    modes/P2PMode.cpp
    modes/ScanMode.cpp
)

target_include_directories(sdr-datalink
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sdr-datalink PRIVATE sdr_core)

install(TARGETS sdr-datalink DESTINATION bin)
