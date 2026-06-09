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

# GDAL and PROJ are always required, but linked differently for WASM vs native
if(NOT EMSCRIPTEN)
    # Native build: link found system libraries
    target_link_libraries(miniset PUBLIC GDAL::GDAL PROJ::proj)
    target_compile_definitions(miniset PUBLIC MINISET_HAS_GDAL MINISET_HAS_PROJ)
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

# Applications (skip for WASM - CLI tools don't make sense in browser)
if(CSMAPI_FOUND AND NOT EMSCRIPTEN)
    add_executable(campt apps/campt.cpp)
    target_link_libraries(campt PRIVATE miniset)
    target_include_directories(campt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    install(TARGETS campt RUNTIME DESTINATION bin)
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
        add_executable(miniset_wasm ${MINISET_SOURCES}
            src/wasm/miniset_bindings.cpp)

        # Copy properties from main library target
        target_include_directories(miniset_wasm
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

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
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','getValue','setValue']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_GDALAllRegister','_GDALOpen','_GDALClose','_GDALGetProjectionRef','_GDALSetProjection','_GDALGetGeoTransform','_GDALGetRasterXSize','_GDALGetRasterYSize','_GDALGetRasterCount','_OSRNewSpatialReference','_OSRDestroySpatialReference','_OSRSetFromUserInput','_OSRExportToWkt','_OSRGetSemiMajor','_OSRGetSemiMinor','_CPLSetConfigOption','_CPLGetLastErrorMsg','_GDALInfo','_GDALInfoOptionsNew','_GDALInfoOptionsFree']"
        "-fexceptions"
        "-sDISABLE_EXCEPTION_CATCHING=0"       # Enable exception catching (required with -fexceptions)
        "-sFETCH=1"                            # Enable fetch API for HTTP VSI
        # NOTE: ASYNCIFY disabled - causes crashes in PROJ's SQLite access during GetSpatialRef()
        # "-sASYNCIFY=1"                         # Required for synchronous I/O emulation
        # "-sASYNCIFY_STACK_SIZE=65536"
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
