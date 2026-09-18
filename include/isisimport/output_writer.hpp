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
