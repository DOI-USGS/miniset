#ifndef MINISET_CSM_TRIANGULATION_HPP
#define MINISET_CSM_TRIANGULATION_HPP

#include "core/types.hpp"
#include <vector>

#ifdef MINISET_HAS_CSMAPI
#include <csm/RasterGM.h>
#endif

namespace csm {

#ifdef MINISET_HAS_CSMAPI
// Multi-ray triangulation (closest approach)
Vec3 triangulateGroundPoint(const std::vector<::csm::RasterGM*>& cameras,
                            const std::vector<std::pair<double,double>>& image_pts);
#endif

} // namespace csm

#endif
