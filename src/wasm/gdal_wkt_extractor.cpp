/*
 * Copyright (C) 2024 USGS Astrogeology Science Center
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


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
