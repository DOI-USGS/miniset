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


#include "utils/coordinate_transforms.hpp"
#include "core/math.hpp"
#include <cmath>

namespace utils {

LatLon ecefToLatLon(const Vec3& ecef_pt, double semi_major, double semi_minor) {
    double lon = std::atan2(ecef_pt.y, ecef_pt.x);
    double p = std::sqrt(ecef_pt.x * ecef_pt.x + ecef_pt.y * ecef_pt.y);
    double lat = std::atan2(ecef_pt.z, p);

    // Iterative refinement for geodetic latitude
    double e2 = 1.0 - (semi_minor * semi_minor) / (semi_major * semi_major);
    for (int i = 0; i < 5; ++i) {
        double sin_lat = std::sin(lat);
        double N = semi_major / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
        lat = std::atan2(ecef_pt.z + e2 * N * sin_lat, p);
    }

    return LatLon(lat, lon);
}

Vec3 latLonToEcef(double lat_rad, double lon_rad, double height,
                  double semi_major, double semi_minor) {
    double e2 = 1.0 - (semi_minor * semi_minor) / (semi_major * semi_major);
    double sin_lat = std::sin(lat_rad);
    double cos_lat = std::cos(lat_rad);
    double N = semi_major / std::sqrt(1.0 - e2 * sin_lat * sin_lat);

    double x = (N + height) * cos_lat * std::cos(lon_rad);
    double y = (N + height) * cos_lat * std::sin(lon_rad);
    double z = (N * (1.0 - e2) + height) * sin_lat;

    return Vec3(x, y, z);
}

std::vector<LatLon> batchEcefToLatLon(const std::vector<Vec3>& points,
                                      double semi_major, double semi_minor) {
    std::vector<LatLon> results;
    results.reserve(points.size());

    for (const auto& pt : points) {
        results.push_back(ecefToLatLon(pt, semi_major, semi_minor));
    }

    return results;
}

} // namespace utils
