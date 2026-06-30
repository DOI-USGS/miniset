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


#ifndef MINISET_SURFACE_INTERSECTION_HPP
#define MINISET_SURFACE_INTERSECTION_HPP

#include "core/types.hpp"
#include "surface/ellipsoid.hpp"

namespace surface {

// Helper functions for surface intersection

// Compute Chebyshev distance (max of abs differences) between two 3D points
double computeIntersectionDistance(const Vec3& pt1, const Vec3& pt2);

// Iterative DEM intersection with refinement
Vec3 intersectWithDEM(const Vec3& sensor_pos, const Vec3& look_vec,
                      const class EllipsoidDEM& dem,
                      double tolerance = 0.0001, int max_iterations = 20);

} // namespace surface

#endif // MINISET_SURFACE_INTERSECTION_HPP
