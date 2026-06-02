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
