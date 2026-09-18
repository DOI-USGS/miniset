#ifndef MINISET_CSM_CAMERA_OPS_HPP
#define MINISET_CSM_CAMERA_OPS_HPP

#include "core/types.hpp"
#include "surface/shape_model.hpp"
#include <csm/RasterGM.h>
#include <vector>
#include <string>

namespace csm {

// Image to ground conversion
Vec3 imageToGround(const ::csm::RasterGM* sensor, double line, double sample, double height);

// Ground to image conversion
void groundToImage(const ::csm::RasterGM* sensor, const Vec3& ground_pt, double& line, double& sample);

} // namespace csm

namespace miniset {

// Boundary representation (Structure of Arrays)
// Optimized for cache coherency and SIMD operations
struct Boundary {
    std::vector<double> lat_deg;
    std::vector<double> lon_deg;
    std::vector<double> height_m;

    size_t size() const { return lat_deg.size(); }
    bool empty() const { return lat_deg.empty(); }

    void reserve(size_t n) {
        lat_deg.reserve(n);
        lon_deg.reserve(n);
        height_m.reserve(n);
    }

    void push_back(double lat, double lon, double height) {
        lat_deg.push_back(lat);
        lon_deg.push_back(lon);
        height_m.push_back(height);
    }

    void clear() {
        lat_deg.clear();
        lon_deg.clear();
        height_m.clear();
    }
};

// Generate image boundary in geodetic coordinates
Boundary generateBoundary(
    const ::csm::RasterGM* camera_model,
    int image_width,
    int image_height,
    const ShapeModel& shape_model,
    int num_edge_samples = 50
);

// Generate WKT POLYGON footprint
std::string generateFootprintWKT(
    const ::csm::RasterGM* camera_model,
    int image_width,
    int image_height,
    const ShapeModel& shape_model,
    int num_edge_samples = 50
);

// Project unprojected image using CSM camera model with shape model
// Writes georeferenced GeoTIFF to output_path
void camproject(
    const std::string& input_image_path,
    ::csm::RasterGM* camera_model,
    const ShapeModel& shape_model,
    const std::string& output_proj_string,
    const std::string& output_path
);

} // namespace miniset

#endif
