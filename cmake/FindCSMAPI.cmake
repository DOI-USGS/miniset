# FindCSMAPI.cmake - Find the CSMAPI library

# For WASM builds, configure to find static libraries in usgscsm/wasm
if(EMSCRIPTEN)
    # Override Emscripten's restrictive search mode
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)

    # Prefer static libraries for WASM
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

    # Where the WASM-built CSM API (libcsmapi.a + csm/ headers) lives. Portable:
    # take it from -DCSMAPI_WASM_ROOT=... or the CSMAPI_WASM_ROOT env var (CI sets
    # this to wherever it built csmapi for wasm), falling back to a local dev path.
    if(DEFINED CSMAPI_WASM_ROOT)
        set(_WASM_CSM_PATH "${CSMAPI_WASM_ROOT}")
    elseif(DEFINED ENV{CSMAPI_WASM_ROOT})
        set(_WASM_CSM_PATH "$ENV{CSMAPI_WASM_ROOT}")
    else()
        set(_WASM_CSM_PATH "/Users/krodriguez/repos/usgscsm/wasm/csm")
    endif()
endif()

find_path(CSMAPI_INCLUDE_DIR
    NAMES csm/csm.h csm/RasterGM.h
    PATHS
        ${_WASM_CSM_PATH}
        ${CSMAPI_ROOT}
        ${CSMAPI_ROOT}/include
        $ENV{CSMAPI_ROOT}
        $ENV{CSMAPI_ROOT}/include
        $ENV{CONDA_PREFIX}/include
        /usr/local/include
        /usr/include
)

find_library(CSMAPI_LIBRARY
    NAMES csmapi libcsmapi
    PATHS
        ${_WASM_CSM_PATH}
        ${CSMAPI_ROOT}/lib
        $ENV{CSMAPI_ROOT}/lib
        $ENV{CONDA_PREFIX}/lib
        /usr/local/lib
        /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CSMAPI
    REQUIRED_VARS CSMAPI_LIBRARY CSMAPI_INCLUDE_DIR
)

if(CSMAPI_FOUND AND NOT TARGET CSMAPI::csmapi)
    add_library(CSMAPI::csmapi UNKNOWN IMPORTED)
    set_target_properties(CSMAPI::csmapi PROPERTIES
        IMPORTED_LOCATION "${CSMAPI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CSMAPI_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(CSMAPI_INCLUDE_DIR CSMAPI_LIBRARY)
