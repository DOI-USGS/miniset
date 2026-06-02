#include "csm/camera_ops.hpp"
#include "csm/csm_interface.hpp"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>
#include <proj.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <vector>
#include <cstring>

namespace csm {
Vec3 imageToGround(const ::csm::RasterGM* sensor, double line, double sample, double height) {
    ::csm::ImageCoord img(line, sample);
    ::csm::EcefCoord ground = sensor->imageToGround(img, height);
    return Vec3(ground.x, ground.y, ground.z);
}

void groundToImage(const ::csm::RasterGM* sensor, const Vec3& ground_pt, double& line, double& sample) {
    ::csm::EcefCoord pt(ground_pt.x, ground_pt.y, ground_pt.z);
    ::csm::ImageCoord img = sensor->groundToImage(pt);
    line = img.line;
    sample = img.samp;
}

// Helper function to create ECEF WKT
std::string createECEFWKT(double semi_major, double semi_minor) {
    double inv_flattening = (semi_major - semi_minor) > 1e-10 ?
                           semi_major / (semi_major - semi_minor) : 0.0;

    std::ostringstream wkt;
    wkt << "GEOCCS[\"Custom ECEF\","
        << "DATUM[\"Custom\",SPHEROID[\"Custom\","
        << std::fixed << std::setprecision(3)
        << semi_major << "," << inv_flattening << "]],"
        << "PRIMEM[\"Greenwich\",0],"
        << "UNIT[\"metre\",1]]";
    return wkt.str();
}

} // namespace csm

// Forward declare WASM configuration function
#ifdef __EMSCRIPTEN__
extern "C" void configure_gdal_proj();
#endif

namespace miniset {

void camproject(
    const std::string& input_image_path,
    ::csm::RasterGM* camera_model,
    const miniset::ShapeModel& shape_model,
    const std::string& output_proj_string,
    const std::string& output_path
) {
    // Quadtree structure for efficient limb detection
    struct QuadNode {
        int min_line, max_line, min_sample, max_sample;
        bool all_valid;
        bool all_invalid;
        bool subdivided;
        std::vector<QuadNode> children;
    };

    // 1. Register GDAL drivers and configure PROJ/GDAL (for WASM)
#ifdef __EMSCRIPTEN__
    // For WASM: ensure embedded files are configured before using PROJ
    configure_gdal_proj();
#endif

    GDALAllRegister();

    GDALDataset* input_ds = (GDALDataset*)GDALOpen(input_image_path.c_str(), GA_ReadOnly);
    if (!input_ds) {
        throw std::runtime_error("Failed to open input image: " + input_image_path);
    }

    int input_width = input_ds->GetRasterXSize();
    int input_height = input_ds->GetRasterYSize();
    GDALDataType dtype = input_ds->GetRasterBand(1)->GetRasterDataType();
    int num_bands = input_ds->GetRasterCount();

    // 2. Get camera ellipsoid and setup PROJ
    auto [camera_semi_major, camera_semi_minor] = csm::getRadii(camera_model);

    // Create ECEF WKT for source CRS
    std::string ecef_wkt = csm::createECEFWKT(camera_semi_major, camera_semi_minor);

    PJ_CONTEXT* ctx = proj_context_create();
    if (!ctx) {
        GDALClose(input_ds);
        throw std::runtime_error("Failed to create PROJ context");
    }

    // For WASM: set PROJ search paths on this context
#ifdef __EMSCRIPTEN__
    const char* proj_search_paths[] = {"/usr/share/proj", nullptr};
    proj_context_set_search_paths(ctx, 1, proj_search_paths);
#endif

    // 3. Build quadtree for limb detection
    // For simplicity, use approximate height from shape model at image center
    // TODO: Implement proper per-pixel height queries for DEM models
    double approx_height = 0.0;
    if (shape_model.type == miniset::ShapeModelType::CONSTANT_HEIGHT) {
        approx_height = shape_model.data.constant_height;
    } else if (shape_model.type == miniset::ShapeModelType::DEM) {
        // Use center of image for approximate height
        approx_height = miniset::getShapeModelHeight(shape_model, 0.0, 0.0);
    }

    auto testIntersection = [&](int line, int sample) -> bool {
        try {
            (void)imageToGround(camera_model, line, sample, approx_height);
            return true;
        } catch (...) {
            return false;
        }
    };

    std::function<void(QuadNode&, int)> subdivideQuad = [&](QuadNode& node, int max_depth) {
        if (max_depth <= 0) return;

        bool tl = testIntersection(node.min_line, node.min_sample);
        bool tr = testIntersection(node.min_line, node.max_sample);
        bool bl = testIntersection(node.max_line, node.min_sample);
        bool br = testIntersection(node.max_line, node.max_sample);

        if (tl && tr && bl && br) {
            node.all_valid = true;
            node.all_invalid = false;
            return;
        }

        if (!tl && !tr && !bl && !br) {
            int mid_line = (node.min_line + node.max_line) / 2;
            int mid_sample = (node.min_sample + node.max_sample) / 2;
            if (!testIntersection(mid_line, mid_sample)) {
                node.all_invalid = true;
                node.all_valid = false;
                return;
            }
        }

        int width = node.max_sample - node.min_sample;
        int height = node.max_line - node.min_line;

        if (width <= 4 || height <= 4) {
            node.all_valid = false;
            node.all_invalid = false;
            return;
        }

        int mid_line = (node.min_line + node.max_line) / 2;
        int mid_sample = (node.min_sample + node.max_sample) / 2;

        node.subdivided = true;
        node.children.resize(4);

        node.children[0] = {node.min_line, mid_line, node.min_sample, mid_sample, false, false, false, {}};
        node.children[1] = {node.min_line, mid_line, mid_sample, node.max_sample, false, false, false, {}};
        node.children[2] = {mid_line, node.max_line, node.min_sample, mid_sample, false, false, false, {}};
        node.children[3] = {mid_line, node.max_line, mid_sample, node.max_sample, false, false, false, {}};

        for (auto& child : node.children) {
            subdivideQuad(child, max_depth - 1);
        }
    };

    QuadNode root = {0, input_height, 0, input_width, false, false, false, {}};
    subdivideQuad(root, 10);

    // 4. Collect valid ECEF points for footprint
    std::vector<Vec3> valid_ecef_points;

    std::function<void(const QuadNode&)> collectValidPoints = [&](const QuadNode& node) {
        if (node.all_invalid) return;

        if (node.all_valid || !node.subdivided) {
            std::vector<std::pair<int,int>> sample_points = {
                {node.min_line, node.min_sample},
                {node.min_line, node.max_sample},
                {node.max_line, node.min_sample},
                {node.max_line, node.max_sample},
                {(node.min_line + node.max_line)/2, (node.min_sample + node.max_sample)/2}
            };

            for (const auto& [line, sample] : sample_points) {
                try {
                    Vec3 ecef = imageToGround(camera_model, line, sample, approx_height);
                    valid_ecef_points.push_back(ecef);
                } catch (...) {}
            }
        } else {
            for (const auto& child : node.children) {
                collectValidPoints(child);
            }
        }
    };

    collectValidPoints(root);

    if (valid_ecef_points.size() < 3) {
        proj_context_destroy(ctx);
        GDALClose(input_ds);
        throw std::runtime_error("Insufficient valid points - image may be entirely off-limb");
    }

    // 5. Transform to output CRS and compute bounds
    // Use proj_create_crs_to_crs which accepts strings directly
    PJ* transform = proj_create_crs_to_crs(ctx, ecef_wkt.c_str(), output_proj_string.c_str(), nullptr);
    if (!transform) {
        proj_context_destroy(ctx);
        GDALClose(input_ds);
        throw std::runtime_error("Failed to create coordinate transformation");
    }

    PJ* normalized = proj_normalize_for_visualization(ctx, transform);
    proj_destroy(transform);
    transform = normalized;

    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    for (const auto& ecef : valid_ecef_points) {
        PJ_COORD input = proj_coord(ecef.x, ecef.y, ecef.z, 0);
        PJ_COORD output = proj_trans(transform, PJ_FWD, input);

        if (output.v[0] != HUGE_VAL && output.v[1] != HUGE_VAL) {
            min_x = std::min(min_x, output.v[0]);
            max_x = std::max(max_x, output.v[0]);
            min_y = std::min(min_y, output.v[1]);
            max_y = std::max(max_y, output.v[1]);
        }
    }

    proj_destroy(transform);

    // 6. Determine output resolution
    Vec3 center_gnd = imageToGround(camera_model, input_height/2.0, input_width/2.0, approx_height);
    Vec3 center_right = imageToGround(camera_model, input_height/2.0, input_width/2.0 + 1, approx_height);
    Vec3 center_down = imageToGround(camera_model, input_height/2.0 + 1, input_width/2.0, approx_height);

    // Create another transform for pixel size calculation
    PJ* transform2 = proj_create_crs_to_crs(ctx, ecef_wkt.c_str(), output_proj_string.c_str(), nullptr);
    PJ* normalized2 = proj_normalize_for_visualization(ctx, transform2);
    proj_destroy(transform2);
    transform2 = normalized2;

    PJ_COORD center_in = proj_coord(center_gnd.x, center_gnd.y, center_gnd.z, 0);
    PJ_COORD center_out = proj_trans(transform2, PJ_FWD, center_in);

    PJ_COORD right_in = proj_coord(center_right.x, center_right.y, center_right.z, 0);
    PJ_COORD right_out = proj_trans(transform2, PJ_FWD, right_in);

    PJ_COORD down_in = proj_coord(center_down.x, center_down.y, center_down.z, 0);
    PJ_COORD down_out = proj_trans(transform2, PJ_FWD, down_in);

    proj_destroy(transform2);

    double pixel_width = std::sqrt(
        std::pow(right_out.v[0] - center_out.v[0], 2) +
        std::pow(right_out.v[1] - center_out.v[1], 2)
    );
    double pixel_height = std::sqrt(
        std::pow(down_out.v[0] - center_out.v[0], 2) +
        std::pow(down_out.v[1] - center_out.v[1], 2)
    );

    int output_width = static_cast<int>((max_x - min_x) / pixel_width) + 1;
    int output_height = static_cast<int>((max_y - min_y) / pixel_height) + 1;

    output_width = std::clamp(output_width, 1, 100000);
    output_height = std::clamp(output_height, 1, 100000);

    // 7. Create output dataset
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!driver) {
        proj_context_destroy(ctx);
        GDALClose(input_ds);
        throw std::runtime_error("GTiff driver not available");
    }

    const char* options[] = {"COMPRESS=LZW", "TILED=YES", nullptr};
    GDALDataset* output_ds = driver->Create(output_path.c_str(),
                                           output_width, output_height,
                                           num_bands, dtype, (char**)options);

    if (!output_ds) {
        proj_context_destroy(ctx);
        GDALClose(input_ds);
        throw std::runtime_error("Failed to create output dataset");
    }

    double geotransform[6] = {
        min_x,
        pixel_width,
        0.0,
        max_y,
        0.0,
        -pixel_height
    };
    output_ds->SetGeoTransform(geotransform);

    // Set projection using OGRSpatialReference
    OGRSpatialReference srs;
    if (srs.SetFromUserInput(output_proj_string.c_str()) == OGRERR_NONE) {
        char* wkt = nullptr;
        srs.exportToWkt(&wkt);
        if (wkt) {
            output_ds->SetProjection(wkt);
            CPLFree(wkt);
        }
    }

    // 8. Use GDAL's GCP-based polynomial warp (simple and fast)
    // Collect dense grid of GCPs mapping input pixels to output projected coordinates

    // Create transform for GCP collection (ECEF -> output projection)
    PJ* gcp_transform = proj_create_crs_to_crs(ctx, ecef_wkt.c_str(), output_proj_string.c_str(), nullptr);
    if (!gcp_transform) {
        proj_context_destroy(ctx);
        GDALClose(output_ds);
        GDALClose(input_ds);
        throw std::runtime_error("Failed to create GCP transformation");
    }
    PJ* gcp_normalized = proj_normalize_for_visualization(ctx, gcp_transform);
    proj_destroy(gcp_transform);
    gcp_transform = gcp_normalized;

    std::vector<GDAL_GCP> gcps;
    int gcp_id = 0;

    // Sample GCPs in a regular grid across the image
    int gcp_step_x = std::max(1, input_width / 10);   // ~10 GCPs across width
    int gcp_step_y = std::max(1, input_height / 10);  // ~10 GCPs across height

    for (int line = 0; line < input_height; line += gcp_step_y) {
        for (int sample = 0; sample < input_width; sample += gcp_step_x) {
            try {
                // Get ground point from CSM
                Vec3 ecef = imageToGround(camera_model, line, sample, approx_height);

                // Transform ECEF to output projection
                PJ_COORD ecef_pt = proj_coord(ecef.x, ecef.y, ecef.z, 0);
                PJ_COORD proj_pt = proj_trans(gcp_transform, PJ_FWD, ecef_pt);

                if (proj_pt.v[0] != HUGE_VAL && proj_pt.v[1] != HUGE_VAL) {
                    GDAL_GCP gcp;
                    GDALInitGCPs(1, &gcp);
                    gcp.dfGCPPixel = sample;
                    gcp.dfGCPLine = line;
                    gcp.dfGCPX = proj_pt.v[0];
                    gcp.dfGCPY = proj_pt.v[1];
                    gcp.dfGCPZ = 0.0;
                    snprintf(gcp.pszId, sizeof(gcp.pszId), "%d", gcp_id++);
                    gcps.push_back(gcp);
                }
            } catch (...) {
                // Skip limb pixels
            }
        }
    }

    // Calculate actual GCP bounds (more accurate than quadtree boundary samples)
    double gcp_min_x = std::numeric_limits<double>::infinity();
    double gcp_max_x = -std::numeric_limits<double>::infinity();
    double gcp_min_y = std::numeric_limits<double>::infinity();
    double gcp_max_y = -std::numeric_limits<double>::infinity();
    for (const auto& gcp : gcps) {
        gcp_min_x = std::min(gcp_min_x, gcp.dfGCPX);
        gcp_max_x = std::max(gcp_max_x, gcp.dfGCPX);
        gcp_min_y = std::min(gcp_min_y, gcp.dfGCPY);
        gcp_max_y = std::max(gcp_max_y, gcp.dfGCPY);
    }

    // Use GCP bounds for accurate extent
    min_x = gcp_min_x;
    max_x = gcp_max_x;
    min_y = gcp_min_y;
    max_y = gcp_max_y;

    // Recalculate output dimensions
    output_width = static_cast<int>(std::abs(max_x - min_x) / pixel_width) + 1;
    output_height = static_cast<int>(std::abs(max_y - min_y) / pixel_height) + 1;
    output_width = std::clamp(output_width, 1, 100000);
    output_height = std::clamp(output_height, 1, 100000);

    proj_destroy(gcp_transform);

    if (gcps.size() < 10) {
        for (auto& gcp : gcps) GDALDeinitGCPs(1, &gcp);
        proj_context_destroy(ctx);
        GDALClose(output_ds);
        GDALClose(input_ds);
        throw std::runtime_error("Insufficient GCPs for projection");
    }

    // Close and recreate output dataset with corrected dimensions
    GDALClose(output_ds);

    output_ds = driver->Create(output_path.c_str(),
                               output_width, output_height,
                               num_bands, dtype, (char**)options);

    if (!output_ds) {
        for (auto& gcp : gcps) GDALDeinitGCPs(1, &gcp);
        proj_context_destroy(ctx);
        GDALClose(input_ds);
        throw std::runtime_error("Failed to recreate output dataset");
    }

    // Set corrected geotransform
    geotransform[0] = min_x;
    geotransform[1] = pixel_width;
    geotransform[2] = 0.0;
    geotransform[3] = max_y;
    geotransform[4] = 0.0;
    geotransform[5] = -pixel_height;
    output_ds->SetGeoTransform(geotransform);

    // Set projection
    OGRSpatialReference srs_for_output;
    srs_for_output.SetFromUserInput(output_proj_string.c_str());
    char* output_wkt = nullptr;
    srs_for_output.exportToWkt(&output_wkt);
    if (output_wkt) {
        output_ds->SetProjection(output_wkt);
        CPLFree(output_wkt);
    }

    // Set GCPs on input dataset with output projection
    OGRSpatialReference out_srs;
    out_srs.SetFromUserInput(output_proj_string.c_str());
    char* wkt = nullptr;
    out_srs.exportToWkt(&wkt);

    input_ds->SetGCPs(gcps.size(), gcps.data(), wkt);
    CPLFree(wkt);

    // Create transformer using input dataset with GCPs -> output dataset
    void* transform_arg = GDALCreateGenImgProjTransformer(
        (GDALDatasetH)input_ds,
        nullptr,  // Use GCPs from input dataset
        (GDALDatasetH)output_ds,
        nullptr,  // Use projection from output dataset
        TRUE,     // Use GCPs
        0.0,      // No tolerance
        1         // Polynomial order
    );

    if (!transform_arg) {
        for (auto& gcp : gcps) GDALDeinitGCPs(1, &gcp);
        proj_context_destroy(ctx);
        GDALClose(output_ds);
        GDALClose(input_ds);
        throw std::runtime_error("Failed to create transformer");
    }

    // Setup warp operation
    GDALWarpOptions* psWarpOptions = GDALCreateWarpOptions();
    psWarpOptions->hSrcDS = (GDALDatasetH)input_ds;
    psWarpOptions->hDstDS = (GDALDatasetH)output_ds;
    psWarpOptions->nBandCount = num_bands;
    psWarpOptions->panSrcBands = (int*)CPLMalloc(sizeof(int) * num_bands);
    psWarpOptions->panDstBands = (int*)CPLMalloc(sizeof(int) * num_bands);
    for (int i = 0; i < num_bands; ++i) {
        psWarpOptions->panSrcBands[i] = i + 1;
        psWarpOptions->panDstBands[i] = i + 1;
    }
    psWarpOptions->pfnTransformer = GDALGenImgProjTransform;
    psWarpOptions->pTransformerArg = transform_arg;
    psWarpOptions->eResampleAlg = GRA_Bilinear;

    // Execute warp
    GDALWarpOperation oWarpOperation;
    oWarpOperation.Initialize(psWarpOptions);
    CPLErr err = oWarpOperation.ChunkAndWarpImage(0, 0, output_width, output_height);

    // Cleanup
    GDALDestroyGenImgProjTransformer(transform_arg);
    GDALDestroyWarpOptions(psWarpOptions);
    for (auto& gcp : gcps) GDALDeinitGCPs(1, &gcp);

    /*
    // OLD code kept for reference
    // 8. Collect GCPs from quadtree
    std::vector<GDAL_GCP> gcps;
    int gcp_id = 1;

    std::function<void(const QuadNode&)> collectGCPs = [&](const QuadNode& node) {
        if (node.all_invalid) return;

        if (node.all_valid || !node.subdivided) {
            std::vector<std::pair<int,int>> gcp_points = {
                {node.min_line, node.min_sample},
                {node.min_line, node.max_sample},
                {node.max_line, node.min_sample},
                {node.max_line, node.max_sample},
                {(node.min_line + node.max_line)/2, (node.min_sample + node.max_sample)/2}
            };

            for (const auto& [line, sample] : gcp_points) {
                try {
                    Vec3 ecef = imageToGround(camera_model, line, sample, approx_height);

                    GDAL_GCP gcp;
                    GDALInitGCPs(1, &gcp);
                    gcp.dfGCPPixel = sample;
                    gcp.dfGCPLine = line;
                    gcp.dfGCPX = ecef.x;
                    gcp.dfGCPY = ecef.y;
                    gcp.dfGCPZ = ecef.z;
                    snprintf(gcp.pszId, sizeof(gcp.pszId), "%d", gcp_id++);

                    gcps.push_back(gcp);
                } catch (...) {}
            }
        } else {
            for (const auto& child : node.children) {
                collectGCPs(child);
            }
        }
    };

    */

    // Cleanup
    if (err != CE_None) {
        proj_context_destroy(ctx);
        GDALClose(output_ds);
        GDALClose(input_ds);
        throw std::runtime_error("GDAL warp failed");
    }

    proj_context_destroy(ctx);
    GDALClose(output_ds);
    GDALClose(input_ds);
}

namespace internal {

// Quadtree node for limb detection
struct QuadNode {
    int min_line, max_line, min_sample, max_sample;
    bool all_valid;
    bool all_invalid;
    bool subdivided;
    std::vector<QuadNode> children;
};

// Convert ECEF to geodetic (lat/lon/height) - returns tuple for SOA
std::tuple<double, double, double> ecefToGeodetic(
    const Vec3& ecef,
    const Ellipsoid3& ellipsoid
) {
    // Convert ECEF to geodetic coordinates
    double x = ecef.x;
    double y = ecef.y;
    double z = ecef.z;

    // Longitude is straightforward
    double lon_rad = std::atan2(y, x);
    double lon_deg = lon_rad * 180.0 / M_PI;

    // Latitude and height require iterative solution
    // Using Bowring's method
    double a = ellipsoid.a;
    double c = ellipsoid.c;
    double e2 = 1.0 - (c * c) / (a * a);  // First eccentricity squared

    double p = std::sqrt(x * x + y * y);
    double theta = std::atan2(z * a, p * c);

    double lat_rad = std::atan2(
        z + e2 * c * std::pow(std::sin(theta), 3),
        p - e2 * a * std::pow(std::cos(theta), 3)
    );

    double lat_deg = lat_rad * 180.0 / M_PI;

    double N = a / std::sqrt(1.0 - e2 * std::sin(lat_rad) * std::sin(lat_rad));
    double height_m = p / std::cos(lat_rad) - N;

    return std::make_tuple(lat_deg, lon_deg, height_m);
}

} // namespace internal

// Generate image boundary in geodetic coordinates
Boundary generateBoundary(
    const ::csm::RasterGM* camera_model,
    int image_width,
    int image_height,
    const ShapeModel& shape_model,
    int num_edge_samples
) {
    // For now, sample the image edges (simple implementation)
    // TODO: Use quadtree for proper limb detection

    Boundary boundary;
    boundary.reserve(num_edge_samples * 4);

    // Sample top edge
    for (int i = 0; i < num_edge_samples; ++i) {
        double sample = (double)i / (num_edge_samples - 1) * (image_width - 1);
        double line = 0.0;

        try {
            double height = getShapeModelHeight(shape_model, 0.0, 0.0);  // Approximate
            Vec3 ecef = csm::imageToGround(camera_model, line, sample, height);
            auto [lat, lon, h] = internal::ecefToGeodetic(ecef, shape_model.ellipsoid);
            boundary.push_back(lat, lon, h);
        } catch (...) {
            // Skip points that fail
        }
    }

    // Sample right edge
    for (int i = 1; i < num_edge_samples; ++i) {
        double sample = image_width - 1;
        double line = (double)i / (num_edge_samples - 1) * (image_height - 1);

        try {
            double height = getShapeModelHeight(shape_model, 0.0, 0.0);  // Approximate
            Vec3 ecef = csm::imageToGround(camera_model, line, sample, height);
            auto [lat, lon, h] = internal::ecefToGeodetic(ecef, shape_model.ellipsoid);
            boundary.push_back(lat, lon, h);
        } catch (...) {
            // Skip points that fail
        }
    }

    // Sample bottom edge (reverse order)
    for (int i = num_edge_samples - 2; i >= 0; --i) {
        double sample = (double)i / (num_edge_samples - 1) * (image_width - 1);
        double line = image_height - 1;

        try {
            double height = getShapeModelHeight(shape_model, 0.0, 0.0);  // Approximate
            Vec3 ecef = csm::imageToGround(camera_model, line, sample, height);
            auto [lat, lon, h] = internal::ecefToGeodetic(ecef, shape_model.ellipsoid);
            boundary.push_back(lat, lon, h);
        } catch (...) {
            // Skip points that fail
        }
    }

    // Sample left edge (reverse order)
    for (int i = num_edge_samples - 2; i > 0; --i) {
        double sample = 0.0;
        double line = (double)i / (num_edge_samples - 1) * (image_height - 1);

        try {
            double height = getShapeModelHeight(shape_model, 0.0, 0.0);  // Approximate
            Vec3 ecef = csm::imageToGround(camera_model, line, sample, height);
            auto [lat, lon, h] = internal::ecefToGeodetic(ecef, shape_model.ellipsoid);
            boundary.push_back(lat, lon, h);
        } catch (...) {
            // Skip points that fail
        }
    }

    return boundary;
}

// Generate WKT POLYGON footprint
std::string generateFootprintWKT(
    const ::csm::RasterGM* camera_model,
    int image_width,
    int image_height,
    const ShapeModel& shape_model,
    int num_edge_samples
) {
    // Generate boundary points
    Boundary boundary = generateBoundary(
        camera_model, image_width, image_height, shape_model, num_edge_samples);

    if (boundary.empty()) {
        throw std::runtime_error("No valid boundary points found");
    }

    // Format as WKT POLYGON
    std::ostringstream wkt;
    wkt << std::fixed << std::setprecision(6);
    wkt << "POLYGON((";

    for (size_t i = 0; i < boundary.size(); ++i) {
        if (i > 0) wkt << ", ";
        wkt << boundary.lon_deg[i] << " " << boundary.lat_deg[i];
    }

    // Close polygon
    if (boundary.size() > 0) {
        wkt << ", " << boundary.lon_deg[0] << " " << boundary.lat_deg[0];
    }

    wkt << "))";
    return wkt.str();
}

} // namespace miniset
