#include <gtest/gtest.h>
#include "surface/ellipsoid.hpp"
#include "core/math.hpp"
#include <cmath>

namespace ellipsoid::tests {

class EllipsoidTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Sphere with radius 10
        sphere = Ellipsoid3(10.0, 10.0, 10.0);
    }

    Ellipsoid3 sphere;
};

TEST_F(EllipsoidTest, GetSurfaceNormal) {
    Vec3 ground_pt{1.0, 0.0, 0.0};
    Vec3 normal = ellipsoid::getSurfaceNormal(sphere, ground_pt);

    EXPECT_NEAR(normal.x, 1.0, 1e-12);
    EXPECT_NEAR(normal.y, 0.0, 1e-12);
    EXPECT_NEAR(normal.z, 0.0, 1e-12);
}

TEST_F(EllipsoidTest, IntersectSurface) {
    Vec3 sensor_pos{100.0, 0.0, 0.0};
    Vec3 look_vec{-1.0, 0.0, 0.0};

    Vec3 intersection = ellipsoid::intersectSurface(sphere, sensor_pos, look_vec);

    EXPECT_NEAR(intersection.x, 10.0, 1e-10);
    EXPECT_NEAR(intersection.y, 0.0, 1e-10);
    EXPECT_NEAR(intersection.z, 0.0, 1e-10);
}

TEST_F(EllipsoidTest, IntersectSurfaceAngled) {
    // Test intersection from above - ray pointing down in +x direction
    Vec3 sensor_pos{5.0, 0.0, 20.0};
    Vec3 look_vec{0.0, 0.0, -1.0};  // Straight down

    Vec3 intersection = ellipsoid::intersectSurface(sphere, sensor_pos, look_vec);

    // Should intersect at approximately (5, 0, sqrt(75))
    double radius = math::magnitude(intersection);
    EXPECT_NEAR(radius, 10.0, 1e-9);
}

TEST_F(EllipsoidTest, GetRadiusAtEquator) {
    // For a sphere, radius should be constant
    double radius = ellipsoid::getRadiusAt(sphere, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(radius, 10.0);
}

TEST_F(EllipsoidTest, GetRadiusAtPole) {
    double radius = ellipsoid::getRadiusAt(sphere, M_PI / 2.0, 0.0);
    EXPECT_DOUBLE_EQ(radius, 10.0);
}

// Test with oblate ellipsoid (Mars-like)
TEST(EllipsoidOblateTest, RadiusAtEquator) {
    Ellipsoid3 mars(3396190.0, 3396190.0, 3376200.0);

    double radius_eq = ellipsoid::getRadiusAt(mars, 0.0, 0.0);
    EXPECT_NEAR(radius_eq, 3396190.0, 1.0);
}

TEST(EllipsoidOblateTest, RadiusAtPole) {
    Ellipsoid3 mars(3396190.0, 3396190.0, 3376200.0);

    double radius_pole = ellipsoid::getRadiusAt(mars, M_PI / 2.0, 0.0);
    EXPECT_NEAR(radius_pole, 3376200.0, 1.0);
}

TEST(EllipsoidOblateTest, IntersectOblate) {
    Ellipsoid3 mars(3396190.0, 3396190.0, 3376200.0);

    // Ray from above equator
    Vec3 sensor_pos{0.0, 0.0, 4000000.0};
    Vec3 look_vec{0.0, 0.0, -1.0};

    Vec3 intersection = ellipsoid::intersectSurface(mars, sensor_pos, look_vec);

    // Should hit the pole
    EXPECT_NEAR(intersection.x, 0.0, 1e-6);
    EXPECT_NEAR(intersection.y, 0.0, 1e-6);
    EXPECT_NEAR(intersection.z, 3376200.0, 1.0);
}

// Test batch operations
TEST(EllipsoidBatchTest, SurfaceNormalBatch) {
    Ellipsoid3 sphere(10.0, 10.0, 10.0);

    std::vector<Vec3> points = {
        {10.0, 0.0, 0.0},
        {0.0, 10.0, 0.0},
        {0.0, 0.0, 10.0}
    };

    std::vector<Vec3> normals = ellipsoid::getSurfaceNormalBatch(sphere, points);

    ASSERT_EQ(normals.size(), 3);
    EXPECT_NEAR(normals[0].x, 1.0, 1e-12);
    EXPECT_NEAR(normals[1].y, 1.0, 1e-12);
    EXPECT_NEAR(normals[2].z, 1.0, 1e-12);
}

} // namespace ellipsoid::tests
