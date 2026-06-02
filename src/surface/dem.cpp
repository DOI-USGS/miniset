#include "surface/dem.hpp"
#include <cmath>
#include <stdexcept>

// Include GDAL headers here (not in header file to avoid triggering
// PROJ initialization during Embind processing in WASM)
#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <ogr_spatialref.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Forward declare GDAL/PROJ configuration function from bindings
extern "C" void configure_gdal_proj();
#endif

namespace surface {

EllipsoidDEM::EllipsoidDEM(double semi_major, double semi_minor)
    : a_(semi_major), b_(semi_major), c_(semi_minor > 0 ? semi_minor : semi_major) {
    if (a_ <= 0 || c_ <= 0) {
        throw std::invalid_argument("Ellipsoid radii must be positive");
    }
}

double EllipsoidDEM::getHeight(double lat_deg, double lon_deg) const {
    // Pure ellipsoid has no height above itself
    return 0.0;
}

double EllipsoidDEM::getRadius(double lat_deg, double lon_deg) const {
    // Convert degrees to radians
    double lat_rad = lat_deg * M_PI / 180.0;
    double lon_rad = lon_deg * M_PI / 180.0;

    // Same calculation as Ellipsoid::getRadiusAt
    double cos_lat = std::cos(lat_rad);
    double sin_lat = std::sin(lat_rad);
    double cos_lon = std::cos(lon_rad);
    double sin_lon = std::sin(lon_rad);

    double bc_cos_lat_cos_lon = b_ * c_ * cos_lat * cos_lon;
    double ac_cos_lat_sin_lon = a_ * c_ * cos_lat * sin_lon;
    double ab_sin_lat = a_ * b_ * sin_lat;

    double denom = std::sqrt(
        bc_cos_lat_cos_lon * bc_cos_lat_cos_lon +
        ac_cos_lat_sin_lon * ac_cos_lat_sin_lon +
        ab_sin_lat * ab_sin_lat
    );

    if (denom < 1e-10) {
        return a_;
    }

    return (a_ * b_ * c_) / denom;
}

GdalDEM::GdalDEM(const std::string& dem_file, DEMType dem_type)
    : EllipsoidDEM(6378137.0, 6356752.314245),  // Default WGS84, will be overwritten if SRS available
      dataset_(nullptr), dem_type_(dem_type), no_data_value_(0.0) {

#ifdef __EMSCRIPTEN__
    // Configure GDAL/PROJ after embedded files are loaded (lazy initialization)
    configure_gdal_proj();

    // Enable GDAL error logging
    CPLSetConfigOption("CPL_DEBUG", "ON");
    EM_ASM({ console.log('Opening GDAL file: ' + UTF8ToString($0)); }, dem_file.c_str());
#else
    GDALAllRegister();
#endif

    dataset_ = (GDALDataset*)GDALOpen(dem_file.c_str(), GA_ReadOnly);

    if (!dataset_) {
#ifdef __EMSCRIPTEN__
        const char* err = CPLGetLastErrorMsg();
        EM_ASM({ console.error('GDALOpen failed: ' + UTF8ToString($0)); }, err);
#endif
        throw std::runtime_error("Failed to open DEM file: " + dem_file);
    }

    // Extract ellipsoid parameters from spatial reference
#ifdef __EMSCRIPTEN__
    // WASM: Use C API (GetProjectionRef now works with proper exception handling!)
    const char* wkt = GDALGetProjectionRef((GDALDatasetH)dataset_);
    if (wkt && strlen(wkt) > 0) {
        EM_ASM({ console.log('Got WKT from GDALGetProjectionRef'); });

        // Create OGRSpatialReference from WKT to extract ellipsoid
        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(wkt);
        if (hSRS) {
            double semi_major = OSRGetSemiMajor(hSRS, nullptr);
            double semi_minor = OSRGetSemiMinor(hSRS, nullptr);

            if (semi_major > 0) {
                a_ = semi_major;
                b_ = semi_major;
                if (semi_minor > 0) {
                    c_ = semi_minor;
                } else {
                    c_ = semi_major;  // Sphere
                }
                EM_ASM({
                    console.log('Extracted ellipsoid from spatial reference: a=' + $0 + ' m, c=' + $1 + ' m');
                }, a_, c_);
            }
            OSRDestroySpatialReference(hSRS);
        }
    } else {
        EM_ASM({ console.warn('No spatial reference found in DEM, using default WGS84'); });
    }
#else
    // Native builds - use C++ API
    const OGRSpatialReference* srs = dataset_->GetSpatialRef();
    if (srs) {
        OGRErr err;
        double semi_major = srs->GetSemiMajor(&err);
        if (err == OGRERR_NONE && semi_major > 0) {
            a_ = semi_major;
            b_ = semi_major;
        }

        double semi_minor = srs->GetSemiMinor(&err);
        if (err == OGRERR_NONE && semi_minor > 0) {
            c_ = semi_minor;
        }
    }
#endif

    // Get geotransform
    CPLErr geoerr = dataset_->GetGeoTransform(geotransform_);
    if (geoerr != CE_None) {
        std::string error_msg = "Failed to get geotransform from DEM: " + dem_file;
        if (geoerr == CE_Failure) {
            error_msg += " (CE_Failure - file may not have georeference info)";
        } else if (geoerr == CE_Fatal) {
            error_msg += " (CE_Fatal)";
        }
        GDALClose(dataset_);
        dataset_ = nullptr;
        throw std::runtime_error(error_msg);
    }

    // Get no-data value
    GDALRasterBand* band = dataset_->GetRasterBand(1);
    if (band) {
        int has_no_data;
        no_data_value_ = band->GetNoDataValue(&has_no_data);
        if (!has_no_data) {
            no_data_value_ = -9999.0;  // Default
        }
    }
}

// Constructor with explicit ellipsoid parameters (for WASM + proj4js workflow)
GdalDEM::GdalDEM(const std::string& dem_file, DEMType dem_type, double semi_major, double semi_minor)
    : EllipsoidDEM(semi_major, semi_minor),
      dataset_(nullptr), dem_type_(dem_type), no_data_value_(0.0) {

#ifdef __EMSCRIPTEN__
    configure_gdal_proj();
    CPLSetConfigOption("CPL_DEBUG", "ON");
    EM_ASM({ console.log('Opening GDAL file with explicit ellipsoid: ' + UTF8ToString($0)); }, dem_file.c_str());
#else
    GDALAllRegister();
#endif

    dataset_ = (GDALDataset*)GDALOpen(dem_file.c_str(), GA_ReadOnly);

    if (!dataset_) {
#ifdef __EMSCRIPTEN__
        const char* err = CPLGetLastErrorMsg();
        EM_ASM({ console.error('GDALOpen failed: ' + UTF8ToString($0)); }, err);
#endif
        throw std::runtime_error("Failed to open DEM file: " + dem_file);
    }

    // Ellipsoid already set in constructor - skip spatial reference extraction

    // Get geotransform
    CPLErr geoerr = dataset_->GetGeoTransform(geotransform_);
    if (geoerr != CE_None) {
        std::string error_msg = "Failed to get geotransform from DEM: " + dem_file;
        if (geoerr == CE_Failure) {
            error_msg += " (CE_Failure - file may not have georeference info)";
        } else if (geoerr == CE_Fatal) {
            error_msg += " (CE_Fatal)";
        }
        GDALClose(dataset_);
        dataset_ = nullptr;
        throw std::runtime_error(error_msg);
    }

    // Get no-data value
    GDALRasterBand* band = dataset_->GetRasterBand(1);
    if (band) {
        int has_no_data;
        no_data_value_ = band->GetNoDataValue(&has_no_data);
        if (!has_no_data) {
            no_data_value_ = -9999.0;
        }
    }
}

// Set ellipsoid parameters directly
void GdalDEM::setEllipsoid(double semi_major, double semi_minor) {
    if (semi_major <= 0 || semi_minor <= 0) {
        throw std::invalid_argument("Ellipsoid radii must be positive");
    }
    a_ = semi_major;
    b_ = semi_major;  // Triaxial ellipsoid
    c_ = semi_minor;

#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[WASM] Ellipsoid set: a=' + $0 + ', c=' + $1); }, semi_major, semi_minor);
#endif
}

double GdalDEM::extractSemiMajor(const std::string& dem_file) {
    // This method is no longer used but kept for API compatibility
    GDALAllRegister();
    GDALDataset* temp_ds = (GDALDataset*)GDALOpen(dem_file.c_str(), GA_ReadOnly);

    if (!temp_ds) {
        return 6378137.0;  // Return default instead of throwing
    }

    const OGRSpatialReference* srs = temp_ds->GetSpatialRef();
    double semi_major = 6378137.0;  // Default to WGS84

    if (srs) {
        OGRErr err;
        semi_major = srs->GetSemiMajor(&err);
        if (err != OGRERR_NONE) {
            semi_major = 6378137.0;  // Fallback to WGS84
        }
    }

    GDALClose(temp_ds);
    return semi_major;
}

double GdalDEM::extractSemiMinor(const std::string& dem_file) {
    // This method is no longer used but kept for API compatibility
    GDALAllRegister();
    GDALDataset* temp_ds = (GDALDataset*)GDALOpen(dem_file.c_str(), GA_ReadOnly);

    if (!temp_ds) {
        return 6356752.314245;  // Return default instead of throwing
    }

    const OGRSpatialReference* srs = temp_ds->GetSpatialRef();
    double semi_minor = 6356752.314245;  // Default to WGS84

    if (srs) {
        OGRErr err;
        semi_minor = srs->GetSemiMinor(&err);
        if (err != OGRERR_NONE) {
            semi_minor = 6356752.314245;  // Fallback to WGS84
        }
    }

    GDALClose(temp_ds);
    return semi_minor;
}

GdalDEM::~GdalDEM() {
    if (dataset_) {
        GDALClose(dataset_);
    }
}

void GdalDEM::setProj(const std::string& proj_string) {
    // Use OSR C API for both native and WASM builds
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    if (!srs) {
        throw std::runtime_error("Failed to create spatial reference");
    }

    OGRErr err = OSRSetFromUserInput(srs, proj_string.c_str());
    if (err != OGRERR_NONE) {
        OSRDestroySpatialReference(srs);
        throw std::runtime_error("Failed to parse PROJ string: " + proj_string);
    }

    // Extract ellipsoid parameters
    double semi_major = OSRGetSemiMajor(srs, &err);
    if (err == OGRERR_NONE && semi_major > 0) {
        a_ = semi_major;
        b_ = semi_major;
    }

    double semi_minor = OSRGetSemiMinor(srs, &err);
    if (err == OGRERR_NONE && semi_minor > 0) {
        c_ = semi_minor;
    }

    OSRDestroySpatialReference(srs);
}

void GdalDEM::latLonToPixel(double lat_rad, double lon_rad, int& pixel_x, int& pixel_y) const {
    // Convert radians to degrees
    double lon_deg = lon_rad * 180.0 / M_PI;
    double lat_deg = lat_rad * 180.0 / M_PI;

    // Apply inverse geotransform
    // geotransform: [0]=top-left x, [1]=pixel width, [2]=rotation, [3]=top-left y, [4]=rotation, [5]=pixel height
    double det = geotransform_[1] * geotransform_[5] - geotransform_[2] * geotransform_[4];

    if (std::abs(det) < 1e-10) {
        pixel_x = pixel_y = -1;
        return;
    }

    double dx = lon_deg - geotransform_[0];
    double dy = lat_deg - geotransform_[3];

    pixel_x = static_cast<int>((geotransform_[5] * dx - geotransform_[2] * dy) / det);
    pixel_y = static_cast<int>((geotransform_[1] * dy - geotransform_[4] * dx) / det);
}

double GdalDEM::getRasterValue(double lat_rad, double lon_rad) const {
    int pixel_x, pixel_y;
    latLonToPixel(lat_rad, lon_rad, pixel_x, pixel_y);

    // Check bounds
    int width = dataset_->GetRasterXSize();
    int height = dataset_->GetRasterYSize();

    if (pixel_x < 0 || pixel_x >= width || pixel_y < 0 || pixel_y >= height) {
        return no_data_value_;
    }

    // Read pixel value
    GDALRasterBand* band = dataset_->GetRasterBand(1);
    if (!band) {
        return no_data_value_;
    }

    float value;
    CPLErr err = band->RasterIO(GF_Read, pixel_x, pixel_y, 1, 1, &value, 1, 1, GDT_Float32, 0, 0);

    if (err != CE_None) {
        return no_data_value_;
    }

    // Check for no-data
    if (std::abs(value - no_data_value_) < 1e-6) {
        return no_data_value_;
    }

    return static_cast<double>(value);
}

double GdalDEM::getHeight(double lat_deg, double lon_deg) const {
    // Convert degrees to radians for internal calculations
    double lat_rad = lat_deg * M_PI / 180.0;
    double lon_rad = lon_deg * M_PI / 180.0;

    double raster_value = getRasterValue(lat_rad, lon_rad);

    if (std::abs(raster_value - no_data_value_) < 1e-6) {
        return 0.0;  // No data - return 0 height
    }

    if (dem_type_ == DEMType::HEIGHT) {
        return raster_value;
    } else {  // DEMType::RADIUS
        double ellipsoid_radius = EllipsoidDEM::getRadius(lat_deg, lon_deg);
        return raster_value - ellipsoid_radius;
    }
}

double GdalDEM::getRadius(double lat_deg, double lon_deg) const {
    // Convert degrees to radians for internal calculations
    double lat_rad = lat_deg * M_PI / 180.0;
    double lon_rad = lon_deg * M_PI / 180.0;

    double raster_value = getRasterValue(lat_rad, lon_rad);

    if (std::abs(raster_value - no_data_value_) < 1e-6) {
        return EllipsoidDEM::getRadius(lat_deg, lon_deg);  // No data - return ellipsoid radius
    }

    if (dem_type_ == DEMType::RADIUS) {
        return raster_value;
    } else {  // DEMType::HEIGHT
        double ellipsoid_radius = EllipsoidDEM::getRadius(lat_deg, lon_deg);
        return ellipsoid_radius + raster_value;
    }
}

} // namespace surface
