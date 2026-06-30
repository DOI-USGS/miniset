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


#ifndef MINISET_SENSOR_ANGLES_HPP
#define MINISET_SENSOR_ANGLES_HPP

#include "core/types.hpp"
#include "surface/ellipsoid.hpp"

namespace sensor {

// Note: PhotometricAngles struct is defined in csm/campt_compute.hpp
// Use that for angle calculations

// Compute individual angles (in radians)
double computePhaseAngle(const Vec3& ground_pt, const Vec3& sensor_pos, const Vec3& sun_pos);
double computeEmissionAngle(const Vec3& surface_normal, const Vec3& look_vec);
double computeIncidenceAngle(const Vec3& ground_pt, const Vec3& sun_pos, const Vec3& surface_normal);

// Geometric utilities
double computeSlantDistance(const Vec3& sensor_pos, const Vec3& ground_pt);
double computeTargetCenterDistance(const Vec3& sensor_pos);
double computeLocalRadius(const Vec3& ground_pt);

} // namespace sensor

#endif
