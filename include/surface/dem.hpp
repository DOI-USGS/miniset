#ifndef MINISET_SURFACE_DEM_HPP
#define MINISET_SURFACE_DEM_HPP

#include "surface/ellipsoid.hpp"
#include <string>

// Forward declarations to avoid including GDAL headers
// (which trigger PROJ initialization that crashes in WASM during Embind processing)
class GDALDataset;  // Forward declaration

namespace surface {

// DEM type
enum class DEMType {
    HEIGHT,  // Values are heights above ellipsoid
    RADIUS   // Values are radii from center
};

// Base ellipsoid DEM (no raster data)
class EllipsoidDEM {
public:
    EllipsoidDEM(double semi_major, double semi_minor = 0.0);
    virtual ~EllipsoidDEM() = default;

    // Get height above ellipsoid (degrees)
    virtual double getHeight(double lat_deg, double lon_deg) const;

    // Get radius from center (degrees)
    virtual double getRadius(double lat_deg, double lon_deg) const;

    double getSemiMajorA() const { return a_; }
    double getSemiMajorB() const { return b_; }
    double getSemiMinorC() const { return c_; }

protected:
    double a_, b_, c_;  // Ellipsoid radii
};

// GDAL-backed DEM
class GdalDEM : public EllipsoidDEM {
public:
    GdalDEM(const std::string& dem_file, DEMType dem_type = DEMType::HEIGHT);

    GdalDEM(const std::string& dem_file, DEMType dem_type, double semi_major, double semi_minor);

    ~GdalDEM();

    void setProj(const std::string& proj_string);

    // Set ellipsoid parameters directly (for WASM + proj4js workflow)
    void setEllipsoid(double semi_major, double semi_minor);

    // Disable copy (GDAL dataset is not copyable)
    GdalDEM(const GdalDEM&) = delete;
    GdalDEM& operator=(const GdalDEM&) = delete;

    // Get height above ellipsoid (degrees)
    double getHeight(double lat_deg, double lon_deg) const override;

    // Get radius from center (degrees)
    double getRadius(double lat_deg, double lon_deg) const override;

    // Get raw raster value
    double getRasterValue(double lat_rad, double lon_rad) const;

private:
    GDALDataset* dataset_;
    DEMType dem_type_;
    double no_data_value_;
    double geotransform_[6];

    void latLonToPixel(double lat_rad, double lon_rad, int& pixel_x, int& pixel_y) const;

    // Helper methods to extract ellipsoid parameters from GeoTIFF metadata
    static double extractSemiMajor(const std::string& dem_file);
    static double extractSemiMinor(const std::string& dem_file);
};

} // namespace surface

#endif // MINISET_SURFACE_DEM_HPP
