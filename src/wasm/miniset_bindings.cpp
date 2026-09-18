/**
 * WebAssembly bindings for Miniset using Emscripten Embind
 *
 * This file provides JavaScript API matching the C++ API structure.
 */

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/emscripten.h>

// Include miniset headers from include directory
#include "csm/csm_interface.hpp"
#include "csm/campt.hpp"
#include "csm/camera_ops.hpp"
#include "core/types.hpp"
#include "core/math.hpp"

// Include CSM API headers
#include <csm/RasterGM.h>
#include <csm/Isd.h>
#include <csm/Plugin.h>

// Include DEM support (GDAL required)
#include "surface/dem.hpp"
#include "surface/shape_model.hpp"
#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include <cpl_conv.h>

// Include PROJ for spatial reference support
#include <proj.h>

#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <cmath>
#include <fstream>

using namespace emscripten;

// Forward declarations
extern "C" void configure_gdal_proj();

// Helper function to check if plugin is registered
namespace {
    void ensurePluginRegistered() {
        const ::csm::Plugin* plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
        // Plugin check - no output needed
        (void)plugin;
    }
}

// Convert CamptInfo struct to JavaScript object
val camptInfoToJS(const csm::CamptInfo& info) {
    val result = val::object();

    // Input
    result.set("sample", info.sample);
    result.set("line", info.line);
    result.set("height", info.height);

    // Ground point (ECEF, meters)
    val ground = val::object();
    ground.set("x", info.ground_point.x);
    ground.set("y", info.ground_point.y);
    ground.set("z", info.ground_point.z);
    result.set("groundPoint", ground);

    // Lat/Lon (convert radians to degrees)
    result.set("planetocentricLat", info.planetocentric_lat * 180.0 / M_PI);
    result.set("planetographicLat", info.planetographic_lat * 180.0 / M_PI);
    result.set("positiveEastLon", info.positive_east_lon * 180.0 / M_PI);
    result.set("localRadius", info.local_radius);

    // Sensor geometry
    val sensor = val::object();
    sensor.set("x", info.sensor_position.x);
    sensor.set("y", info.sensor_position.y);
    sensor.set("z", info.sensor_position.z);
    result.set("sensorPosition", sensor);

    val look = val::object();
    look.set("x", info.look_direction.x);
    look.set("y", info.look_direction.y);
    look.set("z", info.look_direction.z);
    result.set("lookDirection", look);

    result.set("sensorTime", info.sensor_time);

    // Photometric angles (already in degrees)
    result.set("phaseAngle", info.phase_angle);
    result.set("emissionAngle", info.emission_angle);
    result.set("incidenceAngle", info.incidence_angle);
    result.set("northAzimuth", info.north_azimuth);

    // Resolution (meters/pixel)
    result.set("sampleResolution", info.sample_resolution);
    result.set("lineResolution", info.line_resolution);
    result.set("pixelResolution", info.pixel_resolution);
    result.set("obliqueResolution", info.oblique_resolution);

    // Distances (meters)
    result.set("slantDistance", info.slant_distance);
    result.set("targetCenterDistance", info.target_center_distance);
    result.set("spacecraftAltitude", info.spacecraft_altitude);

    // Sub-spacecraft point
    result.set("subSpacecraftLat", info.sub_spacecraft_lat * 180.0 / M_PI);
    result.set("subSpacecraftLon", info.sub_spacecraft_lon * 180.0 / M_PI);
    result.set("offNadirAngle", info.off_nadir_angle);
    result.set("spacecraftAzimuth", info.spacecraft_azimuth);
    result.set("subSpacecraftGroundAzimuth", info.sub_spacecraft_ground_azimuth);

    // Sun information (if available)
    if (info.solar_distance > 0) {
        val sun = val::object();
        sun.set("x", info.sun_position.x);
        sun.set("y", info.sun_position.y);
        sun.set("z", info.sun_position.z);
        result.set("sunPosition", sun);

        result.set("subSolarLat", info.sub_solar_lat * 180.0 / M_PI);
        result.set("subSolarLon", info.sub_solar_lon * 180.0 / M_PI);
        result.set("solarDistance", info.solar_distance);
        result.set("subSolarAzimuth", info.sub_solar_azimuth);
        result.set("subSolarGroundAzimuth", info.sub_solar_ground_azimuth);
        result.set("solarLongitude", info.solar_longitude * 180.0 / M_PI);
        result.set("localSolarTime", info.local_solar_time);
    }

    // Celestial coordinates (convert radians to degrees)
    result.set("rightAscension", info.right_ascension * 180.0 / M_PI);
    result.set("declination", info.declination * 180.0 / M_PI);

    // Look direction in different frames
    if (info.look_j2000.x != 0 || info.look_j2000.y != 0 || info.look_j2000.z != 0) {
        val lookJ2000 = val::object();
        lookJ2000.set("x", info.look_j2000.x);
        lookJ2000.set("y", info.look_j2000.y);
        lookJ2000.set("z", info.look_j2000.z);
        result.set("lookDirectionJ2000", lookJ2000);

        val lookCamera = val::object();
        lookCamera.set("x", info.look_camera.x);
        lookCamera.set("y", info.look_camera.y);
        lookCamera.set("z", info.look_camera.z);
        result.set("lookDirectionCamera", lookCamera);
    }

    return result;
}

// JavaScript wrapper for createCsmFromISD
uintptr_t createCsmFromISD_JS(const std::string& isd_json) {
    ensurePluginRegistered();
    ::csm::RasterGM* model = csm::createCsmFromISD(isd_json);
    if (!model) {
        throw std::runtime_error("Failed to create CSM model from ISD");
    }
    return reinterpret_cast<uintptr_t>(model);
}

// JavaScript wrapper for createCsmFromStateString
uintptr_t createCsmFromStateString_JS(const std::string& state_string) {
    ensurePluginRegistered();
    ::csm::RasterGM* model = csm::createCsmFromStateString(state_string);
    if (!model) {
        throw std::runtime_error("Failed to create CSM model from state string");
    }
    return reinterpret_cast<uintptr_t>(model);
}

// JavaScript wrapper for createCsmFromAttachedSpice
uintptr_t createCsmFromAttachedSpice_JS(const std::string& image_path) {
    // Ensure GDAL is initialized (required for reading image metadata)
    configure_gdal_proj();
    ensurePluginRegistered();

    try {
        // Call the C++ function - any exceptions will be automatically converted by Embind
        ::csm::RasterGM* model = csm::createCsmFromAttachedSpice(image_path);
        if (!model) {
            throw std::runtime_error("Failed to create CSM model from attached SPICE data");
        }
        return reinterpret_cast<uintptr_t>(model);
    } catch (const std::exception& e) {
        EM_ASM({ console.error('[WASM Binding] Caught std::exception: ' + UTF8ToString($0)); }, e.what());
        throw std::runtime_error(std::string("CSM creation failed: ") + e.what());
    } catch (...) {
        EM_ASM({ console.error('[WASM Binding] Caught unknown exception'); });
        throw std::runtime_error("CSM creation failed with unknown exception");
    }
}

// JavaScript wrapper for campt
val campt_JS(uintptr_t model_ptr, double sample, double line, double a, double b, double c, double height) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    Ellipsoid3 ellipsoid(a, b, c);
    csm::CamptInfo info = csm::campt(model, sample, line, ellipsoid, height);
    return camptInfoToJS(info);
}

// JavaScript wrapper for getRadii
val getRadii_JS(uintptr_t model_ptr) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    auto radii = csm::getRadii(model);
    val result = val::object();
    result.set("a", radii.first);
    result.set("c", radii.second);
    return result;
}

// JavaScript wrapper for imageToGround
val imageToGround_JS(uintptr_t model_ptr, double line, double sample, double height) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    ::csm::ImageCoord imagePt(line, sample);
    ::csm::EcefCoord groundPt = model->imageToGround(imagePt, height);

    val result = val::object();
    result.set("x", groundPt.x);
    result.set("y", groundPt.y);
    result.set("z", groundPt.z);
    return result;
}

// JavaScript wrapper for groundToImage
val groundToImage_JS(uintptr_t model_ptr, double x, double y, double z) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    ::csm::EcefCoord groundPt(x, y, z);
    ::csm::ImageCoord imagePt = model->groundToImage(groundPt);

    val result = val::object();
    result.set("line", imagePt.line);
    result.set("sample", imagePt.samp);
    return result;
}

// JavaScript wrapper for getImageSize
val getImageSize_JS(uintptr_t model_ptr) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    ::csm::ImageVector size = model->getImageSize();

    val result = val::object();
    result.set("lines", size.line);
    result.set("samples", size.samp);
    return result;
}

// JavaScript wrapper for getModelName
std::string getModelName_JS(uintptr_t model_ptr) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    return model->getModelName();
}

// JavaScript wrapper for getImageIdentifier
std::string getImageIdentifier_JS(uintptr_t model_ptr) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    return model->getImageIdentifier();
}

// JavaScript wrapper for getSensorState
val getSensorState_JS(uintptr_t model_ptr, double line, double sample) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    csm::SensorState state = csm::getSensorState(model, line, sample);

    val result = val::object();

    val pos = val::object();
    pos.set("x", state.sensor_pos.x);
    pos.set("y", state.sensor_pos.y);
    pos.set("z", state.sensor_pos.z);
    result.set("position", pos);

    val look = val::object();
    look.set("x", state.look_vec.x);
    look.set("y", state.look_vec.y);
    look.set("z", state.look_vec.z);
    result.set("lookVector", look);

    result.set("time", state.sensor_time);

    return result;
}

// JavaScript wrapper for getModelState
std::string getModelState_JS(uintptr_t model_ptr) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }
    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
    return model->getModelState();
}

// JavaScript wrapper for deleteModel
void deleteModel_JS(uintptr_t model_ptr) {
    if (model_ptr) {
        ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);
        delete model;
    }
}

// Module initialization function (called early, before embedded files are extracted)
EMSCRIPTEN_KEEPALIVE
extern "C" void miniset_module_init() {
    // Create /tmp directory in virtual filesystem for ISD files
    EM_ASM({
        try {
            FS.mkdir('/tmp');
        } catch (e) {
            // Directory already exists
        }
    });

    // Ensure plugin is registered
    ensurePluginRegistered();
}

// Configure GDAL/PROJ after embedded files are loaded (called lazily on first DEM creation)
static bool gdal_proj_configured = false;

EMSCRIPTEN_KEEPALIVE
extern "C" void configure_gdal_proj() {
    if (gdal_proj_configured) {
        return;  // Already configured
    }

    // Set PROJ paths to /usr/share/proj (matching gdal3.js convention)
    EM_ASM(
        ENV['PROJ_DATA'] = '/usr/share/proj';
        ENV['PROJ_LIB'] = '/usr/share/proj';
    );

    // Set via CPL config options (used by GDAL's PROJ integration)
    CPLSetConfigOption("PROJ_DATA", "/usr/share/proj");
    CPLSetConfigOption("PROJ_LIB", "/usr/share/proj");

    // Initialize PROJ
    PJ_CONTEXT* ctx = proj_context_create();
    if (ctx) {
        const char* search_paths[] = {"/usr/share/proj", nullptr};
        proj_context_set_search_paths(ctx, 1, search_paths);
    }

    // GDAL_DATA set to /usr/share/gdal (matching gdal3.js convention)
    EM_ASM(
        ENV['GDAL_DATA'] = '/usr/share/gdal';
        ENV['GDAL_NUM_THREADS'] = '0';  // Critical: Disable threading (like gdal3.js)
    );

    CPLSetConfigOption("GDAL_DATA", "/usr/share/gdal");

    // Add /usr/share/gdal to GDAL's file finder search path
    CPLPushFinderLocation("/usr/share/gdal");

    // Set PROJ search paths for GDAL's OGRSpatialReference (spatial reference system)
    const char* proj_paths[] = {"/usr/share/proj", nullptr};
    OSRSetPROJSearchPaths(proj_paths);

    // Register GDAL drivers
    GDALAllRegister();

    gdal_proj_configured = true;
}

// JavaScript wrapper for camproject
void camproject_JS(uintptr_t model_ptr,
                   const std::string& input_image_path,
                   const std::string& output_proj_string,
                   const std::string& output_path,
                   double ground_height) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }

    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);

    // Create ShapeModel from ground height
    auto [semi_major, semi_minor] = csm::getRadii(model);
    Ellipsoid3 ellipsoid(semi_major, semi_major, semi_minor);
    miniset::ShapeModel shape_model = miniset::createConstantHeightShapeModel(ground_height, ellipsoid);

    miniset::camproject(input_image_path, model, shape_model, output_proj_string, output_path);
}

// JavaScript wrapper for generateBoundary
val generateBoundary_JS(uintptr_t model_ptr,
                        int image_width,
                        int image_height,
                        double ground_height,
                        int num_edge_samples) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }

    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);

    // Create ShapeModel from ground height
    auto [semi_major, semi_minor] = csm::getRadii(model);
    Ellipsoid3 ellipsoid(semi_major, semi_major, semi_minor);
    miniset::ShapeModel shape_model = miniset::createConstantHeightShapeModel(ground_height, ellipsoid);

    // Generate boundary
    miniset::Boundary boundary = miniset::generateBoundary(
        model, image_width, image_height, shape_model, num_edge_samples);

    // Convert to JavaScript object
    val result = val::object();
    result.set("lat_deg", val::array(boundary.lat_deg.begin(), boundary.lat_deg.end()));
    result.set("lon_deg", val::array(boundary.lon_deg.begin(), boundary.lon_deg.end()));
    result.set("height_m", val::array(boundary.height_m.begin(), boundary.height_m.end()));

    return result;
}

// JavaScript wrapper for generateFootprintWKT
std::string generateFootprintWKT_JS(uintptr_t model_ptr,
                                     int image_width,
                                     int image_height,
                                     double ground_height,
                                     int num_edge_samples) {
    if (!model_ptr) {
        throw std::runtime_error("Invalid model pointer");
    }

    ::csm::RasterGM* model = reinterpret_cast<::csm::RasterGM*>(model_ptr);

    // Create ShapeModel from ground height
    auto [semi_major, semi_minor] = csm::getRadii(model);
    Ellipsoid3 ellipsoid(semi_major, semi_major, semi_minor);
    miniset::ShapeModel shape_model = miniset::createConstantHeightShapeModel(ground_height, ellipsoid);

    // Generate WKT footprint
    return miniset::generateFootprintWKT(
        model, image_width, image_height, shape_model, num_edge_samples);
}

// Utility functions for degree/radian conversion
double degreesToRadians(double degrees) {
    return degrees * M_PI / 180.0;
}

double radiansToDegrees(double radians) {
    return radians * 180.0 / M_PI;
}

EMSCRIPTEN_BINDINGS(miniset) {
    // Call initialization on module load
    miniset_module_init();

    // Utility functions
    function("degreesToRadians", &degreesToRadians);
    function("radiansToDegrees", &radiansToDegrees);

    function("createCsmFromISD", &createCsmFromISD_JS);
    function("createCsmFromStateString", &createCsmFromStateString_JS);
    function("createCsmFromAttachedSpice", &createCsmFromAttachedSpice_JS);
    function("getModelState", &getModelState_JS);
    function("campt", &campt_JS);
    function("getRadii", &getRadii_JS);
    function("imageToGround", &imageToGround_JS);
    function("groundToImage", &groundToImage_JS);
    function("getImageSize", &getImageSize_JS);
    function("getModelName", &getModelName_JS);
    function("getImageIdentifier", &getImageIdentifier_JS);
    function("getSensorState", &getSensorState_JS);
    function("deleteModel", &deleteModel_JS);

    function("camproject", &camproject_JS);
    function("generateBoundary", &generateBoundary_JS);
    function("generateFootprintWKT", &generateFootprintWKT_JS);

    // DEM type enum
    enum_<surface::DEMType>("DEMType")
        .value("HEIGHT", surface::DEMType::HEIGHT)
        .value("RADIUS", surface::DEMType::RADIUS);

    // EllipsoidDEM base class
    class_<surface::EllipsoidDEM>("EllipsoidDEM")
        .constructor<double, double>()
        .function("getHeight", &surface::EllipsoidDEM::getHeight)
        .function("getRadius", &surface::EllipsoidDEM::getRadius)
        .function("getSemiMajorA", &surface::EllipsoidDEM::getSemiMajorA)
        .function("getSemiMajorB", &surface::EllipsoidDEM::getSemiMajorB)
        .function("getSemiMinorC", &surface::EllipsoidDEM::getSemiMinorC);

    // GdalDEM class (inherits from EllipsoidDEM)
    class_<surface::GdalDEM, base<surface::EllipsoidDEM>>("GdalDEM")
        .constructor<const std::string&, surface::DEMType>()
        .constructor<const std::string&, surface::DEMType, double, double>()
        .function("setEllipsoid", &surface::GdalDEM::setEllipsoid)
        .function("getHeight", &surface::GdalDEM::getHeight)
        .function("getRadius", &surface::GdalDEM::getRadius)
        .function("getRasterValue", &surface::GdalDEM::getRasterValue)
        .function("getSemiMajorA", &surface::GdalDEM::getSemiMajorA)
        .function("getSemiMajorB", &surface::GdalDEM::getSemiMajorB)
        .function("getSemiMinorC", &surface::GdalDEM::getSemiMinorC);
}

