/**
 * miniset_wasm.cpp
 * WebAssembly wrapper for miniset C++ library
 *
 * Exports C functions that can be called from JavaScript
 */

#include <emscripten/emscripten.h>
#include <cmath>
#include <vector>

// Include miniset headers
#include "core/types.hpp"
#include "utils/coordinate_transforms.hpp"

extern "C" {

/**
 * Convert ECEF coordinates to Lat/Lon
 * @param x ECEF X coordinate (meters)
 * @param y ECEF Y coordinate (meters)
 * @param z ECEF Z coordinate (meters)
 * @param semi_major Semi-major axis (meters)
 * @param semi_minor Semi-minor axis (meters)
 * @param out_lat Pointer to output latitude (radians)
 * @param out_lon Pointer to output longitude (radians)
 */
EMSCRIPTEN_KEEPALIVE
void ecefToLatLon(double x, double y, double z,
                  double semi_major, double semi_minor,
                  double* out_lat, double* out_lon) {
    Vec3 ecef{x, y, z};
    LatLon result = utils::ecefToLatLon(ecef, semi_major, semi_minor);
    *out_lat = result.lat;
    *out_lon = result.lon;
}

/**
 * Convert Lat/Lon to ECEF coordinates
 * @param lat Latitude (radians)
 * @param lon Longitude (radians)
 * @param height Height above ellipsoid (meters)
 * @param semi_major Semi-major axis (meters)
 * @param semi_minor Semi-minor axis (meters)
 * @param out_x Pointer to output ECEF X (meters)
 * @param out_y Pointer to output ECEF Y (meters)
 * @param out_z Pointer to output ECEF Z (meters)
 */
EMSCRIPTEN_KEEPALIVE
void latLonToEcef(double lat, double lon, double height,
                  double semi_major, double semi_minor,
                  double* out_x, double* out_y, double* out_z) {
    Vec3 result = utils::latLonToEcef(lat, lon, height, semi_major, semi_minor);
    *out_x = result.x;
    *out_y = result.y;
    *out_z = result.z;
}

/**
 * Compute ellipsoid radius at given lat/lon
 * Simple approximation without full Ellipsoid class
 * @param lat Latitude (radians)
 * @param lon Longitude (radians)
 * @param semi_major Semi-major axis (meters)
 * @param semi_minor Semi-minor axis (meters)
 * @return Radius from center (meters)
 */
EMSCRIPTEN_KEEPALIVE
double getEllipsoidRadius(double lat, double lon,
                          double semi_major, double semi_minor) {
    // Simple approximation: R = sqrt(a^2*cos^2(lat) + c^2*sin^2(lat))
    double cos_lat = std::cos(lat);
    double sin_lat = std::sin(lat);
    double a2 = semi_major * semi_major;
    double c2 = semi_minor * semi_minor;
    return std::sqrt(a2 * cos_lat * cos_lat + c2 * sin_lat * sin_lat);
}

/**
 * Generate a meshgrid of lat/lon coordinates
 * @param lat_min Minimum latitude (degrees)
 * @param lat_max Maximum latitude (degrees)
 * @param lon_min Minimum longitude (degrees)
 * @param lon_max Maximum longitude (degrees)
 * @param nx Number of points in longitude direction
 * @param ny Number of points in latitude direction
 * @param out_lats Pointer to output latitudes array (radians)
 * @param out_lons Pointer to output longitudes array (radians)
 */
EMSCRIPTEN_KEEPALIVE
void generateMeshgrid(double lat_min, double lat_max,
                      double lon_min, double lon_max,
                      int nx, int ny,
                      double* out_lats, double* out_lons) {
    int idx = 0;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            double lat_deg = lat_min + (lat_max - lat_min) * j / (ny - 1);
            double lon_deg = lon_min + (lon_max - lon_min) * i / (nx - 1);

            // Convert to radians
            out_lats[idx] = lat_deg * M_PI / 180.0;
            out_lons[idx] = lon_deg * M_PI / 180.0;
            idx++;
        }
    }
}

/**
 * Compute mock reprojection differences for demonstration
 * This would normally call CSM and ISIS functions, but for the mockup
 * we generate synthetic data.
 *
 * @param nx Number of points in X direction
 * @param ny Number of points in Y direction
 * @param out_lats Output latitudes (radians)
 * @param out_lons Output longitudes (radians)
 * @param out_diff_x Output X differences (pixels)
 * @param out_diff_y Output Y differences (pixels)
 * @param out_magnitudes Output magnitudes (pixels)
 */
EMSCRIPTEN_KEEPALIVE
void computeMockReprojectionDiff(int nx, int ny,
                                  double* out_lats, double* out_lons,
                                  double* out_diff_x, double* out_diff_y,
                                  double* out_magnitudes) {
    // Generate grid over Mercury-like region
    double lat_min = -10.0;
    double lat_max = 8.0;
    double lon_min = 30.0;
    double lon_max = 48.0;

    int idx = 0;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            // Generate lat/lon
            double lat_deg = lat_min + (lat_max - lat_min) * j / (ny - 1);
            double lon_deg = lon_min + (lon_max - lon_min) * i / (nx - 1);

            out_lats[idx] = lat_deg * M_PI / 180.0;
            out_lons[idx] = lon_deg * M_PI / 180.0;

            // Generate synthetic reprojection differences
            // These would normally come from CSM vs ISIS comparison
            double seed = std::sin(lat_deg * 0.1) * std::cos(lon_deg * 0.1);
            out_diff_x[idx] = seed * 2.0 - 1.0;  // Range: -1 to 1 pixels
            out_diff_y[idx] = seed * 1.5 - 0.75; // Range: -0.75 to 0.75 pixels

            // Compute magnitude
            out_magnitudes[idx] = std::sqrt(
                out_diff_x[idx] * out_diff_x[idx] +
                out_diff_y[idx] * out_diff_y[idx]
            );

            idx++;
        }
    }
}

/**
 * Get library version string
 */
EMSCRIPTEN_KEEPALIVE
const char* getVersion() {
    return "miniset-wasm-1.0.0";
}

/**
 * Test function to verify module is working
 */
EMSCRIPTEN_KEEPALIVE
double testFunction(double x) {
    return x * 2.0 + 1.0;
}

} // extern "C"
