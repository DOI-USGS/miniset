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

#ifndef MINISET_ISISIMPORT_OUTPUT_WRITER_HPP
#define MINISET_ISISIMPORT_OUTPUT_WRITER_HPP

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "isisimport/import_image.hpp"
#include "isisimport/transform_registry.hpp"

namespace isisimport {

/// Write the output raster + json:ISIS3 label.
///
/// Opens the input as the GDAL pixel source, applies the crop window from `ctx`
/// if set, attaches `isis_label` to the source via SetMetadata(..., "json:ISIS3")
/// (required: the ISIS3 driver only merges custom groups from the *source*), then
/// CreateCopy's to the driver chosen by `format`. Works with /vsi* paths.
///
/// @param err set to an explanation on failure
/// @return true on success
bool write_output(const std::string& from,
                  const std::string& to,
                  OutputFormat format,
                  const nlohmann::ordered_json& isis_label,
                  const std::vector<std::pair<std::string, std::string>>& creation_options,
                  const TransformContext& ctx,
                  std::string& err);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_OUTPUT_WRITER_HPP
