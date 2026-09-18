#ifndef MINISET_ISISIMPORT_INPUT_LABEL_HPP
#define MINISET_ISISIMPORT_INPUT_LABEL_HPP

#include <string>

#include <nlohmann/json.hpp>

namespace isisimport {

/// Product family of the input label. Drives dispatch and label parsing.
enum class ProductType { PDS3, PDS4, Qube, Unknown };

/// Parse the input image's label into a JSON tree usable by translation specs.
///
/// PDS3 labels come from GDAL's `json:PDS` metadata domain (nested objects,
/// scalars/arrays, and {value,unit} keywords) so no PVL parser is required.
/// PDS4 labels are parsed from XML via GDAL's cpl_minixml (no Qt). The caret
/// pointer keys (e.g. `^IMAGE`) are aliased to `ptrIMAGE` and `:` becomes `_`
/// to match ISIS key references.
///
/// @param from        input path (local or /vsi*)
/// @param out_type    set to the detected product type
/// @throws std::runtime_error if the label cannot be read/parsed
nlohmann::json load_input_label(const std::string& from, ProductType& out_type);

/// Resolve a dotted path (e.g. "IMAGE.LINE_SAMPLES") against an input label,
/// returning the scalar value: if the node is a {value,unit} object the "value"
/// is returned, otherwise the node itself. Returns null json if not found.
nlohmann::json resolve_input_path(const nlohmann::json& label, const std::string& dotted_path);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_INPUT_LABEL_HPP
