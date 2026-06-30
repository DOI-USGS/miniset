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


#ifndef MINISET_CORE_MATH_HPP
#define MINISET_CORE_MATH_HPP

#include "types.hpp"
#include <cmath>

namespace math {

// Vector magnitude (Euclidean norm)
double magnitude(const Vec3& vec);

// Distance between two points
double distance(const Vec3& start, const Vec3& stop);

// Separation angle between two vectors (in radians)
double separationAngle(const Vec3& a_vec, const Vec3& b_vec);

// Normalize vector to unit length
Vec3 unitVector(const Vec3& vec);

// Vector cross product
Vec3 crossProduct(const Vec3& a_vec, const Vec3& b_vec);

// Vector dot product
double dotProduct(const Vec3& a_vec, const Vec3& b_vec);

// Scale vector by scalar
Vec3 scaleVector(const Vec3& vec, double scalar);

// Component of b_vec perpendicular to a_vec
Vec3 perpendicularVector(const Vec3& a_vec, const Vec3& b_vec);

// Coordinate transformations
LatLon radiansToDegrees(const LatLon& radian_lat_lon);
Vec3 sphericalToRect(const Sphere& spherical);
Sphere rectToSpherical(const Vec3& rectangular);

// Ground azimuth computation
double groundAzimuth(const LatLon& ground_pt, const LatLon& sub_pt);

} // namespace math

#endif // MINISET_CORE_MATH_HPP
