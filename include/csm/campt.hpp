#ifndef MINISET_CSM_CAMPT_HPP
#define MINISET_CSM_CAMPT_HPP

#include "core/types.hpp"
#include <string>

#ifdef MINISET_HAS_CSMAPI
#include <csm/RasterGM.h>
#endif

namespace csm {

// Camera point information at a single image coordinate
struct CamptInfo {
    // Input
    double sample;
    double line;

    // Ground point (ECEF)
    Vec3 ground_point;

    // Lat/Lon/Height
    double planetocentric_lat;     // radians
    double planetographic_lat;     // radians
    double positive_east_lon;      // radians (0 to 2π)
    double height;                 // meters above ellipsoid

    // Sensor geometry
    Vec3 sensor_position;
    Vec3 look_direction;
    double sensor_time;

    // Photometric angles (degrees)
    double phase_angle;
    double emission_angle;
    double incidence_angle;
    double north_azimuth;

    // Resolution
    double sample_resolution;      // meters/pixel
    double line_resolution;        // meters/pixel
    double pixel_resolution;       // meters/pixel (average)
    double oblique_resolution;     // meters (oblique to surface)

    // Distances
    double slant_distance;         // meters (sensor to ground)
    double target_center_distance; // meters (sensor to body center)
    double spacecraft_altitude;    // meters (sensor altitude above surface)
    double local_radius;           // meters (body center to ground)

    // Sub-spacecraft point
    double sub_spacecraft_lat;     // radians (planetocentric)
    double sub_spacecraft_lon;     // radians
    double off_nadir_angle;        // degrees
    double spacecraft_azimuth;     // degrees
    double sub_spacecraft_ground_azimuth; // degrees

    // Sun information (if available)
    Vec3 sun_position;             // ECEF, meters
    double sub_solar_lat;          // radians
    double sub_solar_lon;          // radians
    double solar_distance;         // AU
    double sub_solar_azimuth;      // degrees
    double sub_solar_ground_azimuth; // degrees
    double solar_longitude;        // radians (Ls)
    double local_solar_time;       // hours

    // Celestial coordinates
    double right_ascension;        // radians (J2000)
    double declination;            // radians (J2000)

    // Look direction in different frames
    Vec3 look_j2000;               // J2000 frame
    Vec3 look_camera;              // Camera frame
};

#ifdef MINISET_HAS_CSMAPI
// Compute camera point information for a single image coordinate
// Uses CSM sensor model to compute all geometric and photometric properties
// ellipsoid: target body ellipsoid for lat/lon conversion
// height: height above ellipsoid for intersection (default 0.0)
CamptInfo campt(const ::csm::RasterGM* sensor,
                double sample, double line,
                const Ellipsoid3& ellipsoid,
                double height = 0.0);

// Batch version for multiple points
std::vector<CamptInfo> camptBatch(const ::csm::RasterGM* sensor,
                                  const std::vector<::ImageCoord>& coords,
                                  const Ellipsoid3& ellipsoid,
                                  double height = 0.0);
#endif

} // namespace csm

#endif // MINISET_CSM_CAMPT_HPP
