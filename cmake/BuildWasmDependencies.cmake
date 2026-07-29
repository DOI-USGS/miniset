# BuildWasmDependencies.cmake
# Use ExternalProject to build PROJ and GDAL at build-time (not configure-time)

if(NOT EMSCRIPTEN)
    return()
endif()

message(STATUS "Configuring WebAssembly dependencies as external projects...")

include(ExternalProject)
include(ProcessorCount)

# Detect number of processors for parallel builds
ProcessorCount(N_PROCS)
if(N_PROCS EQUAL 0)
    set(N_PROCS 4)  # Fallback to 4 if detection fails
endif()
message(STATUS "Building WASM dependencies with ${N_PROCS} parallel jobs")

set(WASM_DEPS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(WASM_DEPS_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/wasm-deps")

# ==============================================================================
# SQLite3 External Project (custom build with SQLITE_THREADSAFE=0)
# ==============================================================================

set(SQLITE_VERSION "3450100")
set(SQLITE_INSTALL_DIR "${WASM_DEPS_INSTALL_DIR}/sqlite")

ExternalProject_Add(sqlite_external
    URL "https://www.sqlite.org/2024/sqlite-autoconf-${SQLITE_VERSION}.tar.gz"
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite-src"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite-build"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_C_COMPILER}
        -DSQLITE_THREADSAFE=0
        -DSQLITE_DISABLE_LFS
        -DSQLITE_ENABLE_FTS3
        -DSQLITE_ENABLE_FTS3_PARENTHESIS
        -DSQLITE_ENABLE_JSON1
        -DSQLITE_ENABLE_NORMALIZE
        -O2
        -c <SOURCE_DIR>/sqlite3.c
        -o sqlite3.o
    COMMAND ${CMAKE_AR} rcs libsqlite3.a sqlite3.o
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${SQLITE_INSTALL_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SQLITE_INSTALL_DIR}/include
    COMMAND ${CMAKE_COMMAND} -E copy libsqlite3.a ${SQLITE_INSTALL_DIR}/lib/
    COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/sqlite3.h ${SQLITE_INSTALL_DIR}/include/
    COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/sqlite3ext.h ${SQLITE_INSTALL_DIR}/include/
    BUILD_BYPRODUCTS
        ${SQLITE_INSTALL_DIR}/lib/libsqlite3.a
        ${SQLITE_INSTALL_DIR}/include/sqlite3.h
    INSTALL_DIR ${SQLITE_INSTALL_DIR}
)

# ==============================================================================
# PROJ External Project
# ==============================================================================

set(PROJ_INSTALL_DIR "${WASM_DEPS_INSTALL_DIR}/proj")

ExternalProject_Add(proj_external
    DEPENDS sqlite_external
    SOURCE_DIR "${WASM_DEPS_SOURCE_DIR}/proj"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/proj-build"
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -j${N_PROCS}
    CMAKE_ARGS
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_FLAGS=-fexceptions
        -DCMAKE_CXX_FLAGS=-fexceptions
        -DCMAKE_INSTALL_PREFIX=${PROJ_INSTALL_DIR}
        -DBUILD_SHARED_LIBS=OFF
        -DENABLE_TIFF=OFF
        -DENABLE_CURL=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_PROJSYNC=OFF
        -DBUILD_APPS=OFF  # Don't build CLI apps
        -DBUILD_CCT=OFF
        -DBUILD_CS2CS=OFF
        -DBUILD_GEOD=OFF
        -DBUILD_GIE=OFF
        -DBUILD_PROJ=OFF
        -DBUILD_PROJINFO=OFF
        -DSQLITE3_INCLUDE_DIR=${SQLITE_INSTALL_DIR}/include
        -DSQLITE3_LIBRARY=${SQLITE_INSTALL_DIR}/lib/libsqlite3.a
    BUILD_BYPRODUCTS
        ${PROJ_INSTALL_DIR}/lib/libproj.a
        ${PROJ_INSTALL_DIR}/include/proj.h
        ${PROJ_INSTALL_DIR}/share/proj/proj.db
        ${PROJ_INSTALL_DIR}/share/proj/proj.ini
    INSTALL_DIR ${PROJ_INSTALL_DIR}
)

# ==============================================================================
# GDAL External Project (depends on PROJ)
# ==============================================================================

set(GDAL_INSTALL_DIR "${WASM_DEPS_INSTALL_DIR}/gdal")

ExternalProject_Add(gdal_external
    DEPENDS proj_external
    SOURCE_DIR "${WASM_DEPS_SOURCE_DIR}/gdal"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/gdal-build"
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -j${N_PROCS}
    CMAKE_ARGS
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_FLAGS=-fexceptions
        -DCMAKE_CXX_FLAGS=-fexceptions
        -DCMAKE_INSTALL_PREFIX=${GDAL_INSTALL_DIR}
        -DBUILD_SHARED_LIBS=OFF
        -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF
        -DOGR_BUILD_OPTIONAL_DRIVERS=OFF
        -DGDAL_USE_GEOTIFF_INTERNAL=ON
        -DGDAL_USE_TIFF_INTERNAL=ON
        -DGDAL_USE_ZLIB=ON
        -DGDAL_USE_JPEG_INTERNAL=ON
        -DGDAL_USE_PNG=OFF
        -DGDAL_USE_PROJ=ON
        -DPROJ_INCLUDE_DIR=${PROJ_INSTALL_DIR}/include
        -DPROJ_LIBRARY=${PROJ_INSTALL_DIR}/lib/libproj.a
        -DACCEPT_MISSING_SQLITE3_MUTEX_ALLOC=ON  # Critical: Allow GDAL to use SQLite without mutex
        -DGDAL_USE_CURL=OFF  # Disable CURL to avoid dependency issues
        -DGDAL_ENABLE_DRIVER_HTTP=OFF  # Disable HTTP driver
        -DGDAL_USE_HDF4=OFF
        -DGDAL_USE_HDF5=OFF
        -DGDAL_USE_NETCDF=OFF
        -DGDAL_USE_GEOS=OFF
        -DGDAL_USE_SQLITE3=OFF
        -DGDAL_USE_POSTGRESQL=OFF
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
        -DHAVE_FOPEN64=0
        -DHAVE_FSEEK64=0
        -DHAVE_FTELL64=0
        -DHAVE_STAT64=0
        -DBUILD_APPS=OFF  # Don't build CLI apps
        -DBUILD_TESTING=OFF  # Don't build tests
    BUILD_BYPRODUCTS
        ${GDAL_INSTALL_DIR}/lib/libgdal.a
        ${GDAL_INSTALL_DIR}/include/gdal.h
    INSTALL_DIR ${GDAL_INSTALL_DIR}
)

# Set variables in current scope (will be visible to parent)
set(PROJ_INCLUDE_DIR "${PROJ_INSTALL_DIR}/include")
set(PROJ_LIBRARY "${PROJ_INSTALL_DIR}/lib/libproj.a")
set(PROJ_FOUND TRUE)

set(GDAL_INCLUDE_DIR "${GDAL_INSTALL_DIR}/include")
set(GDAL_LIBRARY "${GDAL_INSTALL_DIR}/lib/libgdal.a")
set(GDAL_FOUND TRUE)

message(STATUS "External projects configured:")
message(STATUS "  - PROJ will be built to: ${PROJ_INSTALL_DIR}")
message(STATUS "  - GDAL will be built to: ${GDAL_INSTALL_DIR}")
message(STATUS "  - Dependencies will be built when you run 'make miniset_wasm'")
