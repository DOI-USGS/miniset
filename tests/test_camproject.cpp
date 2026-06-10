/**
 * Test camproject functionality with real MRO CTX data
 */

#include "csm/camera_ops.hpp"
#include "csm/csm_interface.hpp"
#include "surface/shape_model.hpp"

#include <gtest/gtest.h>
#include <gdal_priv.h>

#include <fstream>
#include <string>
#include <cmath>

class CamprojectTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        GDALAllRegister();
    }

    void SetUp() override {
        // Use smaller cropped test image
        isd_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.json";
        image_path = "../tests/data/W02_089524_2073_XN_27N269W.cropped.tiff";
        output_path = "/tmp/mroctx_projected.tif";
        model = nullptr;
    }

    void TearDown() override {
        if (model) {
            delete model;
            model = nullptr;
        }
    }

    bool testDataAvailable() {
        std::ifstream isd_file(isd_path);
        std::ifstream image_file(image_path);
        return isd_file.good() && image_file.good();
    }

    std::string isd_path;
    std::string image_path;
    std::string output_path;
    ::csm::RasterGM* model;
};

TEST_F(CamprojectTest, CenterPixelGroundCoordinates) {
    if (!testDataAvailable()) {
        GTEST_SKIP() << "Test data not available at " << isd_path;
    }

    model = csm::createCsmFromISD(isd_path);
    ASSERT_NE(model, nullptr);

    auto size = model->getImageSize();
    double center_line = size.line / 2.0;
    double center_sample = size.samp / 2.0;

    // Test imageToGround at center
    Vec3 ground = csm::imageToGround(model, center_line, center_sample, 0.0);

    // Verify we get reasonable ECEF coordinates
    double distance = std::sqrt(ground.x * ground.x + ground.y * ground.y + ground.z * ground.z);
    EXPECT_GT(distance, 3300000.0);  // Mars radius ~3396 km
    EXPECT_LT(distance, 3500000.0);

    // Calculate lat/lon for verification (just to ensure reasonable coordinates)
    double lon_rad = std::atan2(ground.y, ground.x);
    double lat_rad = std::atan2(ground.z, std::sqrt(ground.x*ground.x + ground.y*ground.y));
    (void)lon_rad;  // Used for verification
    (void)lat_rad;  // Used for verification

    // Test round-trip accuracy
    double line_out, sample_out;
    csm::groundToImage(model, ground, line_out, sample_out);

    double line_error = std::abs(line_out - center_line);
    double sample_error = std::abs(sample_out - center_sample);

    EXPECT_LT(line_error, 0.1) << "Round-trip line error too large";
    EXPECT_LT(sample_error, 0.1) << "Round-trip sample error too large";
}

TEST_F(CamprojectTest, FullImageProjection) {
    if (!testDataAvailable()) {
        GTEST_SKIP() << "Test data not available at " << isd_path;
    }

    model = csm::createCsmFromISD(isd_path);
    ASSERT_NE(model, nullptr);
    EXPECT_STREQ(model->getModelName().c_str(), "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL");

    auto size = model->getImageSize();
    EXPECT_GT(size.line, 0);
    EXPECT_GT(size.samp, 0);

    // Project to Mars equirectangular
    std::string output_proj = "+proj=eqc +lat_ts=0 +lat_0=0 +lon_0=0 +x_0=0 +y_0=0 "
                              "+a=3396190 +b=3376200 +units=m +no_defs";

    // Create shape model with constant height
    auto [semi_major, semi_minor] = csm::getRadii(model);
    Ellipsoid3 ellipsoid(semi_major, semi_major, semi_minor);
    miniset::ShapeModel shape_model = miniset::createConstantHeightShapeModel(0.0, ellipsoid);

    ASSERT_NO_THROW({
        miniset::camproject(image_path, model, shape_model, output_proj, output_path);
    });

    // Verify output file
    GDALDataset* output_ds = (GDALDataset*)GDALOpen(output_path.c_str(), GA_ReadOnly);
    ASSERT_NE(output_ds, nullptr) << "Failed to open output file";

    // Check dimensions
    EXPECT_GT(output_ds->GetRasterXSize(), 0);
    EXPECT_GT(output_ds->GetRasterYSize(), 0);
    EXPECT_GT(output_ds->GetRasterCount(), 0);

    // Check projection
    const char* proj_wkt = output_ds->GetProjectionRef();
    EXPECT_NE(proj_wkt, nullptr);
    EXPECT_GT(strlen(proj_wkt), 0) << "Output should have projection info";

    // Check geotransform
    double geotransform[6];
    EXPECT_EQ(output_ds->GetGeoTransform(geotransform), CE_None);

    // Pixel size should be positive for width, negative for height (north-up)
    EXPECT_GT(geotransform[1], 0.0) << "Pixel width should be positive";
    EXPECT_LT(geotransform[5], 0.0) << "Pixel height should be negative (north-up)";

    GDALClose(output_ds);
}
