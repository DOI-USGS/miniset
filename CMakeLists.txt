cmake_minimum_required(VERSION 3.15)
project(miniset VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(MINISET_BUILD_TESTS "Build tests" ON)
option(MINISET_ENABLE_OPENMP "Enable OpenMP parallelization" ON)

# Performance flags (skip for WASM builds)
if(NOT EMSCRIPTEN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native -O3 -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        add_compile_options(/W4 /O2 /arch:AVX2)
    endif()
endif()

# Find dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# StarDS is a git submodule (external/StarDS, git@github.com:DOI-USGS/StarDS.git).
# The header-only library lives at StarDS/include/stards.h within it; this dir is
# added to the include path for every target that compiles src/cnet/stards_io.cpp
# (native lib, native apps, WASM). Fail early with a clear message if the
# submodule wasn't initialized (git submodule update --init external/StarDS).
set(STARDS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/StarDS/StarDS/include)
if(NOT EXISTS ${STARDS_INCLUDE_DIR}/stards.h)
    message(FATAL_ERROR
        "StarDS submodule not found at ${STARDS_INCLUDE_DIR}/stards.h. "
        "Run: git submodule update --init --recursive external/StarDS")
endif()

message(STATUS "conda prefix: $ENV{CONDA_PREFIX}")
find_package(CSMAPI REQUIRED)

# For WASM, build USGSCSM from external folder with debug prints
# For native, use system USGSCSM
if(EMSCRIPTEN)
    message(STATUS "WASM build: Using local USGSCSM from external/usgscsm with debug prints")
    # Build USGSCSM but exclude from install to avoid export conflicts
    set(USGSCSM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/usgscsm EXCLUDE_FROM_ALL)
    # Create alias target to match find_package result
    add_library(USGSCSM::usgscsm ALIAS usgscsm)
    # Mark as IMPORTED to avoid install export errors
    set_target_properties(usgscsm PROPERTIES EXCLUDE_FROM_ALL TRUE)
else()
    find_package(USGSCSM REQUIRED)
endif()

# For WASM, GDAL/PROJ are built as external projects (see cmake/BuildWasmDependencies.cmake)
# For native, they're required system dependencies
if(NOT EMSCRIPTEN)
    find_package(GDAL REQUIRED)
    find_package(PROJ REQUIRED)
else()
    # WASM: Will be built and linked later via BuildWasmDependencies.cmake
    find_package(GDAL)
    find_package(PROJ)
endif()

if(MINISET_ENABLE_OPENMP)
    find_package(OpenMP)
endif()

# Library sources - Phases 1-6
set(MINISET_SOURCES
    src/core/math.cpp
    src/core/transforms.cpp
    src/surface/ellipsoid.cpp
    src/surface/dem.cpp
    src/surface/shape_model.cpp
    src/surface/intersection.cpp
    src/csm/csm_interface.cpp
    src/csm/camera_ops.cpp
    src/csm/triangulation.cpp
    src/csm/campt.cpp
    src/csm/campt_compute.cpp
    src/sensor/angles.cpp
    src/utils/coordinate_transforms.cpp
    # cnet: portable control-network core (protobuf .net read/write is pure C++,
    # so it compiles for both native and WASM). Parquet I/O is native-only and
    # added via target_sources below.
    src/cnet/net_protobuf.cpp
    src/cnet/control_net_io.cpp
    src/cnet/column_source.cpp
    # cnet: header-only Parquet reader (external/miniparquet.hpp) + ColumnBatch
    # bridge, and the lazy ControlNetReader (now mparq-backed, no GDAL). Portable —
    # compiled in both native and WASM (replaces GDAL/Arrow on read natively and
    # hyparquet in WASM).
    src/cnet/miniparquet_io.cpp
    src/cnet/control_net_reader.cpp
)

# Main library target
add_library(miniset SHARED ${MINISET_SOURCES})

target_include_directories(miniset
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
target_link_libraries(miniset PUBLIC CSMAPI::csmapi USGSCSM::usgscsm)
target_compile_definitions(miniset PUBLIC MINISET_HAS_CSMAPI MINISET_HAS_USGSCSM)

# cnet header-only Parquet reader (external/miniparquet.hpp): portable, so the
# include path + feature define apply to BOTH native and WASM. Snappy-compressed
# cnet files need zlib only for GZIP-compressed files; Snappy is vendored
# header-only. Enable GZIP support when zlib is available.
target_include_directories(miniset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_compile_definitions(miniset PRIVATE MINISET_CNET_HAS_MPARQUET)
find_package(ZLIB)
if(ZLIB_FOUND)
    target_compile_definitions(miniset PRIVATE MINIPARQUET_ENABLE_ZLIB)
    target_link_libraries(miniset PRIVATE ZLIB::ZLIB)
endif()

# GDAL and PROJ are always required, but linked differently for WASM vs native
if(NOT EMSCRIPTEN)
    # Native build: link found system libraries
    target_link_libraries(miniset PUBLIC GDAL::GDAL PROJ::proj)
    target_compile_definitions(miniset PUBLIC MINISET_HAS_GDAL MINISET_HAS_PROJ)

    # isisimport module (native only): reads PDS3/PDS4 and writes ISIS3/GTiff/COG
    # via GDAL. Requires the planetary drivers + json:ISIS3 metadata domain, which
    # are not present in the minimal WASM GDAL build, so it is excluded there.
    target_sources(miniset PRIVATE
        src/isisimport/import_image.cpp
        src/isisimport/input_label.cpp
        src/isisimport/translation_spec.cpp
        src/isisimport/transform_registry.cpp
        src/isisimport/dispatcher.cpp
        src/isisimport/output_writer.cpp
        src/isisimport/spec_paths.cpp
    )
    # Vendored header-only nlohmann/json for the isisimport translation engine.
    target_include_directories(miniset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
    # Build-tree fallback so `isisimport` finds specs before `make install`.
    target_compile_definitions(miniset PRIVATE
        MINISET_APPDATA_BUILD="${CMAKE_CURRENT_SOURCE_DIR}/appdata/import")

    # cnet Parquet no longer uses GDAL/Arrow at all: the header-only miniparquet
    # reader AND writer (in MINISET_SOURCES) handle both read and write in native
    # and WASM builds. The libgdal-arrow-parquet driver is no longer a dependency.

    # cnet StarDS I/O (native): header-only columnar store from the StarDS
    # submodule (${STARDS_INCLUDE_DIR}/stards.h) with block-level random access.
    # zlib is available in the native env, so enable GZIP (keeps get_slice partial
    # reads working); StarDS needs a thread lib.
    find_package(Threads REQUIRED)
    find_package(ZLIB)
    target_sources(miniset PRIVATE src/cnet/stards_io.cpp)
    target_include_directories(miniset PRIVATE ${STARDS_INCLUDE_DIR})
    target_compile_definitions(miniset PRIVATE MINISET_CNET_HAS_STARDS)
    target_link_libraries(miniset PRIVATE Threads::Threads)
    if(ZLIB_FOUND)
        target_compile_definitions(miniset PRIVATE ENABLE_ZLIB)
        target_link_libraries(miniset PRIVATE ZLIB::ZLIB)
    endif()
endif()

# Note: For WASM, GDAL/PROJ linking happens in the miniset_wasm target (see below)

if(MINISET_ENABLE_OPENMP AND OpenMP_CXX_FOUND)
    target_link_libraries(miniset PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(miniset PRIVATE MINISET_HAS_OPENMP)
endif()

# Tests
if(MINISET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Applications (skip for WASM - CLI tools don't make sense in browser).
#
# Paradigm: native apps use ISIS-agnostic, snake_case names. An app may expose
# ISIS-style compatibility aliases (same binary, different name) via ALIASES.
# See cmake/MinisetApps.cmake.
include(cmake/MinisetApps.cmake)

if(CSMAPI_FOUND AND NOT EMSCRIPTEN)
    miniset_add_app(campt
        SOURCES apps/campt.cpp
        LINK    miniset
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# image_import (native only): needs GDAL, not CSM. Also installed as the ISIS
# compatibility alias `isisimport`.
if(NOT EMSCRIPTEN)
    miniset_add_app(image_import
        SOURCES apps/image_import.cpp
        LINK    miniset
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/external
        ALIASES isisimport)

    # Bundle the import specs + dispatch table next to the install.
    install(DIRECTORY appdata/import/ DESTINATION share/miniset/import
            FILES_MATCHING PATTERN "*.json")

    # cnet_convert (native only): convert control nets between .net and .parquet.
    miniset_add_app(cnet_convert
        SOURCES apps/cnet_convert.cpp
        LINK    miniset
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/external)
endif()

# Installation (skip for WASM to avoid export conflicts with external libs)
if(NOT EMSCRIPTEN)
    install(TARGETS miniset
        EXPORT miniset-targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

if(NOT EMSCRIPTEN)
    install(DIRECTORY include/ DESTINATION include
            FILES_MATCHING PATTERN "*.hpp")

    install(EXPORT miniset-targets
        FILE miniset-config.cmake
        NAMESPACE miniset::
        DESTINATION lib/cmake/miniset
    )
endif()

# ==============================================================================
# WebAssembly Build Configuration
# ==============================================================================
if(EMSCRIPTEN)
    message(STATUS "Configuring for WebAssembly build with Emscripten")

    # Clear native flags that are incompatible with WASM
    set(CMAKE_CXX_FLAGS "")
    set(CMAKE_C_FLAGS "")
    set(CMAKE_EXE_LINKER_FLAGS "")
    set(CMAKE_SHARED_LINKER_FLAGS "")
    set(CMAKE_MODULE_LINKER_FLAGS "")

    # WASM-specific compile options
    option(MINISET_WASM_OPTIMIZE "Enable WASM optimizations" ON)
    option(MINISET_WASM_DEBUG "Enable WASM debug symbols" OFF)

    # Build WASM dependencies (PROJ and GDAL) automatically
    include(cmake/BuildWasmDependencies.cmake)

    # Create WASM executable target with bindings
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/miniset_bindings.cpp)
        # cnet WASM bindings: protobuf .net reading (pure C++) + Parquet reading
        # via the header-only miniparquet reader (in MINISET_SOURCES) — no
        # hyparquet, no Arrow. GDAL's Arrow/Parquet driver stays absent.
        add_executable(miniset_wasm ${MINISET_SOURCES}
            src/wasm/miniset_bindings.cpp
            src/cnet/wasm_bindings.cpp)

        # Copy properties from main library target
        target_include_directories(miniset_wasm
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/external
                ${STARDS_INCLUDE_DIR}
        )
        # Header-only Parquet reader + its GZIP path (emscripten bundles zlib).
        target_compile_definitions(miniset_wasm PRIVATE
            MINISET_CNET_HAS_MPARQUET MINIPARQUET_ENABLE_ZLIB)

        # StarDS reader in WASM: powers the docs hero banner (streams adjusted XYZ
        # points from a .stards net for the Three.js render). Header-only store;
        # ENABLE_ZLIB lets it read GZIP-compressed nets, and it runs single-
        # threaded (the bindings call setNumThreads(1)) since the WASM build has
        # no pthreads.
        #
        # ENABLE_CURL turns on StarDS's HTTP(/vsicurl) + S3(/vsis3) range-read
        # backend. Under emscripten there is no libcurl: the header uses the JS
        # global fetch() via EM_ASYNC_JS (guarded on __EMSCRIPTEN__), so read_at()
        # stays blocking and the build MUST link -sASYNCIFY (added to
        # WASM_LINK_FLAGS below) to suspend/rewind the Wasm stack across the
        # awaited fetch Promise. This lets the banner stream points straight from
        # the remote net over /vsicurl/ (no local copy). No -sFETCH: StarDS
        # deliberately avoids emscripten_fetch (its sync mode fails off a Worker).
        target_sources(miniset_wasm PRIVATE src/cnet/stards_io.cpp)
        target_compile_definitions(miniset_wasm PRIVATE MINISET_CNET_HAS_STARDS ENABLE_ZLIB ENABLE_CURL)

        # Compression support in WASM: miniset's own code (StarDS GZIP nets,
        # miniparquet GZIP pages) calls zlib directly. Link Emscripten's bundled
        # zlib port so deflate/inflate resolve in the module — without it those
        # symbols are only left "undefined" (allowed by the flags below) and any
        # compressed read would trap at runtime. -sUSE_ZLIB=1 is needed at both
        # compile (for <zlib.h>) and link time.
        #
        # -fexceptions must also be a COMPILE option (not just a link flag): the
        # cnet code uses try/catch (e.g. HeroPointsReader::open probes array keys
        # by catching array_length's throw). Compiled without it, catch clauses are
        # dropped and a C++ throw aborts/propagates instead of being caught. It is
        # repeated in WASM_LINK_FLAGS for the link step.
        target_compile_options(miniset_wasm PRIVATE
            "-sUSE_ZLIB=1" "-fexceptions" "-sDISABLE_EXCEPTION_CATCHING=0")

        # Link same dependencies
        if(CSMAPI_FOUND)
            target_link_libraries(miniset_wasm PUBLIC CSMAPI::csmapi)
            target_compile_definitions(miniset_wasm PUBLIC MINISET_HAS_CSMAPI)
        endif()

        # For WASM static linking, we need to force inclusion of the entire
        # USGSCSM library to ensure the plugin registration code runs
        # Note: USGSCSM::usgscsm alias is created either by find_package or add_subdirectory
        target_link_libraries(miniset_wasm PUBLIC -Wl,--whole-archive USGSCSM::usgscsm -Wl,--no-whole-archive)
        target_compile_definitions(miniset_wasm PUBLIC MINISET_HAS_USGSCSM)

        # Link GDAL and PROJ (built by BuildWasmDependencies.cmake)
        if(GDAL_FOUND AND PROJ_FOUND)
            # Add dependencies on all external projects
            add_dependencies(miniset_wasm gdal_external proj_external sqlite_external)

            target_link_libraries(miniset_wasm PUBLIC
                -Wl,--whole-archive
                ${GDAL_LIBRARY}
                -Wl,--no-whole-archive
            )
            target_include_directories(miniset_wasm PUBLIC ${GDAL_INCLUDE_DIR})
            target_compile_definitions(miniset_wasm PUBLIC MINISET_HAS_GDAL)
            message(STATUS "WASM: Will link GDAL from ${GDAL_LIBRARY}")
        endif()

        if(PROJ_FOUND)
            # WASM PROJ Linking Strategy:
            # PROJ is bundled inside USGSCSM via --whole-archive (line 181).
            # Linking PROJ separately causes duplicate symbol errors (e.g., proj_context_delete_cpp_context).
            # We only link SQLite separately since PROJ depends on it for datum transformations.
            target_link_libraries(miniset_wasm PUBLIC
                # ${PROJ_LIBRARY}  # DO NOT LINK - already in USGSCSM
                ${WASM_DEPS_INSTALL_DIR}/sqlite/lib/libsqlite3.a  # Custom SQLite with SQLITE_THREADSAFE=0
            )
            target_include_directories(miniset_wasm PUBLIC ${PROJ_INCLUDE_DIR})
            target_compile_definitions(miniset_wasm PUBLIC MINISET_HAS_PROJ)
            message(STATUS "WASM: PROJ headers from ${PROJ_INCLUDE_DIR} (library bundled in USGSCSM)")
            message(STATUS "WASM: Will link custom SQLite (THREADSAFE=0) from ${WASM_DEPS_INSTALL_DIR}/sqlite/lib/libsqlite3.a")
        endif()

        message(STATUS "Added WASM executable target with bindings")
    else()
        message(FATAL_ERROR "WASM bindings not found at src/wasm/miniset_bindings.cpp")
    endif()

    # Set WASM link flags for browser and Node.js environments
    set(WASM_LINK_FLAGS
        "--bind"
        "-lembind"
        "-sUSE_ZLIB=1"                         # Emscripten's zlib port: real
                                               # deflate/inflate for StarDS +
                                               # miniparquet GZIP reads/writes.
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/pre.js"
        "-sWASM=1"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME='Miniset'"
        "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web,node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=536870912"           # 512 MB (increased for GDAL)
        "-sMAXIMUM_MEMORY=2147483648"          # 2 GB (restored for DEM operations)
        "-sSTACK_SIZE=67108864"                # 64 MB
        "-s FORCE_FILESYSTEM=1"                # Force filesystem init (like gdal3.js)
        "-lworkerfs.js"                        # Add workerfs support
        "-lnodefs.js"                          # Add nodefs support
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','getValue','setValue','getExceptionMessage']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_GDALAllRegister','_GDALOpen','_GDALClose','_GDALGetProjectionRef','_GDALSetProjection','_GDALGetGeoTransform','_GDALGetRasterXSize','_GDALGetRasterYSize','_GDALGetRasterCount','_OSRNewSpatialReference','_OSRDestroySpatialReference','_OSRSetFromUserInput','_OSRExportToWkt','_OSRGetSemiMajor','_OSRGetSemiMinor','_CPLSetConfigOption','_CPLGetLastErrorMsg','_GDALInfo','_GDALInfoOptionsNew','_GDALInfoOptionsFree']"
        "-fexceptions"
        "-sDISABLE_EXCEPTION_CATCHING=0"       # Enable exception catching (required with -fexceptions)
        "-sFETCH=1"                            # Enable Fetch API: HTTP VSI + miniparquet
                                               # remote range reads (EmscriptenFetchRangeReader).
        # ASYNCIFY: required by StarDS's WASM /vsicurl + /vsis3 backend. Under
        # emscripten StarDS performs each ranged read with `await fetch()` via
        # EM_ASYNC_JS while keeping read_at() blocking; ASYNCIFY suspends/rewinds
        # the Wasm stack across that awaited Promise so the blocking call resolves.
        # This is what lets the docs hero stream points straight from the remote
        # net (S3 over /vsicurl/) with no local copy.
        #
        # It was previously disabled after a crash in PROJ's SQLite access during
        # GetSpatialRef(). ASYNCIFY_ADD/ONLY can scope instrumentation if that
        # resurfaces; for now instrument broadly and verify the CSM/PROJ paths
        # (campt) still work after enabling it.
        "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=65536"
        "-sERROR_ON_UNDEFINED_SYMBOLS=0"       # Allow undefined symbols (will be resolved at link time)
        "-Wl,--allow-undefined"
        # Embed PROJ database files for spatial reference support (use /usr/share/proj path like gdal3.js)
        "--embed-file=${CMAKE_CURRENT_BINARY_DIR}/wasm-deps/proj/share/proj@/usr/share/proj"
        # Embed GDAL data files for coordinate system definitions (use /usr/share/gdal path like gdal3.js)
        "--embed-file=${CMAKE_CURRENT_BINARY_DIR}/wasm-deps/gdal/share/gdal@/usr/share/gdal"
    )

    # Add optimization flags
    if(MINISET_WASM_OPTIMIZE)
        list(APPEND WASM_LINK_FLAGS "-O2")
        message(STATUS "WASM optimization enabled: -O2")
    else()
        list(APPEND WASM_LINK_FLAGS "-O1")
        message(STATUS "WASM optimization disabled: -O1")
    endif()

    # Add debug flags
    if(MINISET_WASM_DEBUG)
        list(APPEND WASM_LINK_FLAGS "-g" "-gsource-map" "-sASSERTIONS=1" "-sEXCEPTION_DEBUG=1")
        message(STATUS "WASM debug symbols enabled")
    else()
        # Even in release, enable basic exception debugging
        list(APPEND WASM_LINK_FLAGS "-sEXCEPTION_DEBUG=1")
    endif()

    # Apply link flags to WASM executable
    target_link_options(miniset_wasm PRIVATE ${WASM_LINK_FLAGS})

    # Set output names
    set_target_properties(miniset_wasm PROPERTIES
        OUTPUT_NAME "miniset"
        SUFFIX ".js"
    )

    message(STATUS "WASM build configured:")
    message(STATUS "  - Environment: Browser and Node.js (web,node)")
    message(STATUS "  - Export name: Miniset")
    message(STATUS "  - Output: miniset.js + miniset.wasm")

endif()
