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

#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>

namespace miniset {
namespace wasm {

struct EllipsoidParams {
    double semi_major;
    double semi_minor;
    bool found;
};

// Helper to find substring case-insensitive
static const char* stristr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;

    for (; *haystack; ++haystack) {
        if (std::toupper(*haystack) == std::toupper(*needle)) {
            const char* h = haystack;
            const char* n = needle;

            while (*h && *n && std::toupper(*h) == std::toupper(*n)) {
                ++h;
                ++n;
            }

            if (!*n) return haystack;
        }
    }
    return nullptr;
}

// Extract number after a pattern in string
static bool extractNumber(const char* str, const char* pattern, double& value) {
    const char* pos = stristr(str, pattern);
    if (!pos) return false;

    pos += strlen(pattern);

    // Skip whitespace and '=' if present
    while (*pos && (std::isspace(*pos) || *pos == '=')) ++pos;

    char* endptr;
    value = std::strtod(pos, &endptr);

    return endptr != pos; // Success if we parsed something
}

// Parse SPHEROID from WKT: SPHEROID["name",semi_major,inv_flattening]
static bool parseWKTSpheroid(const char* wkt, EllipsoidParams& params) {
    const char* spheroid_start = stristr(wkt, "SPHEROID");
    if (!spheroid_start) {
        spheroid_start = stristr(wkt, "ELLIPSOID");
    }
    if (!spheroid_start) return false;

    // Find opening bracket
    const char* bracket = strchr(spheroid_start, '[');
    if (!bracket) return false;

    // Skip the name in quotes
    const char* quote1 = strchr(bracket, '"');
    if (!quote1) return false;
    const char* quote2 = strchr(quote1 + 1, '"');
    if (!quote2) return false;

    // Now find the comma after the name
    const char* comma1 = strchr(quote2, ',');
    if (!comma1) return false;

    // Parse semi-major axis
    char* endptr;
    params.semi_major = std::strtod(comma1 + 1, &endptr);
    if (endptr == comma1 + 1) return false;

    // Find next comma for inverse flattening
    const char* comma2 = strchr(endptr, ',');
    if (!comma2) return false;

    double inv_flattening = std::strtod(comma2 + 1, &endptr);
    if (endptr == comma2 + 1) return false;

    // Calculate semi-minor from inverse flattening
    if (inv_flattening == 0.0) {
        // Sphere
        params.semi_minor = params.semi_major;
    } else {
        // Ellipsoid: b = a * (1 - 1/f)
        params.semi_minor = params.semi_major * (1.0 - 1.0 / inv_flattening);
    }

    params.found = true;
    return true;
}

// Parse PROJ string: +a=6378137 +b=6356752.314245 or +ellps=WGS84
static bool parsePROJString(const char* proj, EllipsoidParams& params) {
    bool has_a = extractNumber(proj, "+a", params.semi_major);
    bool has_b = extractNumber(proj, "+b", params.semi_minor);

    if (has_a && has_b) {
        params.found = true;
        return true;
    }

    if (has_a && !has_b) {
        // Check for +rf (reverse flattening) or +f (flattening)
        double rf = 0, f = 0;
        bool has_rf = extractNumber(proj, "+rf", rf);
        bool has_f = extractNumber(proj, "+f", f);

        if (has_rf && rf > 0) {
            params.semi_minor = params.semi_major * (1.0 - 1.0 / rf);
            params.found = true;
            return true;
        } else if (has_f && f > 0) {
            params.semi_minor = params.semi_major * (1.0 - f);
            params.found = true;
            return true;
        }
    }

    // Check for common ellipsoid names
    if (stristr(proj, "+ellps=WGS84") || stristr(proj, "WGS84")) {
        params.semi_major = 6378137.0;
        params.semi_minor = 6356752.314245;
        params.found = true;
        return true;
    }

    return false;
}

// Main parsing function - tries WKT first, then PROJ
EllipsoidParams parseEllipsoidFromString(const char* proj_string) {
    EllipsoidParams params = {6378137.0, 6356752.314245, false};

    if (!proj_string || proj_string[0] == '\0') {
        return params;
    }

    // Try WKT format first (contains SPHEROID or ELLIPSOID keyword)
    if (stristr(proj_string, "SPHEROID") || stristr(proj_string, "ELLIPSOID")) {
        if (parseWKTSpheroid(proj_string, params)) {
            return params;
        }
    }

    // Try PROJ string format (starts with + or contains +a=)
    if (proj_string[0] == '+' || stristr(proj_string, "+a=")) {
        if (parsePROJString(proj_string, params)) {
            return params;
        }
    }

    // Also try WKT even if no SPHEROID keyword (might be abbreviated)
    if (!params.found) {
        parseWKTSpheroid(proj_string, params);
    }

    return params;
}

} // namespace wasm
} // namespace miniset
