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
