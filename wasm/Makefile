# Miniset WASM Build using latest GDAL
# Based on gdal3.js approach

# Latest versions as of May 2025
GDAL_VERSION = 3.13.0
PROJ_VERSION = 9.5.1
SQLITE_VERSION = 3460100
GEOS_VERSION = 3.13.0
ZLIB_VERSION = 1.3.2
TIFF_VERSION = 4.7.0
GEOTIFF_VERSION = 1.7.3

# Download URLs
SQLITE_URL = "https://www.sqlite.org/2024/sqlite-autoconf-$(SQLITE_VERSION).tar.gz"
PROJ_URL = "https://download.osgeo.org/proj/proj-$(PROJ_VERSION).tar.gz"
GEOS_URL = "https://download.osgeo.org/geos/geos-$(GEOS_VERSION).tar.bz2"
ZLIB_URL = "https://zlib.net/zlib-$(ZLIB_VERSION).tar.gz"
GDAL_URL = "https://github.com/OSGeo/gdal/releases/download/v$(GDAL_VERSION)/gdal-$(GDAL_VERSION).tar.gz"
TIFF_URL = "https://download.osgeo.org/libtiff/tiff-$(TIFF_VERSION).tar.gz"
GEOTIFF_URL = "https://github.com/OSGeo/libgeotiff/releases/download/$(GEOTIFF_VERSION)/libgeotiff-$(GEOTIFF_VERSION).tar.gz"

PWD = $(shell pwd)
SRC_DIR = build/src
SRC_DIR_FULL = $(PWD)/$(SRC_DIR)
ROOT_DIR = $(PWD)/build/install
DIST_DIR = $(PWD)/build/dist
PREFIX = --prefix=$(ROOT_DIR)
PREFIX_CMAKE = "-DCMAKE_INSTALL_PREFIX=$(ROOT_DIR)"

# Emscripten flags
EMCC_CFLAGS = -O3 -fexceptions -s ERROR_ON_UNDEFINED_SYMBOLS=0
EMMAKE ?= EMCC_CFLAGS="$(EMCC_CFLAGS)" emmake
EMCMAKE ?= emcmake
EMCC ?= emcc
EMCONFIGURE ?= LDFLAGS="" CXXFLAGS="$(EMCC_CFLAGS)" CFLAGS="$(EMCC_CFLAGS)" emconfigure

# Emscripten link flags for final WASM module
EMCC_LINK_FLAGS = -O3
EMCC_LINK_FLAGS += -s ERROR_ON_UNDEFINED_SYMBOLS=0
EMCC_LINK_FLAGS += -s FORCE_FILESYSTEM=1
EMCC_LINK_FLAGS += -lworkerfs.js
EMCC_LINK_FLAGS += -lnodefs.js
EMCC_LINK_FLAGS += -s TOTAL_MEMORY=512MB
EMCC_LINK_FLAGS += -s ALLOW_MEMORY_GROWTH=1
EMCC_LINK_FLAGS += -s DISABLE_EXCEPTION_CATCHING=0
EMCC_LINK_FLAGS += -s WASM=1
EMCC_LINK_FLAGS += -s MODULARIZE=1
EMCC_LINK_FLAGS += -s EXPORT_NAME="'Miniset'"
EMCC_LINK_FLAGS += -s EXPORT_ES6=1
EMCC_LINK_FLAGS += -s ENVIRONMENT=web,node

# Exported C functions (GDAL/PROJ API)
EMCC_LINK_FLAGS += -s EXPORTED_FUNCTIONS="[\
  '_malloc',\
  '_free',\
  '_GDALAllRegister',\
  '_GDALOpen',\
  '_GDALClose',\
  '_GDALGetRasterXSize',\
  '_GDALGetRasterYSize',\
  '_GDALGetRasterCount',\
  '_GDALGetRasterBand',\
  '_GDALGetProjectionRef',\
  '_GDALSetProjection',\
  '_GDALGetGeoTransform',\
  '_GDALSetGeoTransform',\
  '_OSRNewSpatialReference',\
  '_OSRDestroySpatialReference',\
  '_OSRSetFromUserInput',\
  '_OSRExportToWkt',\
  '_OSRGetSemiMajor',\
  '_OSRGetSemiMinor',\
  '_CPLSetConfigOption',\
  '_CPLGetLastErrorMsg',\
  '_CPLGetLastErrorNo',\
  '_CPLErrorReset',\
  '_GDALGetSpatialRef'\
]"

# Exported runtime methods
EMCC_LINK_FLAGS += -s EXPORTED_RUNTIME_METHODS="[\
  'setValue',\
  'getValue',\
  'ccall',\
  'cwrap',\
  'stringToUTF8',\
  'UTF8ToString',\
  'lengthBytesUTF8',\
  'FS',\
  'ENV',\
  'WORKERFS',\
  'NODEFS',\
  'MEMFS'\
]"

# Preload data files (like gdal3.js)
EMCC_LINK_FLAGS += --preload-file $(ROOT_DIR)/share/gdal@/usr/share/gdal
EMCC_LINK_FLAGS += --preload-file $(ROOT_DIR)/share/proj@/usr/share/proj

.PHONY: all clean miniset_wasm deps

all: miniset_wasm

miniset_wasm: $(DIST_DIR)/miniset_core.js

deps: $(ROOT_DIR)/lib/libgdal.a

########
# ZLIB #
########
ZLIB_SRC = $(SRC_DIR)/zlib-$(ZLIB_VERSION)

$(ROOT_DIR)/lib/libz.a: $(ZLIB_SRC)/Makefile
	cd $(ZLIB_SRC); \
	sed -i.bak 's|^AR=libtool$$|AR=emar|g; s|^ARFLAGS=-o|ARFLAGS=cr|g' Makefile && \
	$(EMMAKE) make -j4 install

$(ZLIB_SRC)/Makefile: $(ZLIB_SRC)/configure
	cd $(ZLIB_SRC); \
	AR=emar $(EMCONFIGURE) ./configure $(PREFIX) --static

$(ZLIB_SRC)/configure:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(ZLIB_URL) | tar xz

##########
# SQLITE #
##########
SQLITE_SRC = $(SRC_DIR)/sqlite-autoconf-$(SQLITE_VERSION)

$(ROOT_DIR)/lib/libsqlite3.a: $(SQLITE_SRC)/Makefile
	cd $(SQLITE_SRC); \
	$(EMMAKE) make -j4 libsqlite3.la && \
	$(EMMAKE) make install-libLTLIBRARIES install-data

$(SQLITE_SRC)/Makefile: $(SQLITE_SRC)/configure
	cd $(SQLITE_SRC); \
	$(EMCONFIGURE) ./configure $(PREFIX) \
		--host=wasm32-unknown-emscripten \
		--disable-shared --enable-static \
		--disable-threadsafe \
		CFLAGS="-DSQLITE_THREADSAFE=0 $(EMCC_CFLAGS)"

$(SQLITE_SRC)/configure:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(SQLITE_URL) | tar xz

########
# PROJ #
########
PROJ_SRC = $(SRC_DIR)/proj-$(PROJ_VERSION)

$(ROOT_DIR)/lib/libproj.a: $(PROJ_SRC)/build/Makefile
	cd $(PROJ_SRC)/build; \
	$(EMMAKE) make -j4 install

$(PROJ_SRC)/build/Makefile: $(ROOT_DIR)/lib/libsqlite3.a $(PROJ_SRC)/CMakeLists.txt
	cd $(PROJ_SRC); \
	mkdir -p build; \
	cd build; \
	$(EMCMAKE) cmake .. $(PREFIX_CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_TESTING=OFF \
		-DBUILD_APPS=OFF \
		-DENABLE_TIFF=OFF \
		-DENABLE_CURL=OFF \
		-DSQLITE3_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DSQLITE3_LIBRARY=$(ROOT_DIR)/lib/libsqlite3.a

$(PROJ_SRC)/CMakeLists.txt:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(PROJ_URL) | tar xz

########
# TIFF #
########
TIFF_SRC = $(SRC_DIR)/tiff-$(TIFF_VERSION)

$(ROOT_DIR)/lib/libtiff.a: $(TIFF_SRC)/build/Makefile
	cd $(TIFF_SRC)/build; \
	$(EMMAKE) make -j4 install

$(TIFF_SRC)/build/Makefile: $(ROOT_DIR)/lib/libz.a $(TIFF_SRC)/CMakeLists.txt
	cd $(TIFF_SRC); \
	mkdir -p build; \
	cd build; \
	$(EMCMAKE) cmake .. $(PREFIX_CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DZLIB_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DZLIB_LIBRARY=$(ROOT_DIR)/lib/libz.a

$(TIFF_SRC)/CMakeLists.txt:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(TIFF_URL) | tar xz

############
# GEOTIFF  #
############
GEOTIFF_SRC = $(SRC_DIR)/libgeotiff-$(GEOTIFF_VERSION)

$(ROOT_DIR)/lib/libgeotiff.a: $(GEOTIFF_SRC)/build/Makefile
	cd $(GEOTIFF_SRC)/build; \
	$(EMMAKE) make -j4 install

$(GEOTIFF_SRC)/build/Makefile: $(ROOT_DIR)/lib/libproj.a $(ROOT_DIR)/lib/libtiff.a $(GEOTIFF_SRC)/CMakeLists.txt
	cd $(GEOTIFF_SRC); \
	mkdir -p build; \
	cd build; \
	$(EMCMAKE) cmake .. $(PREFIX_CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DPROJ_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DPROJ_LIBRARY=$(ROOT_DIR)/lib/libproj.a \
		-DTIFF_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DTIFF_LIBRARY=$(ROOT_DIR)/lib/libtiff.a

$(GEOTIFF_SRC)/CMakeLists.txt:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(GEOTIFF_URL) | tar xz

########
# GDAL #
########
GDAL_SRC = $(SRC_DIR)/gdal-$(GDAL_VERSION)

$(ROOT_DIR)/lib/libgdal.a: $(GDAL_SRC)/build/Makefile
	cd $(GDAL_SRC)/build; \
	$(EMMAKE) make -j4 install

$(GDAL_SRC)/build/Makefile: $(ROOT_DIR)/lib/libproj.a $(ROOT_DIR)/lib/libsqlite3.a $(ROOT_DIR)/lib/libgeotiff.a $(GDAL_SRC)/CMakeLists.txt
	cd $(GDAL_SRC); \
	mkdir -p build; \
	cd build; \
	$(EMCMAKE) cmake .. $(PREFIX_CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_APPS=OFF \
		-DGDAL_USE_TIFF_INTERNAL=OFF \
		-DGDAL_USE_GEOTIFF_INTERNAL=OFF \
		-DGDAL_USE_SQLITE3=ON \
		-DSQLite3_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DSQLite3_LIBRARY=$(ROOT_DIR)/lib/libsqlite3.a \
		-DACCEPT_MISSING_SQLITE3_MUTEX_ALLOC=ON \
		-DGDAL_USE_PROJ=ON \
		-DPROJ_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DPROJ_LIBRARY=$(ROOT_DIR)/lib/libproj.a \
		-DTIFF_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DTIFF_LIBRARY=$(ROOT_DIR)/lib/libtiff.a \
		-DGEOTIFF_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DGEOTIFF_LIBRARY=$(ROOT_DIR)/lib/libgeotiff.a \
		-DZLIB_INCLUDE_DIR=$(ROOT_DIR)/include \
		-DZLIB_LIBRARY=$(ROOT_DIR)/lib/libz.a \
		-DGDAL_USE_CURL=OFF \
		-DGDAL_USE_GEOS=OFF \
		-DGDAL_USE_HDF5=OFF \
		-DGDAL_USE_NETCDF=OFF

$(GDAL_SRC)/CMakeLists.txt:
	mkdir -p $(SRC_DIR); \
	cd $(SRC_DIR); \
	curl -L $(GDAL_URL) | tar xz

#################
# Miniset WASM  #
#################

# Miniset C++ sources (core library only, no CSM for now)
MINISET_SOURCES = \
	../src/surface/ellipsoid.cpp \
	../src/surface/dem.cpp \
	../src/wasm/proj_wrapper.cpp \
	src/miniset_bindings.cpp

# Additional Embind flags
EMCC_EMBIND_FLAGS = --bind -lembind

# Link everything together
$(DIST_DIR)/miniset_core.js: $(ROOT_DIR)/lib/libgdal.a
	@mkdir -p $(DIST_DIR)
	$(EMCC) $(MINISET_SOURCES) \
		-I../include \
		-I$(ROOT_DIR)/include \
		-L$(ROOT_DIR)/lib \
		-lgdal -lproj -lsqlite3 -lz -ltiff -lgeotiff \
		-o $@ \
		$(EMCC_LINK_FLAGS) \
		$(EMCC_EMBIND_FLAGS) \
		$(EMCC_CFLAGS) \
		-DMINISET_HAS_GDAL \
		-DMINISET_HAS_PROJ

clean:
	rm -rf build

clean-dist:
	rm -rf $(DIST_DIR)

.PRECIOUS: %/Makefile %/CMakeLists.txt
