#include <gtest/gtest.h>
#include "sensor/angles.hpp"
#include "core/math.hpp"
#include <cmath>

namespace sensor::tests {

TEST(AnglesTest, ComputePhaseAngle) {
    // Ground point at origin
    Vec3 ground_pt{0.0, 0.0, 0.0};

    // Sensor and sun at 90-degree angle (should give 45 degrees phase)
    Vec3 sensor_pos{100.0, 0.0, 0.0};
    Vec3 sun_pos{100.0, 100.0, 0.0};

    double phase = computePhaseAngle(ground_pt, sensor_pos, sun_pos);

    // Phase angle should be 45 degrees (pi/4 radians)
    EXPECT_NEAR(phase, M_PI / 4.0, 1e-10);
}

TEST(AnglesTest, ComputePhaseAngleZero) {
    // Sensor and sun in same direction -> phase = 0
    Vec3 ground_pt{0.0, 0.0, 0.0};
    Vec3 sensor_pos{100.0, 0.0, 0.0};
    Vec3 sun_pos{200.0, 0.0, 0.0};

    double phase = computePhaseAngle(ground_pt, sensor_pos, sun_pos);

    EXPECT_NEAR(phase, 0.0, 1e-10);
}

TEST(AnglesTest, ComputePhaseAngle180) {
    // Sensor and sun in opposite directions -> phase = 180
    Vec3 ground_pt{0.0, 0.0, 0.0};
    Vec3 sensor_pos{100.0, 0.0, 0.0};
    Vec3 sun_pos{-100.0, 0.0, 0.0};

    double phase = computePhaseAngle(ground_pt, sensor_pos, sun_pos);

    EXPECT_NEAR(phase, M_PI, 1e-10);
}

TEST(AnglesTest, ComputeEmissionAngleSphere) {
    // For a sphere centered at origin, with ground point on x-axis
    Vec3 ground_pt{10.0, 0.0, 0.0};
    Vec3 sensor_pos{100.0, 0.0, 0.0};

    // Surface normal at (10, 0, 0) points in +x direction
    Vec3 surface_normal{1.0, 0.0, 0.0};

    // Look vector from sensor to ground
    Vec3 look_vec = math::unitVector(Vec3{
        ground_pt.x - sensor_pos.x,
        ground_pt.y - sensor_pos.y,
        ground_pt.z - sensor_pos.z
    });

    double emission = computeEmissionAngle(surface_normal, look_vec);

    // Look vector is -x, normal is +x, so angle should be 180 degrees
    EXPECT_NEAR(emission, M_PI, 1e-10);
}

TEST(AnglesTest, ComputeIncidenceAngle) {
    // Ground point on sphere
    Vec3 ground_pt{10.0, 0.0, 0.0};
    Vec3 sun_pos{100.0, 0.0, 0.0};

    // Surface normal pointing outward
    Vec3 surface_normal{1.0, 0.0, 0.0};

    double incidence = computeIncidenceAngle(ground_pt, sun_pos, surface_normal);

    // Sun direction and normal are aligned -> incidence = 0
    EXPECT_NEAR(incidence, 0.0, 1e-10);
}

TEST(AnglesTest, ComputeIncidenceAngle90) {
    // Ground point on sphere
    Vec3 ground_pt{10.0, 0.0, 0.0};
    Vec3 sun_pos{10.0, 100.0, 0.0};  // Sun to the side

    // Surface normal pointing in +x
    Vec3 surface_normal{1.0, 0.0, 0.0};

    double incidence = computeIncidenceAngle(ground_pt, sun_pos, surface_normal);

    // Sun direction is +y, normal is +x -> 90 degrees
    EXPECT_NEAR(incidence, M_PI / 2.0, 1e-10);
}

// Test complete angle computation
TEST(AnglesTest, ComputeSensorAnglesComplete) {
    // Ground point on sphere at radius 10
    Vec3 ground_pt{10.0, 0.0, 0.0};
    // Sensor directly along +x axis
    Vec3 sensor_pos{20.0, 0.0, 0.0};
    // Sun at 45 degrees in xy plane from ground point
    Vec3 sun_pos{20.0, 10.0, 0.0};
    // Surface normal for sphere at this point
    Vec3 surface_normal{1.0, 0.0, 0.0};
    // Look vector from ground to sensor
    Vec3 look_vec{10.0, 0.0, 0.0};

    // Compute angles individually
    double phase = computePhaseAngle(ground_pt, sensor_pos, sun_pos);
    double emission = computeEmissionAngle(surface_normal, look_vec);
    double incidence = computeIncidenceAngle(ground_pt, sun_pos, surface_normal);

    // Phase: angle between to_sensor and to_sun from ground
    // to_sensor = (10, 0, 0), to_sun = (10, 10, 0) -> 45 degrees
    EXPECT_NEAR(phase, M_PI / 4.0, 1e-10);

    // Emission: angle between surface normal (1,0,0) and look vec (10,0,0) -> 0 degrees
    EXPECT_NEAR(emission, 0.0, 1e-10);

    // Incidence: angle between surface normal and to_sun (10,10,0) -> 45 degrees
    EXPECT_NEAR(incidence, M_PI / 4.0, 1e-10);
}

TEST(AnglesTest, SlantDistance) {
    Vec3 sensor_pos{100.0, 0.0, 0.0};
    Vec3 ground_pt{0.0, 0.0, 0.0};

    double slant = computeSlantDistance(sensor_pos, ground_pt);

    EXPECT_DOUBLE_EQ(slant, 100.0);
}

TEST(AnglesTest, SlantDistance3D) {
    Vec3 sensor_pos{3.0, 4.0, 0.0};
    Vec3 ground_pt{0.0, 0.0, 0.0};

    double slant = computeSlantDistance(sensor_pos, ground_pt);

    EXPECT_DOUBLE_EQ(slant, 5.0);
}

TEST(AnglesTest, TargetCenterDistance) {
    Vec3 sensor_pos{-100.0, 0.0, 0.0};

    double distance = computeTargetCenterDistance(sensor_pos);

    EXPECT_DOUBLE_EQ(distance, 100.0);
}

TEST(AnglesTest, LocalRadius) {
    Vec3 ground_pt{10.0, 0.0, 0.0};

    double radius = computeLocalRadius(ground_pt);

    EXPECT_DOUBLE_EQ(radius, 10.0);
}

} // namespace sensor::tests
