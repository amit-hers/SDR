find_path(LIQUID_INCLUDE_DIR liquid/liquid.h
    PATHS /usr/include /usr/local/include)

find_library(LIQUID_LIBRARY NAMES liquid
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LiquidDSP DEFAULT_MSG
    LIQUID_LIBRARY LIQUID_INCLUDE_DIR)

if(LiquidDSP_FOUND)
    set(LIQUID_LIBRARIES    ${LIQUID_LIBRARY})
    set(LIQUID_INCLUDE_DIRS ${LIQUID_INCLUDE_DIR})
    if(NOT TARGET LiquidDSP::LiquidDSP)
        add_library(LiquidDSP::LiquidDSP UNKNOWN IMPORTED)
        set_target_properties(LiquidDSP::LiquidDSP PROPERTIES
            IMPORTED_LOCATION             "${LIQUID_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIQUID_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(LIQUID_INCLUDE_DIR LIQUID_LIBRARY)
