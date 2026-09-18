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
