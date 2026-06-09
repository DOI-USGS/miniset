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


#include "csm/triangulation.hpp"
#include "core/math.hpp"

namespace csm {

#ifdef MINISET_HAS_CSMAPI
Vec3 triangulateGroundPoint(const std::vector<::csm::RasterGM*>& cameras,
                            const std::vector<std::pair<double,double>>& image_pts) {
    // Placeholder - requires full implementation
    if (cameras.empty() || image_pts.empty()) {
        return Vec3(0, 0, 0);
    }
    // Simplified: just use first camera
    ::csm::ImageCoord img(image_pts[0].first, image_pts[0].second);
    ::csm::EcefCoord ground = cameras[0]->imageToGround(img, 0.0);
    return Vec3(ground.x, ground.y, ground.z);
}
#endif

} // namespace csm
