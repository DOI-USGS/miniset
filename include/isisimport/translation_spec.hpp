#ifndef MINISET_ISISIMPORT_TRANSLATION_SPEC_HPP
#define MINISET_ISISIMPORT_TRANSLATION_SPEC_HPP

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "isisimport/transform_registry.hpp"

namespace isisimport {

/// One output keyword, parsed once from the spec into a flat struct.
struct KeywordSpec {
    std::string out;                        ///< Output keyword name.
    std::vector<std::string> from;          ///< Pre-split dotted input path.
    std::vector<std::string> fallback_from; ///< Used if `from` is missing.
    bool has_default = false;
    nlohmann::json default_value;
    bool has_const = false;
    nlohmann::json const_value;
    std::string unit;                       ///< Non-empty -> emit {value,unit}.
    std::string type = "auto";              ///< string|int|double|auto.
    bool has_enum = false;
    std::unordered_map<std::string, nlohmann::json> enum_map;  ///< value map.
    bool array = false;                     ///< Emit as JSON array (PVL tuple).
    std::string transform;                  ///< Named transform (may be empty).
    nlohmann::json transform_args;
};

/// A group of keywords under a parent object (e.g. IsisCube.Instrument).
struct GroupSpec {
    std::string name;
    std::string parent = "IsisCube";
    std::vector<KeywordSpec> keywords;
};

/// How Core dimensions/pixels are produced.
struct CoreSpec {
    bool derive_from_gdal = true;   ///< Let the GDAL driver build Core.
    std::string transform;          ///< Optional named transform (e.g. crop).
    nlohmann::json transform_args;
};

/// A fully parsed instrument translation spec.
struct TranslationSpec {
    std::string name;
    std::string product_type;
    CoreSpec core;
    std::vector<GroupSpec> groups;
};

/// Load and parse a spec file, resolving a single `extends` base if present.
/// Only the files needed (the spec + its base) are read.
/// @throws std::runtime_error on read/parse error.
TranslationSpec load_spec(const std::string& spec_path);

/// Apply a parsed spec to an input label, producing the json:ISIS3 label object
/// (nested objects/groups tagged "_type"). Transforms are looked up in the
/// registry; a spec-level Core transform may set a crop window on `ctx`.
///
/// The result is an `ordered_json` so groups and keywords keep the spec's order
/// (which mirrors the ISIS template), rather than being alphabetized. GDAL's
/// ISIS3/GTiff writers preserve this order in the json:ISIS3 domain.
nlohmann::ordered_json apply_spec(const TranslationSpec& spec,
                                  const nlohmann::json& input_label,
                                  const TransformRegistry& registry,
                                  TransformContext& ctx);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_TRANSLATION_SPEC_HPP
