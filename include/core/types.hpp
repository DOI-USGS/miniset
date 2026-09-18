#ifndef MINISET_CORE_TYPES_HPP
#define MINISET_CORE_TYPES_HPP

#include <vector>
#include <cstddef>


// Simple 3D vector type
struct Vec3 {
    double x, y, z;

    Vec3() : x(0.0), y(0.0), z(0.0) {}
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

// Image coordinate (line, sample)
struct ImageCoord {
    double line;
    double sample;

    ImageCoord() : line(0.0), sample(0.0) {}
    ImageCoord(double line_, double sample_) : line(line_), sample(sample_) {}
};

// Ellipsoid surface model (biaxial or triaxial)
struct Ellipsoid3 {
    double a;  // Semi-major axis (equatorial, x-axis)
    double b;  // Semi-median axis (equatorial, y-axis)
    double c;  // Semi-minor axis (polar, z-axis)

    Ellipsoid3() : a(0.0), b(0.0), c(0.0) {}
    Ellipsoid3(double a_, double b_, double c_) : a(a_), b(b_), c(c_) {}
};

// Latitude/Longitude in radians
struct LatLon {
    double lat;
    double lon;

    LatLon() : lat(0.0), lon(0.0) {}
    LatLon(double lat_, double lon_) : lat(lat_), lon(lon_) {}
};

// Spherical coordinates
struct Sphere {
    double lat;    // radians
    double lon;    // radians
    double radius; // meters

    Sphere() : lat(0.0), lon(0.0), radius(0.0) {}
    Sphere(double lat_, double lon_, double radius_)
        : lat(lat_), lon(lon_), radius(radius_) {}
};


#endif // MINISET_CORE_TYPES_HPP
