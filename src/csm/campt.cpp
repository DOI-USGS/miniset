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


#include "csm/campt.hpp"
#include "csm/campt_compute.hpp"

#ifdef MINISET_HAS_CSMAPI
#include "surface/ellipsoid.hpp"
#include "utils/coordinate_transforms.hpp"
#include "core/math.hpp"
#include <csm/csm.h>
#include <cmath>

namespace csm {

CamptInfo campt(const ::csm::RasterGM* sensor,
                double sample, double line,
                const Ellipsoid3& ellipsoid,
                double height) {
    CamptInfo info;

    // Store input
    info.sample = sample;
    info.line = line;

    // Get sensor state
    ::csm::ImageCoord img_coord(line, sample);
    info.sensor_time = sensor->getImageTime(img_coord);

    // Get sensor position
    ::csm::EcefCoord sensor_pos_csm = sensor->getSensorPosition(img_coord);
    info.sensor_position = Vec3(sensor_pos_csm.x, sensor_pos_csm.y, sensor_pos_csm.z);

    // Get look direction
    ::csm::EcefLocus locus = sensor->imageToRemoteImagingLocus(img_coord);
    info.look_direction = Vec3(locus.direction.x, locus.direction.y, locus.direction.z);

    // Compute ground intersection
    ::csm::EcefCoord ground_csm = sensor->imageToGround(img_coord, height);
    info.ground_point = Vec3(ground_csm.x, ground_csm.y, ground_csm.z);

    // Convert to lat/lon (planetocentric and planetographic)
    LatLon latlon = utils::ecefToLatLon(info.ground_point, ellipsoid.a, ellipsoid.c);
    info.planetocentric_lat = latlon.lat;
    info.positive_east_lon = latlon.lon;
    if (info.positive_east_lon < 0) info.positive_east_lon += 2.0 * M_PI;

    // Planetographic latitude
    double f = (ellipsoid.a - ellipsoid.c) / ellipsoid.a;  // flattening
    info.planetographic_lat = std::atan(std::tan(info.planetocentric_lat) / ((1.0 - f) * (1.0 - f)));
    info.height = height;

    // Surface normal
    Vec3 normal = ellipsoid::getSurfaceNormal(ellipsoid, info.ground_point);

    // Compute photometric angles
    PhotometricAngles phot = computePhotometricAngles(sensor, ground_csm, normal, info.sensor_position);
    info.phase_angle = phot.phase_angle;
    info.emission_angle = phot.emission_angle;
    info.incidence_angle = phot.incidence_angle;

    // Compute resolution
    Resolution res = computeResolution(sensor, line, sample, height, info.emission_angle);
    info.sample_resolution = res.sample_resolution;
    info.line_resolution = res.line_resolution;
    info.pixel_resolution = res.pixel_resolution;
    info.oblique_resolution = res.oblique_resolution;

    // Compute distances
    info.slant_distance = math::distance(info.sensor_position, info.ground_point);
    info.target_center_distance = math::magnitude(info.sensor_position);
    info.local_radius = math::magnitude(info.ground_point);

    // Compute spacecraft geometry
    SpacecraftGeometry sc_geom = computeSpacecraftGeometry(
        info.sensor_position, info.ground_point, info.look_direction, normal, ellipsoid);
    info.sub_spacecraft_lat = sc_geom.sub_spacecraft_lat;
    info.sub_spacecraft_lon = sc_geom.sub_spacecraft_lon;
    info.spacecraft_altitude = sc_geom.spacecraft_altitude;
    info.off_nadir_angle = sc_geom.off_nadir_angle;
    info.spacecraft_azimuth = sc_geom.spacecraft_azimuth;
    info.sub_spacecraft_ground_azimuth = sc_geom.sub_spacecraft_ground_azimuth;
    info.north_azimuth = sc_geom.spacecraft_azimuth;

    // Compute sun geometry
    SunGeometry sun_geom = computeSunGeometry(sensor, ground_csm, normal, ellipsoid);
    info.sun_position = sun_geom.sun_position;
    info.sub_solar_lat = sun_geom.sub_solar_lat;
    info.sub_solar_lon = sun_geom.sub_solar_lon;
    info.solar_distance = sun_geom.solar_distance;
    info.sub_solar_azimuth = sun_geom.sub_solar_azimuth;
    info.sub_solar_ground_azimuth = sun_geom.sub_solar_ground_azimuth;
    info.solar_longitude = 0;  // Requires orbital mechanics
    info.local_solar_time = 0;  // Requires orbital mechanics

    // Celestial coordinates (RA/Dec) - would need J2000 transformation
    info.right_ascension = 0;
    info.declination = 0;

    // Look direction in different frames
    info.look_j2000 = {0, 0, 0};  // Would need frame transformation
    info.look_camera = {0, 0, 0}; // Would need camera model info

    return info;
}

std::vector<CamptInfo> camptBatch(const ::csm::RasterGM* sensor,
                                  const std::vector<::ImageCoord>& coords,
                                  const Ellipsoid3& ellipsoid,
                                  double height) {
    std::vector<CamptInfo> results;
    results.reserve(coords.size());

    // Note: miniset::ImageCoord uses 'sample', ::csm::ImageCoord uses 'samp'
    for (const auto& coord : coords) {
        results.push_back(campt(sensor, coord.sample, coord.line, ellipsoid, height));
    }

    return results;
}

} // namespace csm

#endif // MINISET_HAS_CSMAPI
