/**
 * Custom GDAL wrapper to extract raw WKT string from GeoTIFF
 * without triggering PROJ parsing (which causes C++ exceptions in WASM)
 */

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_string.h>
#include <string>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern "C" {

// Unused WASM helper functions removed during cleanup
// If WKT extraction/conversion is needed, use GDAL's OGRSpatialReference class directly

} // extern "C"
