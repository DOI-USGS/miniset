#ifndef MINISET_CSM_CSM_INTERFACE_HPP
#define MINISET_CSM_CSM_INTERFACE_HPP

#include "core/types.hpp"
#include <string>
#include <csm/RasterGM.h>

namespace csm {

// CSM sensor state
struct SensorState {
    Vec3 sensor_pos;
    Vec3 look_vec;
    double sensor_time;
};

// Get radii from CSM sensor
std::pair<double, double> getRadii(const ::csm::RasterGM* camera);

// Get sensor state at image point
SensorState getSensorState(const ::csm::RasterGM* sensor, double line, double sample);

// Create CSM sensor from ISD file or JSON string
// If input looks like JSON (starts with '{'), treats as JSON
// Otherwise treats as file path
::csm::RasterGM* createCsmFromISD(const std::string& isd_file_or_json);

// Create CSM sensor from in-memory ISD JSON string
// Explicitly handles JSON string (not a file path)
::csm::RasterGM* loadFromIsd(const std::string& isd_json);

// Create CSM sensor from state string
::csm::RasterGM* createCsmFromStateString(const std::string& state_string);

// Create CSM sensor from attached SPICE data (ISIS cube processed with csminit)
// Reads hex-encoded CSM state from json:ISIS3 domain
::csm::RasterGM* createCsmFromAttachedSpice(const std::string& image_path);

} // namespace csm

#endif
