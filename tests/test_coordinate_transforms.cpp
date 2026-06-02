#include <gtest/gtest.h>
#include "utils/coordinate_transforms.hpp"
#include "core/math.hpp"
#include <cmath>

namespace utils::tests {

// Test ECEF to Lat/Lon conversions
TEST(CoordinateTransformTest, EcefToLatLonEquator) {
    // Point on equator
    Vec3 ecef_pt{6378137.0, 0.0, 0.0};  // WGS84 semi-major axis

    LatLon result = ecefToLatLon(ecef_pt, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.lat, 0.0, 1e-6);
    EXPECT_NEAR(result.lon, 0.0, 1e-6);
}

TEST(CoordinateTransformTest, EcefToLatLonNorthPole) {
    // Point at north pole
    Vec3 ecef_pt{0.0, 0.0, 6356752.314245};  // WGS84 semi-minor axis

    LatLon result = ecefToLatLon(ecef_pt, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.lat, M_PI / 2.0, 1e-6);
    // Longitude undefined at pole
}

TEST(CoordinateTransformTest, EcefToLatLon90East) {
    // Point on equator at 90 degrees east
    Vec3 ecef_pt{0.0, 6378137.0, 0.0};

    LatLon result = ecefToLatLon(ecef_pt, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.lat, 0.0, 1e-6);
    EXPECT_NEAR(result.lon, M_PI / 2.0, 1e-6);
}

TEST(CoordinateTransformTest, EcefToLatLon180) {
    // Point on equator at 180 degrees
    Vec3 ecef_pt{-6378137.0, 0.0, 0.0};

    LatLon result = ecefToLatLon(ecef_pt, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.lat, 0.0, 1e-6);
    EXPECT_NEAR(std::abs(result.lon), M_PI, 1e-6);
}

// Test Lat/Lon to ECEF conversions
TEST(CoordinateTransformTest, LatLonToEcefEquator) {
    Vec3 result = latLonToEcef(0.0, 0.0, 0.0, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.x, 6378137.0, 1.0);
    EXPECT_NEAR(result.y, 0.0, 1e-6);
    EXPECT_NEAR(result.z, 0.0, 1e-6);
}

TEST(CoordinateTransformTest, LatLonToEcefPole) {
    Vec3 result = latLonToEcef(M_PI / 2.0, 0.0, 0.0, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.x, 0.0, 1e-6);
    EXPECT_NEAR(result.y, 0.0, 1e-6);
    EXPECT_NEAR(result.z, 6356752.314245, 1.0);
}

TEST(CoordinateTransformTest, LatLonToEcef90East) {
    Vec3 result = latLonToEcef(0.0, M_PI / 2.0, 0.0, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.x, 0.0, 1e-6);
    EXPECT_NEAR(result.y, 6378137.0, 1.0);
    EXPECT_NEAR(result.z, 0.0, 1e-6);
}

TEST(CoordinateTransformTest, LatLonToEcefWithHeight) {
    // Add 1000m height
    Vec3 result = latLonToEcef(0.0, 0.0, 1000.0, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.x, 6378137.0 + 1000.0, 1.0);
    EXPECT_NEAR(result.y, 0.0, 1e-6);
    EXPECT_NEAR(result.z, 0.0, 1e-6);
}

// Test round-trip conversions
TEST(CoordinateTransformTest, RoundTripEcefToLatLonToEcef) {
    Vec3 original{6378137.0, 0.0, 0.0};

    LatLon latlon = ecefToLatLon(original, 6378137.0, 6356752.314245);
    Vec3 result = latLonToEcef(latlon.lat, latlon.lon, 0.0, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.x, original.x, 1.0);
    EXPECT_NEAR(result.y, original.y, 1.0);
    EXPECT_NEAR(result.z, original.z, 1.0);
}

TEST(CoordinateTransformTest, RoundTripLatLonToEcefToLatLon) {
    double lat = M_PI / 4.0;  // 45 degrees
    double lon = M_PI / 6.0;  // 30 degrees

    Vec3 ecef = latLonToEcef(lat, lon, 0.0, 6378137.0, 6356752.314245);
    LatLon result = ecefToLatLon(ecef, 6378137.0, 6356752.314245);

    EXPECT_NEAR(result.lat, lat, 1e-6);
    EXPECT_NEAR(result.lon, lon, 1e-6);
}

// Test Mars ellipsoid
TEST(CoordinateTransformTest, MarsEcefToLatLon) {
    // Mars radii
    double a = 3396190.0;
    double c = 3376200.0;

    Vec3 ecef_pt{a, 0.0, 0.0};
    LatLon result = ecefToLatLon(ecef_pt, a, c);

    EXPECT_NEAR(result.lat, 0.0, 1e-6);
    EXPECT_NEAR(result.lon, 0.0, 1e-6);
}

TEST(CoordinateTransformTest, MarsLatLonToEcef) {
    double a = 3396190.0;
    double c = 3376200.0;

    Vec3 result = latLonToEcef(M_PI / 2.0, 0.0, 0.0, a, c);

    EXPECT_NEAR(result.x, 0.0, 1e-6);
    EXPECT_NEAR(result.y, 0.0, 1e-6);
    EXPECT_NEAR(result.z, c, 1.0);
}

// Test batch transformations
TEST(CoordinateTransformTest, BatchEcefToLatLon) {
    std::vector<Vec3> points = {
        {6378137.0, 0.0, 0.0},
        {0.0, 6378137.0, 0.0}
    };

    std::vector<LatLon> results = batchEcefToLatLon(points, 6378137.0, 6356752.314245);

    EXPECT_EQ(results.size(), 2);
    EXPECT_NEAR(results[0].lat, 0.0, 1e-6);
    EXPECT_NEAR(results[0].lon, 0.0, 1e-6);
    EXPECT_NEAR(results[1].lat, 0.0, 1e-6);
    EXPECT_NEAR(results[1].lon, M_PI / 2.0, 1e-6);
}
} // namespace utils::tests
