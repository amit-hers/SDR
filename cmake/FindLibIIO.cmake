find_path(LIBIIO_INCLUDE_DIR iio.h
    PATHS /usr/include /usr/local/include)

find_library(LIBIIO_LIBRARY NAMES iio
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibIIO DEFAULT_MSG
    LIBIIO_LIBRARY LIBIIO_INCLUDE_DIR)

if(LibIIO_FOUND)
    set(LIBIIO_LIBRARIES    ${LIBIIO_LIBRARY})
    set(LIBIIO_INCLUDE_DIRS ${LIBIIO_INCLUDE_DIR})
    if(NOT TARGET LibIIO::LibIIO)
        add_library(LibIIO::LibIIO UNKNOWN IMPORTED)
        set_target_properties(LibIIO::LibIIO PROPERTIES
            IMPORTED_LOCATION             "${LIBIIO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBIIO_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(LIBIIO_INCLUDE_DIR LIBIIO_LIBRARY)
