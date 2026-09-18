#include "surface/intersection.hpp"
#include "surface/dem.hpp"
#include "core/math.hpp"
#include <cmath>
#include <algorithm>

namespace surface {

double computeIntersectionDistance(const Vec3& pt1, const Vec3& pt2) {
    // Chebyshev distance (max of abs differences)
    double dx = std::abs(pt1.x - pt2.x);
    double dy = std::abs(pt1.y - pt2.y);
    double dz = std::abs(pt1.z - pt2.z);
    return std::max({dx, dy, dz});
}

Vec3 intersectWithDEM(const Vec3& sensor_pos, const Vec3& look_vec,
                      const class EllipsoidDEM& dem,
                      double tolerance, int max_iterations) {
    // Iterative DEM intersection with refinement
    // Start with ellipsoid intersection
    double a = dem.getSemiMajorA();
    double b = dem.getSemiMajorB();
    double c = dem.getSemiMinorC();

    // Ray: P(t) = sensor_pos + t * look_vec
    // Ellipsoid: (x/a)² + (y/b)² + (z/c)² = 1
    double dx = look_vec.x / a;
    double dy = look_vec.y / b;
    double dz = look_vec.z / c;

    double px = sensor_pos.x / a;
    double py = sensor_pos.y / b;
    double pz = sensor_pos.z / c;

    double A = dx * dx + dy * dy + dz * dz;
    double B = 2.0 * (px * dx + py * dy + pz * dz);
    double C = px * px + py * py + pz * pz - 1.0;

    double discriminant = B * B - 4.0 * A * C;

    Vec3 current_pt;
    if (discriminant < 0) {
        // No intersection - return closest point
        double t_closest = -B / (2.0 * A);
        current_pt = Vec3{
            sensor_pos.x + t_closest * look_vec.x,
            sensor_pos.y + t_closest * look_vec.y,
            sensor_pos.z + t_closest * look_vec.z
        };
    } else {
        double sqrt_disc = std::sqrt(discriminant);
        double t1 = (-B - sqrt_disc) / (2.0 * A);
        double t2 = (-B + sqrt_disc) / (2.0 * A);
        double t = (t1 >= 0) ? t1 : t2;

        current_pt = Vec3{
            sensor_pos.x + t * look_vec.x,
            sensor_pos.y + t * look_vec.y,
            sensor_pos.z + t * look_vec.z
        };
    }

    // Iterative refinement for DEM
    for (int iter = 0; iter < max_iterations; ++iter) {
        // Convert ECEF to lat/lon
        double x = current_pt.x;
        double y = current_pt.y;
        double z = current_pt.z;

        double lon = std::atan2(y, x);
        double p = std::sqrt(x * x + y * y);
        double lat = std::atan2(z, p);

        // Get DEM radius at this point (convert radians to degrees)
        double lat_deg = lat * 180.0 / M_PI;
        double lon_deg = lon * 180.0 / M_PI;
        double dem_radius = dem.getRadius(lat_deg, lon_deg);

        // Compute scale factor to reach DEM surface
        double current_radius = math::magnitude(current_pt);
        if (current_radius < 1e-10) {
            break;
        }

        Vec3 new_pt{
            current_pt.x * (dem_radius / current_radius),
            current_pt.y * (dem_radius / current_radius),
            current_pt.z * (dem_radius / current_radius)
        };

        // Check convergence
        double dist = computeIntersectionDistance(current_pt, new_pt);
        current_pt = new_pt;

        if (dist < tolerance) {
            break;
        }
    }

    return current_pt;
}

} // namespace surface
