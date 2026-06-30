/*
 * Copyright (C) 2024 USGS Astrogeology Science Center
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef MINISET_SURFACE_SHAPE_MODEL_HPP
#define MINISET_SURFACE_SHAPE_MODEL_HPP

#include "surface/dem.hpp"
#include "core/types.hpp"

namespace miniset {

// Shape model types
enum class ShapeModelType {
    CONSTANT_HEIGHT,  // Uniform elevation
    DEM,              // Raster digital elevation model
    TIN,              // Triangulated irregular network (future)
    POINT_CLOUD       // Point cloud (future)
};

// Tagged union for different shape model data
struct ShapeModel {
    ShapeModelType type;
    Ellipsoid3 ellipsoid;

    union {
        double constant_height;           // For CONSTANT_HEIGHT
        surface::EllipsoidDEM* dem;       // For DEM
        void* tin_data;                   // For TIN (future)
        void* point_cloud_data;           // For POINT_CLOUD (future)
    } data;
};

// Free functions for querying shape models (degrees only)
double getShapeModelHeight(const ShapeModel& model, double lat_deg, double lon_deg);
double getShapeModelRadius(const ShapeModel& model, double lat_deg, double lon_deg);

// Construction helpers
ShapeModel createConstantHeightShapeModel(double height, const Ellipsoid3& ellipsoid);
ShapeModel createShapeModelFromDEM(surface::EllipsoidDEM* dem);

} // namespace miniset

#endif // MINISET_SURFACE_SHAPE_MODEL_HPP
