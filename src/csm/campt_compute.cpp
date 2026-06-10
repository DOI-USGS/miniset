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


#include "csm/campt_compute.hpp"

#ifdef MINISET_HAS_CSMAPI
#include "surface/ellipsoid.hpp"
#include "utils/coordinate_transforms.hpp"
#include "core/math.hpp"
#include <csm/csm.h>
#include <cmath>

namespace csm {

PhotometricAngles computePhotometricAngles(
    const ::csm::RasterGM* sensor,
    const ::csm::EcefCoord& ground_point,
    const Vec3& surface_normal,
    const Vec3& sensor_position) {

    PhotometricAngles angles;

    Vec3 ground_pt{ground_point.x, ground_point.y, ground_point.z};

    // Vector from ground to sensor
    Vec3 to_sensor = {
        sensor_position.x - ground_pt.x,
        sensor_position.y - ground_pt.y,
        sensor_position.z - ground_pt.z
    };

    // Emission angle (observer angle)
    double cos_emission = math::dotProduct(surface_normal, math::unitVector(to_sensor));
    angles.emission_angle = std::acos(std::clamp(cos_emission, -1.0, 1.0)) * 180.0 / M_PI;

    // Try to get illumination direction for incidence angle
    try {
        ::csm::EcefVector illum_dir = sensor->getIlluminationDirection(ground_point);
        double illum_mag = std::sqrt(illum_dir.x * illum_dir.x + illum_dir.y * illum_dir.y + illum_dir.z * illum_dir.z);
        Vec3 sun_dir = {illum_dir.x / illum_mag, illum_dir.y / illum_mag, illum_dir.z / illum_mag};

        double cos_incidence = math::dotProduct(surface_normal, sun_dir);
        angles.incidence_angle = std::acos(std::clamp(cos_incidence, -1.0, 1.0)) * 180.0 / M_PI;

        // Phase angle (angle between sun and sensor as seen from ground)
        double cos_phase = math::dotProduct(math::unitVector(to_sensor), sun_dir);
        angles.phase_angle = std::acos(std::clamp(cos_phase, -1.0, 1.0)) * 180.0 / M_PI;
    } catch (...) {
        // Fallback: assume zenith illumination
        double cos_incidence = surface_normal.z / math::magnitude(surface_normal);
        angles.incidence_angle = std::acos(std::clamp(cos_incidence, -1.0, 1.0)) * 180.0 / M_PI;
        angles.phase_angle = std::abs(angles.emission_angle - angles.incidence_angle);
    }

    return angles;
}

Resolution computeResolution(
    const ::csm::RasterGM* sensor,
    double line, double sample,
    double height,
    double emission_angle_deg) {

    Resolution res;

    // Use finite difference: move 1 pixel in each direction
    ::csm::ImageCoord img_coord(line, sample);
    ::csm::ImageCoord img_plus_sample(line, sample + 1.0);
    ::csm::ImageCoord img_plus_line(line + 1.0, sample);

    ::csm::EcefCoord ground = sensor->imageToGround(img_coord, height);
    ::csm::EcefCoord ground_plus_sample = sensor->imageToGround(img_plus_sample, height);
    ::csm::EcefCoord ground_plus_line = sensor->imageToGround(img_plus_line, height);

    Vec3 gp{ground.x, ground.y, ground.z};
    Vec3 gp_sample{ground_plus_sample.x, ground_plus_sample.y, ground_plus_sample.z};
    Vec3 gp_line{ground_plus_line.x, ground_plus_line.y, ground_plus_line.z};

    res.sample_resolution = math::distance(gp, gp_sample);
    res.line_resolution = math::distance(gp, gp_line);
    res.pixel_resolution = (res.sample_resolution + res.line_resolution) / 2.0;
    res.oblique_resolution = res.pixel_resolution / std::cos(emission_angle_deg * M_PI / 180.0);

    return res;
}

SpacecraftGeometry computeSpacecraftGeometry(
    const Vec3& sensor_position,
    const Vec3& ground_point,
    const Vec3& look_direction,
    const Vec3& surface_normal,
    const Ellipsoid3& ellipsoid) {

    SpacecraftGeometry geom;

    // Sub-spacecraft point (nadir)
    Vec3 nadir = ellipsoid::intersectSurface(ellipsoid, sensor_position,
                                              math::scaleVector(sensor_position, -1.0));
    LatLon sub_sc = utils::ecefToLatLon(nadir, ellipsoid.a, ellipsoid.c);
    geom.sub_spacecraft_point = nadir;
    geom.sub_spacecraft_lat = sub_sc.lat;
    geom.sub_spacecraft_lon = sub_sc.lon;
    if (geom.sub_spacecraft_lon < 0) geom.sub_spacecraft_lon += 2.0 * M_PI;

    // Spacecraft altitude
    geom.spacecraft_altitude = math::distance(sensor_position, nadir);

    // Off-nadir angle
    Vec3 nadir_vec = math::unitVector(math::scaleVector(sensor_position, -1.0));
    double cos_off_nadir = math::dotProduct(look_direction, nadir_vec);
    geom.off_nadir_angle = std::acos(std::clamp(cos_off_nadir, -1.0, 1.0)) * 180.0 / M_PI;

    // Compute local coordinate system for azimuth
    Vec3 north_pole = {0, 0, 1};
    Vec3 east_dir = math::crossProduct(north_pole, surface_normal);
    east_dir = math::unitVector(east_dir);
    Vec3 north_dir = math::crossProduct(surface_normal, east_dir);
    north_dir = math::unitVector(north_dir);

    Vec3 to_sensor = {
        sensor_position.x - ground_point.x,
        sensor_position.y - ground_point.y,
        sensor_position.z - ground_point.z
    };
    Vec3 to_sensor_horiz = {
        to_sensor.x - math::dotProduct(to_sensor, surface_normal) * surface_normal.x,
        to_sensor.y - math::dotProduct(to_sensor, surface_normal) * surface_normal.y,
        to_sensor.z - math::dotProduct(to_sensor, surface_normal) * surface_normal.z
    };
    to_sensor_horiz = math::unitVector(to_sensor_horiz);

    double cos_azim = math::dotProduct(to_sensor_horiz, north_dir);
    double sin_azim = math::dotProduct(to_sensor_horiz, east_dir);
    geom.spacecraft_azimuth = std::atan2(sin_azim, cos_azim) * 180.0 / M_PI;
    if (geom.spacecraft_azimuth < 0) geom.spacecraft_azimuth += 360.0;
    geom.sub_spacecraft_ground_azimuth = geom.spacecraft_azimuth;

    return geom;
}

SunGeometry computeSunGeometry(
    const ::csm::RasterGM* sensor,
    const ::csm::EcefCoord& ground_point,
    const Vec3& surface_normal,
    const Ellipsoid3& ellipsoid) {

    SunGeometry sun;
    sun.valid = false;

    try {
        ::csm::EcefVector illum_dir = sensor->getIlluminationDirection(ground_point);

        // Normalize illumination direction
        double illum_mag = std::sqrt(illum_dir.x * illum_dir.x + illum_dir.y * illum_dir.y + illum_dir.z * illum_dir.z);
        Vec3 sun_dir = {illum_dir.x / illum_mag, illum_dir.y / illum_mag, illum_dir.z / illum_mag};

        // Estimate solar distance based on body size
        double AU = 149597870.7;  // km
        double approx_solar_dist_au = (ellipsoid.a > 2000000) ? 1.52 : 1.0;  // Mars vs Moon
        sun.solar_distance = approx_solar_dist_au;

        // Sun position in km
        sun.sun_position = {
            sun_dir.x * approx_solar_dist_au * AU,
            sun_dir.y * approx_solar_dist_au * AU,
            sun_dir.z * approx_solar_dist_au * AU
        };

        // Sub-solar point
        Vec3 ground_pt{ground_point.x, ground_point.y, ground_point.z};
        sun.sub_solar_point = ellipsoid::intersectSurface(ellipsoid, ground_pt, sun_dir);
        LatLon subsolar_ll = utils::ecefToLatLon(sun.sub_solar_point, ellipsoid.a, ellipsoid.c);
        sun.sub_solar_lat = subsolar_ll.lat;
        sun.sub_solar_lon = subsolar_ll.lon;
        if (sun.sub_solar_lon < 0) sun.sub_solar_lon += 2.0 * M_PI;

        // Compute sub-solar azimuth
        Vec3 north_pole = {0, 0, 1};
        Vec3 east_dir = math::crossProduct(north_pole, surface_normal);
        east_dir = math::unitVector(east_dir);
        Vec3 north_dir = math::crossProduct(surface_normal, east_dir);
        north_dir = math::unitVector(north_dir);

        Vec3 to_subsolar = {
            sun.sub_solar_point.x - ground_pt.x,
            sun.sub_solar_point.y - ground_pt.y,
            sun.sub_solar_point.z - ground_pt.z
        };
        Vec3 to_subsolar_horiz = {
            to_subsolar.x - math::dotProduct(to_subsolar, surface_normal) * surface_normal.x,
            to_subsolar.y - math::dotProduct(to_subsolar, surface_normal) * surface_normal.y,
            to_subsolar.z - math::dotProduct(to_subsolar, surface_normal) * surface_normal.z
        };
        to_subsolar_horiz = math::unitVector(to_subsolar_horiz);

        double cos_solar = math::dotProduct(to_subsolar_horiz, north_dir);
        double sin_solar = math::dotProduct(to_subsolar_horiz, east_dir);
        sun.sub_solar_azimuth = std::atan2(sin_solar, cos_solar) * 180.0 / M_PI;
        if (sun.sub_solar_azimuth < 0) sun.sub_solar_azimuth += 360.0;
        sun.sub_solar_ground_azimuth = sun.sub_solar_azimuth;

        sun.valid = true;
    } catch (...) {
        sun.sun_position = {0, 0, 0};
        sun.sub_solar_point = {0, 0, 0};
        sun.sub_solar_lat = 0;
        sun.sub_solar_lon = 0;
        sun.solar_distance = 0;
        sun.sub_solar_azimuth = 0;
        sun.sub_solar_ground_azimuth = 0;
    }

    return sun;
}

Azimuths computeAzimuths(
    const Vec3& ground_point,
    const Vec3& to_spacecraft,
    const Vec3& to_sun,
    const Vec3& surface_normal) {

    Azimuths azim;

    // Local coordinate system
    Vec3 north_pole = {0, 0, 1};
    Vec3 east_dir = math::crossProduct(north_pole, surface_normal);
    east_dir = math::unitVector(east_dir);
    Vec3 north_dir = math::crossProduct(surface_normal, east_dir);
    north_dir = math::unitVector(north_dir);

    // Project spacecraft direction to horizontal plane
    Vec3 sc_horiz = {
        to_spacecraft.x - math::dotProduct(to_spacecraft, surface_normal) * surface_normal.x,
        to_spacecraft.y - math::dotProduct(to_spacecraft, surface_normal) * surface_normal.y,
        to_spacecraft.z - math::dotProduct(to_spacecraft, surface_normal) * surface_normal.z
    };
    sc_horiz = math::unitVector(sc_horiz);

    double cos_sc = math::dotProduct(sc_horiz, north_dir);
    double sin_sc = math::dotProduct(sc_horiz, east_dir);
    azim.north_azimuth = std::atan2(sin_sc, cos_sc) * 180.0 / M_PI;
    if (azim.north_azimuth < 0) azim.north_azimuth += 360.0;
    azim.spacecraft_azimuth = azim.north_azimuth;

    // Project sun direction to horizontal plane
    Vec3 sun_horiz = {
        to_sun.x - math::dotProduct(to_sun, surface_normal) * surface_normal.x,
        to_sun.y - math::dotProduct(to_sun, surface_normal) * surface_normal.y,
        to_sun.z - math::dotProduct(to_sun, surface_normal) * surface_normal.z
    };
    sun_horiz = math::unitVector(sun_horiz);

    double cos_sun = math::dotProduct(sun_horiz, north_dir);
    double sin_sun = math::dotProduct(sun_horiz, east_dir);
    azim.sun_azimuth = std::atan2(sin_sun, cos_sun) * 180.0 / M_PI;
    if (azim.sun_azimuth < 0) azim.sun_azimuth += 360.0;

    return azim;
}

} // namespace csm

#endif // MINISET_HAS_CSMAPI
