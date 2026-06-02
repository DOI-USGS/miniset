#include "surface/ellipsoid.hpp"
#include "core/math.hpp"
#include <cmath>
#include <stdexcept>

namespace ellipsoid {

Vec3 getSurfaceNormal(const Ellipsoid3& e, const Vec3& ground_pt) {
    // Surface normal for ellipsoid: grad(x²/a² + y²/b² + z²/c²)
    // = (2x/a², 2y/b², 2z/c²)
    Vec3 normal;
    normal.x = ground_pt.x / (e.a * e.a);
    normal.y = ground_pt.y / (e.b * e.b);
    normal.z = ground_pt.z / (e.c * e.c);

    // Normalize to unit vector
    return math::unitVector(normal);
}

Vec3 intersectSurface(const Ellipsoid3& e, const Vec3& sensor_pos, const Vec3& look_vec) {
    // Ray: P(t) = sensor_pos + t * look_vec
    // Ellipsoid: (x/a)² + (y/b)² + (z/c)² = 1
    //
    // Substituting ray into ellipsoid equation gives quadratic in t:
    // At² + Bt + C = 0

    double dx = look_vec.x / e.a;
    double dy = look_vec.y / e.b;
    double dz = look_vec.z / e.c;

    double px = sensor_pos.x / e.a;
    double py = sensor_pos.y / e.b;
    double pz = sensor_pos.z / e.c;

    double A = dx * dx + dy * dy + dz * dz;
    double B = 2.0 * (px * dx + py * dy + pz * dz);
    double C = px * px + py * py + pz * pz - 1.0;

    double discriminant = B * B - 4.0 * A * C;

    if (discriminant < 0) {
        // No intersection - return closest point on ray to origin
        double t_closest = -B / (2.0 * A);
        return Vec3(
            sensor_pos.x + t_closest * look_vec.x,
            sensor_pos.y + t_closest * look_vec.y,
            sensor_pos.z + t_closest * look_vec.z
        );
    }

    // Two solutions - take the closer one (smaller positive t)
    double sqrt_disc = std::sqrt(discriminant);
    double t1 = (-B - sqrt_disc) / (2.0 * A);
    double t2 = (-B + sqrt_disc) / (2.0 * A);

    double t = (t1 >= 0) ? t1 : t2;

    return Vec3(
        sensor_pos.x + t * look_vec.x,
        sensor_pos.y + t * look_vec.y,
        sensor_pos.z + t * look_vec.z
    );
}

double getRadiusAt(const Ellipsoid3& e, double lat_rad, double lon_rad) {
    // Radius at geodetic latitude and longitude
    // r = (a*b*c) / sqrt((b*c*cos(lat)*cos(lon))² + (a*c*cos(lat)*sin(lon))² + (a*b*sin(lat))²)

    double cos_lat = std::cos(lat_rad);
    double sin_lat = std::sin(lat_rad);
    double cos_lon = std::cos(lon_rad);
    double sin_lon = std::sin(lon_rad);

    double bc_cos_lat_cos_lon = e.b * e.c * cos_lat * cos_lon;
    double ac_cos_lat_sin_lon = e.a * e.c * cos_lat * sin_lon;
    double ab_sin_lat = e.a * e.b * sin_lat;

    double denom = std::sqrt(
        bc_cos_lat_cos_lon * bc_cos_lat_cos_lon +
        ac_cos_lat_sin_lon * ac_cos_lat_sin_lon +
        ab_sin_lat * ab_sin_lat
    );

    if (denom < 1e-10) {
        return e.a;  // Fallback to semi-major axis
    }

    return (e.a * e.b * e.c) / denom;
}

// Batch operations
std::vector<Vec3> getSurfaceNormalBatch(const Ellipsoid3& e, const std::vector<Vec3>& points) {
    std::vector<Vec3> normals;
    normals.reserve(points.size());

    for (const auto& pt : points) {
        normals.push_back(getSurfaceNormal(e, pt));
    }

    return normals;
}

std::vector<Vec3> intersectSurfaceBatch(const Ellipsoid3& e,
                                         const std::vector<Vec3>& sensor_positions,
                                         const std::vector<Vec3>& look_vectors) {
    if (sensor_positions.size() != look_vectors.size()) {
        throw std::invalid_argument("sensor_positions and look_vectors must have same size");
    }

    std::vector<Vec3> intersections;
    intersections.reserve(sensor_positions.size());

    for (size_t i = 0; i < sensor_positions.size(); ++i) {
        intersections.push_back(intersectSurface(e, sensor_positions[i], look_vectors[i]));
    }

    return intersections;
}

} // namespace ellipsoid
