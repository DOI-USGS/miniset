#include <gtest/gtest.h>
#include "core/math.hpp"
#include <cmath>

namespace math {
namespace tests {

// Test separation angle for right angle (90 degrees)
TEST(MathTest, SeparationAngleRight) {
    Vec3 a_vec{1.0, 0.0, 0.0};
    Vec3 b_vec{0.0, 1.0, 0.0};

    double angle = separationAngle(a_vec, b_vec);

    EXPECT_NEAR(angle, M_PI / 2.0, 1e-12);
}

// Test separation angle for parallel vectors
TEST(MathTest, SeparationAngleParallel) {
    Vec3 a_vec{1.0, 0.0, 0.0};
    Vec3 b_vec{2.0, 0.0, 0.0};

    double angle = separationAngle(a_vec, b_vec);

    EXPECT_NEAR(angle, 0.0, 1e-12);
}

// Test separation angle for opposite vectors
TEST(MathTest, SeparationAngleOpposite) {
    Vec3 a_vec{1.0, 0.0, 0.0};
    Vec3 b_vec{-1.0, 0.0, 0.0};

    double angle = separationAngle(a_vec, b_vec);

    EXPECT_NEAR(angle, M_PI, 1e-12);
}

// Test magnitude
TEST(MathTest, Magnitude) {
    Vec3 vec{3.0, 4.0, 0.0};
    EXPECT_DOUBLE_EQ(magnitude(vec), 5.0);
}

// Test magnitude for zero vector
TEST(MathTest, MagnitudeZero) {
    Vec3 vec{0.0, 0.0, 0.0};
    EXPECT_DOUBLE_EQ(magnitude(vec), 0.0);
}

// Test distance between two points
TEST(MathTest, Distance) {
    Vec3 pt1{1.0, 0.0, 0.0};
    Vec3 pt2{0.0, 1.0, 0.0};
    EXPECT_NEAR(distance(pt1, pt2), std::sqrt(2.0), 1e-12);
}

// Test distance for same point
TEST(MathTest, DistanceSame) {
    Vec3 pt{1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(distance(pt, pt), 0.0);
}

// Test unit vector
TEST(MathTest, UnitVector) {
    Vec3 vec{3.0, 4.0, 0.0};
    Vec3 unit = unitVector(vec);

    EXPECT_NEAR(unit.x, 0.6, 1e-12);
    EXPECT_NEAR(unit.y, 0.8, 1e-12);
    EXPECT_NEAR(unit.z, 0.0, 1e-12);
    EXPECT_NEAR(magnitude(unit), 1.0, 1e-12);
}

// Test cross product
TEST(MathTest, CrossProduct) {
    Vec3 a{1.0, 0.0, 0.0};
    Vec3 b{0.0, 1.0, 0.0};
    Vec3 cross = crossProduct(a, b);

    EXPECT_DOUBLE_EQ(cross.x, 0.0);
    EXPECT_DOUBLE_EQ(cross.y, 0.0);
    EXPECT_DOUBLE_EQ(cross.z, 1.0);
}

// Test dot product
TEST(MathTest, DotProduct) {
    Vec3 a{1.0, 2.0, 3.0};
    Vec3 b{4.0, 5.0, 6.0};

    double dot = dotProduct(a, b);

    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    EXPECT_DOUBLE_EQ(dot, 32.0);
}

// Test scale vector
TEST(MathTest, ScaleVector) {
    Vec3 vec{1.0, 2.0, 3.0};
    Vec3 scaled = scaleVector(vec, 2.0);

    EXPECT_DOUBLE_EQ(scaled.x, 2.0);
    EXPECT_DOUBLE_EQ(scaled.y, 4.0);
    EXPECT_DOUBLE_EQ(scaled.z, 6.0);
}

// Test perpendicular vector
TEST(MathTest, PerpendicularVector) {
    Vec3 a{1.0, 0.0, 0.0};
    Vec3 b{1.0, 1.0, 0.0};

    Vec3 perp = perpendicularVector(a, b);

    // The perpendicular component should be (0, 1, 0)
    EXPECT_NEAR(perp.x, 0.0, 1e-12);
    EXPECT_NEAR(perp.y, 1.0, 1e-12);
    EXPECT_NEAR(perp.z, 0.0, 1e-12);

    // Verify it's perpendicular to a
    EXPECT_NEAR(dotProduct(perp, a), 0.0, 1e-12);
}

// Test spherical to rectangular conversion
TEST(MathTest, SphericalToRect) {
    // Test point at equator, prime meridian
    Sphere sph{0.0, 0.0, 1.0};
    Vec3 rect = sphericalToRect(sph);

    EXPECT_NEAR(rect.x, 1.0, 1e-12);
    EXPECT_NEAR(rect.y, 0.0, 1e-12);
    EXPECT_NEAR(rect.z, 0.0, 1e-12);
}

// Test rectangular to spherical conversion
TEST(MathTest, RectToSpherical) {
    Vec3 rect{1.0, 0.0, 0.0};
    Sphere sph = rectToSpherical(rect);

    EXPECT_NEAR(sph.lat, 0.0, 1e-12);
    EXPECT_NEAR(sph.lon, 0.0, 1e-12);
    EXPECT_NEAR(sph.radius, 1.0, 1e-12);
}

} // namespace tests
} // namespace math
