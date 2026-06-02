/**
 * Miniset Embind bindings
 * Exposes C++ classes to JavaScript
 * Does NOT call GDAL spatial reference functions (handled in JavaScript layer)
 */

#include <emscripten/bind.h>
#include <emscripten/val.h>

// Miniset headers
#include "surface/dem.hpp"
#include "surface/ellipsoid.hpp"

using namespace emscripten;

EMSCRIPTEN_BINDINGS(miniset) {
    // DEM type enum
    enum_<surface::DEMType>("DEMType")
        .value("HEIGHT", surface::DEMType::HEIGHT)
        .value("RADIUS", surface::DEMType::RADIUS);

    // EllipsoidDEM class
    class_<surface::EllipsoidDEM>("EllipsoidDEM")
        .constructor<double, double>()
        .function("getHeight", &surface::EllipsoidDEM::getHeight)
        .function("getRadius", &surface::EllipsoidDEM::getRadius)
        .function("getSemiMajorA", &surface::EllipsoidDEM::getSemiMajorA)
        .function("getSemiMajorB", &surface::EllipsoidDEM::getSemiMajorB)
        .function("getSemiMinorC", &surface::EllipsoidDEM::getSemiMinorC);

#ifdef MINISET_HAS_GDAL
    // GdalDEM class - inherits from EllipsoidDEM
    // Spatial reference extraction happens in JavaScript, not here
    class_<surface::GdalDEM, base<surface::EllipsoidDEM>>("GdalDEM")
        .constructor<const std::string&, surface::DEMType>()
        .constructor<const std::string&>()  // Default to HEIGHT
        .function("getRasterValue", &surface::GdalDEM::getRasterValue)
        .function("setProj", &surface::GdalDEM::setProj);
#endif
}
