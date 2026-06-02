/**
 * Test campt function with both GeoTIFF and ISIS3 cube images
 */

#include "csm/campt.hpp"
#include "csm/csm_interface.hpp"
#include "surface/ellipsoid.hpp"

#include <gtest/gtest.h>
#include <gdal_priv.h>
#include <cmath>

class CamptTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        GDALAllRegister();
    }

    void SetUp() override {
        // Test images (cropped MRO CTX image)
        tiff_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.tiff";
        cube_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.cub";

        // Mars ellipsoid (from test image metadata)
        mars_ellipsoid.a = 3396190.0;  // meters
        mars_ellipsoid.b = 3396190.0;
        mars_ellipsoid.c = 3376200.0;

        tiff_model = nullptr;
        cube_model = nullptr;
    }

    void TearDown() override {
        if (tiff_model) {
            delete tiff_model;
            tiff_model = nullptr;
        }
        if (cube_model) {
            delete cube_model;
            cube_model = nullptr;
        }
    }

    std::string tiff_path;
    std::string cube_path;
    Ellipsoid3 mars_ellipsoid;
    ::csm::RasterGM* tiff_model;
    ::csm::RasterGM* cube_model;
};

// Test: Load model and compute campt info for GeoTIFF
TEST_F(CamptTest, CamptFromTiff) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    ASSERT_NE(tiff_model, nullptr);

    // Compute campt at center of image (50, 50)
    csm::CamptInfo info = csm::campt(tiff_model, 50.0, 50.0, mars_ellipsoid, 0.0);

    // Verify input preserved
    EXPECT_DOUBLE_EQ(info.sample, 50.0);
    EXPECT_DOUBLE_EQ(info.line, 50.0);

    // Verify ground point is on Mars surface
    double radius = std::sqrt(
        info.ground_point.x * info.ground_point.x +
        info.ground_point.y * info.ground_point.y +
        info.ground_point.z * info.ground_point.z
    );
    EXPECT_GT(radius, 3.3e6);  // > 3300 km
    EXPECT_LT(radius, 3.5e6);  // < 3500 km

    // Verify lat/lon are reasonable for Mars
    double lat_deg = info.planetocentric_lat * 180.0 / M_PI;
    double lon_deg = info.positive_east_lon * 180.0 / M_PI;
    EXPECT_GT(lat_deg, -90.0);
    EXPECT_LT(lat_deg, 90.0);
    EXPECT_GE(lon_deg, 0.0);
    EXPECT_LT(lon_deg, 360.0);

    // Verify photometric angles are in valid range
    EXPECT_GE(info.phase_angle, 0.0);
    EXPECT_LE(info.phase_angle, 180.0);
    EXPECT_GE(info.emission_angle, 0.0);
    EXPECT_LE(info.emission_angle, 180.0);
    EXPECT_GE(info.incidence_angle, 0.0);
    EXPECT_LE(info.incidence_angle, 180.0);

    // Verify resolution values are positive
    EXPECT_GT(info.sample_resolution, 0.0);
    EXPECT_GT(info.line_resolution, 0.0);
    EXPECT_GT(info.pixel_resolution, 0.0);
    EXPECT_GT(info.oblique_resolution, 0.0);

    // Verify distances are positive and reasonable
    EXPECT_GT(info.slant_distance, 0.0);
    EXPECT_GT(info.target_center_distance, radius);  // Spacecraft is above surface
    EXPECT_GT(info.spacecraft_altitude, 0.0);
    EXPECT_NEAR(info.local_radius, radius, 1.0);

    // Verify off-nadir angle is reasonable
    EXPECT_GE(info.off_nadir_angle, 0.0);
    EXPECT_LE(info.off_nadir_angle, 90.0);
}

// Test: Load model and compute campt info for ISIS3 cube
TEST_F(CamptTest, CamptFromCube) {
    cube_model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(cube_model, nullptr);

    // Compute campt at center of image (50, 50)
    csm::CamptInfo info = csm::campt(cube_model, 50.0, 50.0, mars_ellipsoid, 0.0);

    // Verify input preserved
    EXPECT_DOUBLE_EQ(info.sample, 50.0);
    EXPECT_DOUBLE_EQ(info.line, 50.0);

    // Verify ground point is on Mars surface
    double radius = std::sqrt(
        info.ground_point.x * info.ground_point.x +
        info.ground_point.y * info.ground_point.y +
        info.ground_point.z * info.ground_point.z
    );
    EXPECT_GT(radius, 3.3e6);
    EXPECT_LT(radius, 3.5e6);

    // Verify lat/lon are reasonable
    double lat_deg = info.planetocentric_lat * 180.0 / M_PI;
    double lon_deg = info.positive_east_lon * 180.0 / M_PI;
    EXPECT_GT(lat_deg, -90.0);
    EXPECT_LT(lat_deg, 90.0);
    EXPECT_GE(lon_deg, 0.0);
    EXPECT_LT(lon_deg, 360.0);

    // Verify photometric angles are valid
    EXPECT_GE(info.phase_angle, 0.0);
    EXPECT_LE(info.phase_angle, 180.0);
    EXPECT_GE(info.emission_angle, 0.0);
    EXPECT_LE(info.emission_angle, 180.0);
    EXPECT_GE(info.incidence_angle, 0.0);
    EXPECT_LE(info.incidence_angle, 180.0);

    // Verify resolution values
    EXPECT_GT(info.sample_resolution, 0.0);
    EXPECT_GT(info.line_resolution, 0.0);
    EXPECT_GT(info.pixel_resolution, 0.0);

    // Verify distances
    EXPECT_GT(info.slant_distance, 0.0);
    EXPECT_GT(info.spacecraft_altitude, 0.0);
}

// Test: TIFF and Cube produce identical results at same image coordinate
TEST_F(CamptTest, TiffAndCubeProduceSameResults) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    cube_model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(tiff_model, nullptr);
    ASSERT_NE(cube_model, nullptr);

    // Test at several points across the image
    std::vector<std::pair<double, double>> test_points = {
        {25.0, 25.0},  // Upper left quadrant
        {50.0, 50.0},  // Center
        {75.0, 75.0},  // Lower right quadrant
        {10.0, 90.0},  // Near corner
    };

    for (const auto& [sample, line] : test_points) {
        csm::CamptInfo tiff_info = csm::campt(tiff_model, sample, line, mars_ellipsoid, 0.0);
        csm::CamptInfo cube_info = csm::campt(cube_model, sample, line, mars_ellipsoid, 0.0);

        // Ground points should match exactly (same CSM state)
        EXPECT_NEAR(tiff_info.ground_point.x, cube_info.ground_point.x, 0.1)
            << "Mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.ground_point.y, cube_info.ground_point.y, 0.1)
            << "Mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.ground_point.z, cube_info.ground_point.z, 0.1)
            << "Mismatch at (" << sample << ", " << line << ")";

        // Lat/lon should match
        EXPECT_NEAR(tiff_info.planetocentric_lat, cube_info.planetocentric_lat, 1e-8)
            << "Lat mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.positive_east_lon, cube_info.positive_east_lon, 1e-8)
            << "Lon mismatch at (" << sample << ", " << line << ")";

        // Photometric angles should match
        EXPECT_NEAR(tiff_info.phase_angle, cube_info.phase_angle, 0.001)
            << "Phase angle mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.emission_angle, cube_info.emission_angle, 0.001)
            << "Emission angle mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.incidence_angle, cube_info.incidence_angle, 0.001)
            << "Incidence angle mismatch at (" << sample << ", " << line << ")";

        // Resolutions should match
        EXPECT_NEAR(tiff_info.sample_resolution, cube_info.sample_resolution, 0.01)
            << "Sample resolution mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.line_resolution, cube_info.line_resolution, 0.01)
            << "Line resolution mismatch at (" << sample << ", " << line << ")";

        // Distances should match
        EXPECT_NEAR(tiff_info.slant_distance, cube_info.slant_distance, 0.1)
            << "Slant distance mismatch at (" << sample << ", " << line << ")";
        EXPECT_NEAR(tiff_info.spacecraft_altitude, cube_info.spacecraft_altitude, 0.1)
            << "Altitude mismatch at (" << sample << ", " << line << ")";
    }
}

// Test: Campt with non-zero height
TEST_F(CamptTest, CamptWithHeight) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    cube_model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(tiff_model, nullptr);
    ASSERT_NE(cube_model, nullptr);

    double height = 1000.0;  // 1km above ellipsoid

    csm::CamptInfo tiff_info = csm::campt(tiff_model, 50.0, 50.0, mars_ellipsoid, height);
    csm::CamptInfo cube_info = csm::campt(cube_model, 50.0, 50.0, mars_ellipsoid, height);

    // Height should be stored
    EXPECT_DOUBLE_EQ(tiff_info.height, height);
    EXPECT_DOUBLE_EQ(cube_info.height, height);

    // Ground point should be higher (farther from center)
    csm::CamptInfo tiff_info_zero = csm::campt(tiff_model, 50.0, 50.0, mars_ellipsoid, 0.0);

    double radius_with_height = std::sqrt(
        tiff_info.ground_point.x * tiff_info.ground_point.x +
        tiff_info.ground_point.y * tiff_info.ground_point.y +
        tiff_info.ground_point.z * tiff_info.ground_point.z
    );
    double radius_zero = std::sqrt(
        tiff_info_zero.ground_point.x * tiff_info_zero.ground_point.x +
        tiff_info_zero.ground_point.y * tiff_info_zero.ground_point.y +
        tiff_info_zero.ground_point.z * tiff_info_zero.ground_point.z
    );

    EXPECT_GT(radius_with_height, radius_zero);
    EXPECT_NEAR(radius_with_height - radius_zero, height, 10.0);  // Approximately height difference

    // TIFF and cube should still match at elevated height
    EXPECT_NEAR(tiff_info.ground_point.x, cube_info.ground_point.x, 0.1);
    EXPECT_NEAR(tiff_info.ground_point.y, cube_info.ground_point.y, 0.1);
    EXPECT_NEAR(tiff_info.ground_point.z, cube_info.ground_point.z, 0.1);
}

// Test: Corner pixels produce valid results
TEST_F(CamptTest, CornerPixels) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    cube_model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(tiff_model, nullptr);
    ASSERT_NE(cube_model, nullptr);

    // Test all four corners (100x100 image, 0-based)
    std::vector<std::pair<double, double>> corners = {
        {0.5, 0.5},      // Top-left (center of first pixel)
        {99.5, 0.5},     // Top-right
        {0.5, 99.5},     // Bottom-left
        {99.5, 99.5}     // Bottom-right
    };

    for (const auto& [sample, line] : corners) {
        // Test with TIFF
        csm::CamptInfo tiff_info = csm::campt(tiff_model, sample, line, mars_ellipsoid, 0.0);
        EXPECT_GT(tiff_info.slant_distance, 0.0)
            << "Invalid result for TIFF at (" << sample << ", " << line << ")";
        EXPECT_GE(tiff_info.emission_angle, 0.0);
        EXPECT_LE(tiff_info.emission_angle, 180.0);

        // Test with cube
        csm::CamptInfo cube_info = csm::campt(cube_model, sample, line, mars_ellipsoid, 0.0);
        EXPECT_GT(cube_info.slant_distance, 0.0)
            << "Invalid result for cube at (" << sample << ", " << line << ")";
        EXPECT_GE(cube_info.emission_angle, 0.0);
        EXPECT_LE(cube_info.emission_angle, 180.0);

        // Results should match
        EXPECT_NEAR(tiff_info.ground_point.x, cube_info.ground_point.x, 0.1);
        EXPECT_NEAR(tiff_info.ground_point.y, cube_info.ground_point.y, 0.1);
        EXPECT_NEAR(tiff_info.ground_point.z, cube_info.ground_point.z, 0.1);
    }
}

// Test: Planetographic vs Planetocentric latitude
TEST_F(CamptTest, LatitudeTypes) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    ASSERT_NE(tiff_model, nullptr);

    csm::CamptInfo info = csm::campt(tiff_model, 50.0, 50.0, mars_ellipsoid, 0.0);

    // For oblate ellipsoid (Mars), planetographic should differ from planetocentric
    // At non-equatorial latitudes
    if (std::abs(info.planetocentric_lat) > 0.1) {
        EXPECT_NE(info.planetographic_lat, info.planetocentric_lat);
    }

    // Both should be valid latitudes
    EXPECT_GE(info.planetocentric_lat, -M_PI/2);
    EXPECT_LE(info.planetocentric_lat, M_PI/2);
    EXPECT_GE(info.planetographic_lat, -M_PI/2);
    EXPECT_LE(info.planetographic_lat, M_PI/2);
}

// Test: Sensor position is above surface
TEST_F(CamptTest, SensorAboveSurface) {
    tiff_model = csm::createCsmFromAttachedSpice(tiff_path);
    cube_model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(tiff_model, nullptr);
    ASSERT_NE(cube_model, nullptr);

    csm::CamptInfo tiff_info = csm::campt(tiff_model, 50.0, 50.0, mars_ellipsoid, 0.0);
    csm::CamptInfo cube_info = csm::campt(cube_model, 50.0, 50.0, mars_ellipsoid, 0.0);

    // Sensor position should be farther from center than ground point
    double sensor_radius_tiff = std::sqrt(
        tiff_info.sensor_position.x * tiff_info.sensor_position.x +
        tiff_info.sensor_position.y * tiff_info.sensor_position.y +
        tiff_info.sensor_position.z * tiff_info.sensor_position.z
    );
    double ground_radius_tiff = std::sqrt(
        tiff_info.ground_point.x * tiff_info.ground_point.x +
        tiff_info.ground_point.y * tiff_info.ground_point.y +
        tiff_info.ground_point.z * tiff_info.ground_point.z
    );
    EXPECT_GT(sensor_radius_tiff, ground_radius_tiff);

    double sensor_radius_cube = std::sqrt(
        cube_info.sensor_position.x * cube_info.sensor_position.x +
        cube_info.sensor_position.y * cube_info.sensor_position.y +
        cube_info.sensor_position.z * cube_info.sensor_position.z
    );
    double ground_radius_cube = std::sqrt(
        cube_info.ground_point.x * cube_info.ground_point.x +
        cube_info.ground_point.y * cube_info.ground_point.y +
        cube_info.ground_point.z * cube_info.ground_point.z
    );
    EXPECT_GT(sensor_radius_cube, ground_radius_cube);

    // Spacecraft altitude should be positive
    EXPECT_GT(tiff_info.spacecraft_altitude, 0.0);
    EXPECT_GT(cube_info.spacecraft_altitude, 0.0);

    // For MRO CTX, altitude should be hundreds of km
    EXPECT_GT(tiff_info.spacecraft_altitude, 100000.0);  // > 100 km
    EXPECT_GT(cube_info.spacecraft_altitude, 100000.0);
}
