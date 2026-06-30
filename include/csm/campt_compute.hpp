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


#ifndef MINISET_CSM_CAMPT_COMPUTE_HPP
#define MINISET_CSM_CAMPT_COMPUTE_HPP

#include "core/types.hpp"

#ifdef MINISET_HAS_CSMAPI
#include <csm/RasterGM.h>
#endif

namespace csm {

#ifdef MINISET_HAS_CSMAPI

// Compute photometric angles at a ground point
struct PhotometricAngles {
    double phase_angle;      // degrees
    double emission_angle;   // degrees
    double incidence_angle;  // degrees
};

PhotometricAngles computePhotometricAngles(
    const ::csm::RasterGM* sensor,
    const ::csm::EcefCoord& ground_point,
    const Vec3& surface_normal,
    const Vec3& sensor_position);

// Compute resolution at an image coordinate
struct Resolution {
    double sample_resolution;  // meters/pixel
    double line_resolution;    // meters/pixel
    double pixel_resolution;   // meters/pixel (average)
    double oblique_resolution; // meters (oblique to surface)
};

Resolution computeResolution(
    const ::csm::RasterGM* sensor,
    double line, double sample,
    double height,
    double emission_angle_deg);

// Compute spacecraft geometry
struct SpacecraftGeometry {
    Vec3 sub_spacecraft_point;
    double sub_spacecraft_lat;  // radians
    double sub_spacecraft_lon;  // radians
    double spacecraft_altitude; // meters
    double off_nadir_angle;     // degrees
    double spacecraft_azimuth;  // degrees
    double sub_spacecraft_ground_azimuth; // degrees
};

SpacecraftGeometry computeSpacecraftGeometry(
    const Vec3& sensor_position,
    const Vec3& ground_point,
    const Vec3& look_direction,
    const Vec3& surface_normal,
    const Ellipsoid3& ellipsoid);

// Compute sun/illumination geometry
struct SunGeometry {
    Vec3 sun_position;         // ECEF, km
    Vec3 sub_solar_point;      // ECEF, meters
    double sub_solar_lat;      // radians
    double sub_solar_lon;      // radians
    double solar_distance;     // AU
    double sub_solar_azimuth;  // degrees
    double sub_solar_ground_azimuth; // degrees
    bool valid;                // true if illumination data available
};

SunGeometry computeSunGeometry(
    const ::csm::RasterGM* sensor,
    const ::csm::EcefCoord& ground_point,
    const Vec3& surface_normal,
    const Ellipsoid3& ellipsoid);

// Compute azimuths (north, spacecraft, sun)
struct Azimuths {
    double north_azimuth;     // degrees
    double spacecraft_azimuth; // degrees
    double sun_azimuth;        // degrees
};

Azimuths computeAzimuths(
    const Vec3& ground_point,
    const Vec3& to_spacecraft,
    const Vec3& to_sun,
    const Vec3& surface_normal);

#endif // MINISET_HAS_CSMAPI

} // namespace csm

#endif // MINISET_CSM_CAMPT_COMPUTE_HPP
