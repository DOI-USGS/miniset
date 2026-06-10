#include <gtest/gtest.h>
#include "surface/dem.hpp"
#include <cmath>

namespace surface::tests {

class EllipsoidDEMTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Mars ellipsoid
        dem = new EllipsoidDEM(3396190.0, 3376200.0);
    }

    void TearDown() override {
        delete dem;
    }

    EllipsoidDEM* dem;
};

TEST_F(EllipsoidDEMTest, HeightAtEquator) {
    // Pure ellipsoid has zero height (degrees)
    double height = dem->getHeight(0.0, 0.0);
    EXPECT_DOUBLE_EQ(height, 0.0);
}

TEST_F(EllipsoidDEMTest, HeightAtPole) {
    // 90 degrees lat, 0 degrees lon
    double height = dem->getHeight(90.0, 0.0);
    EXPECT_DOUBLE_EQ(height, 0.0);
}

TEST_F(EllipsoidDEMTest, HeightAtArbitraryPoint) {
    // 45 degrees lat, 45 degrees lon
    double height = dem->getHeight(45.0, 45.0);
    EXPECT_DOUBLE_EQ(height, 0.0);
}

TEST_F(EllipsoidDEMTest, RadiusAtEquator) {
    // 0 lat, 0 lon (degrees)
    double radius = dem->getRadius(0.0, 0.0);
    EXPECT_DOUBLE_EQ(radius, 3396190.0);
}

TEST_F(EllipsoidDEMTest, RadiusAtEquator180) {
    // 0 lat, 180 lon (degrees)
    double radius = dem->getRadius(0.0, 180.0);
    EXPECT_DOUBLE_EQ(radius, 3396190.0);
}

TEST_F(EllipsoidDEMTest, RadiusAtPole) {
    // 90 lat, 300 lon (degrees)
    double radius = dem->getRadius(90.0, 300.0);
    EXPECT_NEAR(radius, 3376200.0, 1.0);
}

TEST_F(EllipsoidDEMTest, GetRadii) {
    EXPECT_DOUBLE_EQ(dem->getSemiMajorA(), 3396190.0);
    EXPECT_DOUBLE_EQ(dem->getSemiMajorB(), 3396190.0);
    EXPECT_DOUBLE_EQ(dem->getSemiMinorC(), 3376200.0);
}

// Test triaxial ellipsoid
TEST(TriaxialDEMTest, AllRadiiDifferent) {
    // Create triaxial with custom radii
    EllipsoidDEM triaxial(100.0, 90.0);

    EXPECT_DOUBLE_EQ(triaxial.getSemiMajorA(), 100.0);
    EXPECT_DOUBLE_EQ(triaxial.getSemiMajorB(), 100.0);
    EXPECT_DOUBLE_EQ(triaxial.getSemiMinorC(), 90.0);
}
} // namespace surface::tests
