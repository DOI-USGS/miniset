#include "surface/shape_model.hpp"
#include "surface/ellipsoid.hpp"
#include <cmath>
#include <stdexcept>

namespace miniset {

// Query height at geodetic coordinates (degrees)
double getShapeModelHeight(const ShapeModel& model, double lat_deg, double lon_deg) {
    switch (model.type) {
        case ShapeModelType::CONSTANT_HEIGHT:
            return model.data.constant_height;

        case ShapeModelType::DEM:
            return model.data.dem->getHeight(lat_deg, lon_deg);

        case ShapeModelType::TIN:
            throw std::runtime_error("TIN shape models not yet implemented");

        case ShapeModelType::POINT_CLOUD:
            throw std::runtime_error("Point cloud shape models not yet implemented");

        default:
            throw std::runtime_error("Unknown shape model type");
    }
}

// Query radius from center at geodetic coordinates (degrees)
double getShapeModelRadius(const ShapeModel& model, double lat_deg, double lon_deg) {
    switch (model.type) {
        case ShapeModelType::CONSTANT_HEIGHT: {
            // Convert degrees to radians for ellipsoid calculation
            double lat_rad = lat_deg * M_PI / 180.0;
            double lon_rad = lon_deg * M_PI / 180.0;
            // Use ellipsoid function to get radius, then add constant height
            double ellipsoid_radius = ellipsoid::getRadiusAt(
                model.ellipsoid, lat_rad, lon_rad);
            return ellipsoid_radius + model.data.constant_height;
        }

        case ShapeModelType::DEM:
            return model.data.dem->getRadius(lat_deg, lon_deg);

        case ShapeModelType::TIN:
            throw std::runtime_error("TIN shape models not yet implemented");

        case ShapeModelType::POINT_CLOUD:
            throw std::runtime_error("Point cloud shape models not yet implemented");

        default:
            throw std::runtime_error("Unknown shape model type");
    }
}

// Construction helpers
ShapeModel createConstantHeightShapeModel(double height, const Ellipsoid3& ellipsoid) {
    ShapeModel model;
    model.type = ShapeModelType::CONSTANT_HEIGHT;
    model.ellipsoid = ellipsoid;
    model.data.constant_height = height;
    return model;
}

ShapeModel createShapeModelFromDEM(surface::EllipsoidDEM* dem) {
    ShapeModel model;
    model.type = ShapeModelType::DEM;
    model.ellipsoid = Ellipsoid3(dem->getSemiMajorA(),
                                  dem->getSemiMajorB(),
                                  dem->getSemiMinorC());
    model.data.dem = dem;
    return model;
}

} // namespace miniset
