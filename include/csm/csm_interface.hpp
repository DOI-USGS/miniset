/*
 * Copyright (C) 2024 USGS Astrogeology Science Center
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


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
