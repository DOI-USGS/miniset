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
