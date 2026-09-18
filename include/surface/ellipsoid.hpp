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
