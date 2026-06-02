/**
 * Test loading CSM camera model from attached SPICE data
 * (ISIS cube processed with csminit)
 */

#include "csm/csm_interface.hpp"
#include "csm/camera_ops.hpp"

#include <gtest/gtest.h>
#include <gdal_priv.h>

class CsmAttachedSpiceTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Register GDAL drivers once for all tests
        GDALAllRegister();
    }

    void SetUp() override {
        // Use smaller cropped test image (GeoTIFF format)
        image_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.tiff";
        cube_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.cub";
        isd_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.json";
        model = nullptr;
    }

    void TearDown() override {
        if (model) {
            delete model;
            model = nullptr;
        }
    }

    std::string image_path;
    std::string cube_path;
    std::string isd_path;
    ::csm::RasterGM* model;
};

TEST_F(CsmAttachedSpiceTest, LoadModelFromAttachedSpice) {
    // Load CSM model from attached SPICE data
    ASSERT_NO_THROW({
        model = csm::createCsmFromAttachedSpice(image_path);
    });

    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->getModelName(), "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL");
}

TEST_F(CsmAttachedSpiceTest, ImageSize) {
    model = csm::createCsmFromAttachedSpice(image_path);
    ASSERT_NE(model, nullptr);

    auto size = model->getImageSize();
    // CSM state has been properly updated for the cropped image
    EXPECT_EQ(size.line, 100);
    EXPECT_EQ(size.samp, 100);
}

TEST_F(CsmAttachedSpiceTest, ImageToGround) {
    model = csm::createCsmFromAttachedSpice(image_path);
    ASSERT_NE(model, nullptr);

    // Test image to ground at a known point
    Vec3 ground = csm::imageToGround(model, 1000, 1000, 0.0);

    // Verify we get reasonable ECEF coordinates (Mars surface)
    // Mars radius is ~3396 km, so coordinates should be in that range
    double distance = std::sqrt(ground.x * ground.x + ground.y * ground.y + ground.z * ground.z);
    EXPECT_GT(distance, 3300000.0);  // At least 3300 km from center
    EXPECT_LT(distance, 3500000.0);  // No more than 3500 km from center
}

TEST_F(CsmAttachedSpiceTest, RoundTripAccuracy) {
    model = csm::createCsmFromAttachedSpice(image_path);
    ASSERT_NE(model, nullptr);

    // Test round-trip: image -> ground -> image
    double orig_line = 1000.0;
    double orig_sample = 1000.0;

    Vec3 ground = csm::imageToGround(model, orig_line, orig_sample, 0.0);

    double line_rt, sample_rt;
    csm::groundToImage(model, ground, line_rt, sample_rt);

    // Should be accurate to within a small fraction of a pixel
    EXPECT_NEAR(line_rt, orig_line, 0.001);
    EXPECT_NEAR(sample_rt, orig_sample, 0.001);
}

TEST_F(CsmAttachedSpiceTest, InvalidImagePath) {
    // Test with non-existent file
    EXPECT_THROW({
        model = csm::createCsmFromAttachedSpice("/nonexistent/path.tiff");
    }, std::runtime_error);
}

TEST_F(CsmAttachedSpiceTest, ImageWithoutAttachedSpice) {
    // Test with image that doesn't have attached SPICE data
    // Create a simple test image without CSM metadata
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver) {
        std::string temp_path = "/tmp/test_no_csm.tif";
        GDALDataset* temp_ds = driver->Create(temp_path.c_str(), 100, 100, 1, GDT_Byte, nullptr);
        if (temp_ds) {
            GDALClose(temp_ds);

            EXPECT_THROW({
                model = csm::createCsmFromAttachedSpice(temp_path);
            }, std::runtime_error);

            // Cleanup
            GDALDeleteDataset(driver, temp_path.c_str());
        }
    }
}

TEST_F(CsmAttachedSpiceTest, CompareWithIsdLoad) {
    // Load model from attached SPICE (CSM state attached by ISIS csminit)
    ::csm::RasterGM* model_gdal = csm::createCsmFromAttachedSpice(image_path);
    ASSERT_NE(model_gdal, nullptr);

    // Load model from ISD JSON file
    ::csm::RasterGM* model_isd = csm::createCsmFromISD(isd_path);
    ASSERT_NE(model_isd, nullptr);

    // Both should produce the same model type
    EXPECT_EQ(model_gdal->getModelName(), model_isd->getModelName());

    // Both should have the same image size since the CSM state was properly cropped
    auto size_gdal = model_gdal->getImageSize();
    auto size_isd = model_isd->getImageSize();
    EXPECT_EQ(size_gdal.line, size_isd.line);
    EXPECT_EQ(size_gdal.samp, size_isd.samp);

    // Test a point in the middle of the image
    double test_line = 50.0;
    double test_sample = 50.0;

    Vec3 ground_gdal = csm::imageToGround(model_gdal, test_line, test_sample, 0.0);
    Vec3 ground_isd = csm::imageToGround(model_isd, test_line, test_sample, 0.0);

    // ECEF coordinates should match exactly - both derived from same sensor geometry
    EXPECT_NEAR(ground_gdal.x, ground_isd.x, 0.001);
    EXPECT_NEAR(ground_gdal.y, ground_isd.y, 0.001);
    EXPECT_NEAR(ground_gdal.z, ground_isd.z, 0.001);

    // Cleanup
    delete model_gdal;
    delete model_isd;
}

// Cube-specific tests - skip metadata test, just test loading

TEST_F(CsmAttachedSpiceTest, LoadModelFromCube) {
    // Load CSM model from ISIS3 cube with attached CSM state
    // The cube was processed with ISIS csminit, and GDAL exposes the label via json:ISIS3
    ASSERT_NO_THROW({
        model = csm::createCsmFromAttachedSpice(cube_path);
    });

    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->getModelName(), "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL");
}

TEST_F(CsmAttachedSpiceTest, CubeImageSize) {
    // ISIS3 cubes are supported via GDAL ISIS3 driver
    model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(model, nullptr);

    auto size = model->getImageSize();
    EXPECT_EQ(size.line, 100);
    EXPECT_EQ(size.samp, 100);
}

TEST_F(CsmAttachedSpiceTest, CubeRoundTripAccuracy) {
    // Test that cube format works the same as GeoTIFF for projections
    model = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(model, nullptr);

    // Test round-trip: image -> ground -> image
    double orig_line = 50.0;
    double orig_sample = 50.0;

    Vec3 ground = csm::imageToGround(model, orig_line, orig_sample, 0.0);

    double line_rt, sample_rt;
    csm::groundToImage(model, ground, line_rt, sample_rt);

    // Should be accurate to within a small fraction of a pixel
    EXPECT_NEAR(line_rt, orig_line, 0.001);
    EXPECT_NEAR(sample_rt, orig_sample, 0.001);
}

TEST_F(CsmAttachedSpiceTest, CubeTiffSameResult) {
    // Verify that cube and GeoTIFF with same CSM state produce identical results
    ::csm::RasterGM* model_cube = csm::createCsmFromAttachedSpice(cube_path);
    ASSERT_NE(model_cube, nullptr);

    ::csm::RasterGM* model_tiff = csm::createCsmFromAttachedSpice(image_path);
    ASSERT_NE(model_tiff, nullptr);

    // Both should have same model type
    EXPECT_EQ(model_cube->getModelName(), model_tiff->getModelName());

    // Both should have same image size
    auto size_cube = model_cube->getImageSize();
    auto size_tiff = model_tiff->getImageSize();
    EXPECT_EQ(size_cube.line, size_tiff.line);
    EXPECT_EQ(size_cube.samp, size_tiff.samp);

    // Test projection at same point
    double test_line = 50.0;
    double test_sample = 50.0;

    Vec3 ground_cube = csm::imageToGround(model_cube, test_line, test_sample, 0.0);
    Vec3 ground_tiff = csm::imageToGround(model_tiff, test_line, test_sample, 0.0);

    // Results should be identical (same CSM state)
    EXPECT_NEAR(ground_cube.x, ground_tiff.x, 0.001);
    EXPECT_NEAR(ground_cube.y, ground_tiff.y, 0.001);
    EXPECT_NEAR(ground_cube.z, ground_tiff.z, 0.001);

    delete model_cube;
    delete model_tiff;
}
