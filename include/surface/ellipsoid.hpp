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


#ifndef MINISET_SURFACE_ELLIPSOID_HPP
#define MINISET_SURFACE_ELLIPSOID_HPP

#include "core/types.hpp"
#include <vector>

namespace ellipsoid {

// Factory function from CSM sensor
#ifdef MINISET_HAS_CSMAPI
Ellipsoid3 fromCsmSensor(const void* sensor);
#endif

// Get surface normal at a point
Vec3 getSurfaceNormal(const Ellipsoid3& e, const Vec3& ground_pt);

// Ray-ellipsoid intersection
Vec3 intersectSurface(const Ellipsoid3& e, const Vec3& sensor_pos, const Vec3& look_vec);

// Get radius at geodetic point
double getRadiusAt(const Ellipsoid3& e, double lat_rad, double lon_rad);

// Batch operations
std::vector<Vec3> getSurfaceNormalBatch(const Ellipsoid3& e, const std::vector<Vec3>& points);
std::vector<Vec3> intersectSurfaceBatch(const Ellipsoid3& e,
                                         const std::vector<Vec3>& sensor_positions,
                                         const std::vector<Vec3>& look_vectors);

} // namespace ellipsoid

#endif // MINISET_SURFACE_ELLIPSOID_HPP
