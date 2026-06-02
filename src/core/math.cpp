#include "core/math.hpp"
#include <cmath>
#include <algorithm>

namespace math {

double magnitude(const Vec3& vec) {
    return std::sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
}

double distance(const Vec3& start, const Vec3& stop) {
    double dx = stop.x - start.x;
    double dy = stop.y - start.y;
    double dz = stop.z - start.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double separationAngle(const Vec3& a_vec, const Vec3& b_vec) {
    double mag_a = magnitude(a_vec);
    double mag_b = magnitude(b_vec);

    if (mag_a == 0.0 || mag_b == 0.0) {
        return 0.0;
    }

    double dot = dotProduct(a_vec, b_vec);
    double cos_angle = dot / (mag_a * mag_b);

    // Clamp to valid range to avoid numerical errors
    cos_angle = std::max(-1.0, std::min(1.0, cos_angle));

    return std::acos(cos_angle);
}

Vec3 unitVector(const Vec3& vec) {
    double mag = magnitude(vec);
    if (mag == 0.0) {
        return Vec3(0.0, 0.0, 0.0);
    }
    return Vec3(vec.x / mag, vec.y / mag, vec.z / mag);
}

Vec3 crossProduct(const Vec3& a_vec, const Vec3& b_vec) {
    return Vec3(
        a_vec.y * b_vec.z - a_vec.z * b_vec.y,
        a_vec.z * b_vec.x - a_vec.x * b_vec.z,
        a_vec.x * b_vec.y - a_vec.y * b_vec.x
    );
}

double dotProduct(const Vec3& a_vec, const Vec3& b_vec) {
    return a_vec.x * b_vec.x + a_vec.y * b_vec.y + a_vec.z * b_vec.z;
}

Vec3 scaleVector(const Vec3& vec, double scalar) {
    return Vec3(vec.x * scalar, vec.y * scalar, vec.z * scalar);
}

Vec3 perpendicularVector(const Vec3& a_vec, const Vec3& b_vec) {
    // q = b - proj_a(b) = b - (a·b/|a|²)*a
    double mag_a_sq = a_vec.x * a_vec.x + a_vec.y * a_vec.y + a_vec.z * a_vec.z;

    if (mag_a_sq == 0.0) {
        return b_vec;
    }

    double dot = dotProduct(a_vec, b_vec);
    double scale = dot / mag_a_sq;

    return Vec3(
        b_vec.x - scale * a_vec.x,
        b_vec.y - scale * a_vec.y,
        b_vec.z - scale * a_vec.z
    );
}

LatLon radiansToDegrees(const LatLon& radian_lat_lon) {
    constexpr double rad_to_deg = 180.0 / M_PI;
    double lon_deg = radian_lat_lon.lon * rad_to_deg;

    // Normalize longitude to [0, 360)
    while (lon_deg < 0.0) {
        lon_deg += 360.0;
    }
    while (lon_deg >= 360.0) {
        lon_deg -= 360.0;
    }

    return LatLon(radian_lat_lon.lat * rad_to_deg, lon_deg);
}

Vec3 sphericalToRect(const Sphere& spherical) {
    // x = r*cos(lat)*cos(lon)
    // y = r*cos(lat)*sin(lon)
    // z = r*sin(lat)
    double r_cos_lat = spherical.radius * std::cos(spherical.lat);

    return Vec3(
        r_cos_lat * std::cos(spherical.lon),
        r_cos_lat * std::sin(spherical.lon),
        spherical.radius * std::sin(spherical.lat)
    );
}

Sphere rectToSpherical(const Vec3& rectangular) {
    double r = magnitude(rectangular);

    if (r < 1e-10) {
        return Sphere(0.0, 0.0, 0.0);
    }

    double lat = std::asin(rectangular.z / r);
    double lon = std::atan2(rectangular.y, rectangular.x);

    return Sphere(lat, lon, r);
}

double groundAzimuth(const LatLon& ground_pt, const LatLon& sub_pt) {
    constexpr double rad_to_deg = 180.0 / M_PI;

    double lat1 = sub_pt.lat;
    double lon1 = sub_pt.lon;
    double lat2 = ground_pt.lat;
    double lon2 = ground_pt.lon;

    double dlon = lon2 - lon1;

    // Handle antimeridian crossing
    if (dlon > M_PI) {
        dlon -= 2.0 * M_PI;
    } else if (dlon < -M_PI) {
        dlon += 2.0 * M_PI;
    }

    // Spherical azimuth calculation using atan2
    double x = std::sin(dlon) * std::cos(lat2);
    double y = std::cos(lat1) * std::sin(lat2) -
               std::sin(lat1) * std::cos(lat2) * std::cos(dlon);

    double azimuth_rad = std::atan2(x, y);
    double azimuth_deg = azimuth_rad * rad_to_deg;

    // Normalize to [0, 360)
    while (azimuth_deg < 0.0) {
        azimuth_deg += 360.0;
    }
    while (azimuth_deg >= 360.0) {
        azimuth_deg -= 360.0;
    }

    return azimuth_deg;
}

} // namespace math
