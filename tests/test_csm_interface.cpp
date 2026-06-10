#include <gtest/gtest.h>
#include "surface/intersection.hpp"
#include "core/math.hpp"

namespace surface::tests {

TEST(IntersectionTest, ComputeIntersectionDistanceZero) {
    Vec3 pt1{0.0, 0.0, 0.0};
    Vec3 pt2{0.0, 0.0, 0.0};

    double dist = computeIntersectionDistance(pt1, pt2);
    EXPECT_DOUBLE_EQ(dist, 0.0);
}

TEST(IntersectionTest, ComputeIntersectionDistanceUnit) {
    Vec3 pt1{0.0, 0.0, 0.0};
    Vec3 pt2{1.0, 1.0, 1.0};

    // Chebyshev distance is max of abs differences
    double dist = computeIntersectionDistance(pt1, pt2);
    EXPECT_DOUBLE_EQ(dist, 1.0);
}

TEST(IntersectionTest, ComputeIntersectionDistanceDifferent) {
    Vec3 pt1{0.0, 0.0, 0.0};
    Vec3 pt2{2.0, 1.0, 3.0};

    // Max difference is 3.0 (in z)
    double dist = computeIntersectionDistance(pt1, pt2);
    EXPECT_DOUBLE_EQ(dist, 3.0);
}

TEST(IntersectionTest, ComputeIntersectionDistanceNegative) {
    Vec3 pt1{5.0, 5.0, 5.0};
    Vec3 pt2{2.0, 7.0, 3.0};

    // Max abs difference: |5-2|=3, |5-7|=2, |5-3|=2
    double dist = computeIntersectionDistance(pt1, pt2);
    EXPECT_DOUBLE_EQ(dist, 3.0);
}

// CSM-specific tests require actual CSM sensors
// Note: Full CSM integration tests require actual ISD files and CSM plugins.
// These would be integration tests rather than unit tests.
} // namespace surface::tests
