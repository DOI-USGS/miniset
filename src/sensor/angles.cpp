#include "sensor/angles.hpp"
#include "core/math.hpp"
#include <cmath>

namespace sensor {

double computePhaseAngle(const Vec3& ground_pt, const Vec3& sensor_pos, const Vec3& sun_pos) {
    // Phase angle is the angle between sensor and sun as seen from the ground point
    Vec3 to_sensor{
        sensor_pos.x - ground_pt.x,
        sensor_pos.y - ground_pt.y,
        sensor_pos.z - ground_pt.z
    };

    Vec3 to_sun{
        sun_pos.x - ground_pt.x,
        sun_pos.y - ground_pt.y,
        sun_pos.z - ground_pt.z
    };

    return math::separationAngle(to_sensor, to_sun);
}

double computeEmissionAngle(const Vec3& surface_normal, const Vec3& look_vec) {
    // Emission angle is the angle between surface normal and look vector
    // Note: look_vec should point from ground to sensor
    return math::separationAngle(surface_normal, look_vec);
}

double computeIncidenceAngle(const Vec3& ground_pt, const Vec3& sun_pos, const Vec3& surface_normal) {
    // Incidence angle is the angle between surface normal and sun direction
    Vec3 to_sun{
        sun_pos.x - ground_pt.x,
        sun_pos.y - ground_pt.y,
        sun_pos.z - ground_pt.z
    };

    return math::separationAngle(surface_normal, to_sun);
}

double computeSlantDistance(const Vec3& sensor_pos, const Vec3& ground_pt) {
    return math::distance(sensor_pos, ground_pt);
}

double computeTargetCenterDistance(const Vec3& sensor_pos) {
    Vec3 origin{0.0, 0.0, 0.0};
    return math::distance(sensor_pos, origin);
}

double computeLocalRadius(const Vec3& ground_pt) {
    return math::magnitude(ground_pt);
}

} // namespace sensor
