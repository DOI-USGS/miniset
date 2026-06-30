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
