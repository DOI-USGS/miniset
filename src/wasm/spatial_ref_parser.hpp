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
 * Minimal spatial reference parser for WASM
 * Extracts ellipsoid parameters from WKT and PROJ strings without PROJ database
 */

#ifndef MINISET_WASM_SPATIAL_REF_PARSER_HPP
#define MINISET_WASM_SPATIAL_REF_PARSER_HPP

namespace miniset {
namespace wasm {

struct EllipsoidParams {
    double semi_major;
    double semi_minor;
    bool found;
};

/**
 * Parse ellipsoid parameters from WKT or PROJ string
 *
 * Supports:
 * - WKT format: SPHEROID["name",semi_major,inv_flattening]
 * - PROJ format: +a=6378137 +b=6356752.314245
 * - PROJ format: +a=6378137 +rf=298.257223563
 * - Common names: +ellps=WGS84
 *
 * @param proj_string WKT or PROJ format string
 * @return EllipsoidParams with semi_major, semi_minor, and found flag
 */
EllipsoidParams parseEllipsoidFromString(const char* proj_string);

} // namespace wasm
} // namespace miniset

#endif // MINISET_WASM_SPATIAL_REF_PARSER_HPP
