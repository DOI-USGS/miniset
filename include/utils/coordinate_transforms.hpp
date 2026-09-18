#ifndef MINISET_UTILS_COORDINATE_TRANSFORMS_HPP
#define MINISET_UTILS_COORDINATE_TRANSFORMS_HPP

#include "core/types.hpp"
#include <vector>

namespace utils {

// ECEF to Lat/Lon/Alt
LatLon ecefToLatLon(const Vec3& ecef_pt, double semi_major, double semi_minor);

// Lat/Lon/Alt to ECEF
Vec3 latLonToEcef(double lat_rad, double lon_rad, double height,
                  double semi_major, double semi_minor);

// Batch conversions
std::vector<LatLon> batchEcefToLatLon(const std::vector<Vec3>& points,
                                      double semi_major, double semi_minor);

} // namespace utils

#endif
