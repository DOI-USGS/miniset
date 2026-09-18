#include "isisimport/import_image.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "isisimport/dispatcher.hpp"
#include "isisimport/input_label.hpp"
#include "isisimport/output_writer.hpp"
#include "isisimport/spec_paths.hpp"
#include "isisimport/translation_spec.hpp"
#include "isisimport/transform_registry.hpp"

namespace isisimport {

namespace {

std::string lower_ext(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

}  // namespace

OutputFormat infer_format_from_extension(const std::string& path) {
    std::string ext = lower_ext(path);
    if (ext == "cub" || ext == "lbl") return OutputFormat::Cube;
    if (ext == "tif" || ext == "tiff") return OutputFormat::GTiff;
    return OutputFormat::Auto;
}

ImportResult import_image(const ImportOptions& options) {
    ImportResult result;
    result.output_path = options.to;

    if (options.from.empty() || options.to.empty()) {
        result.message = "Both FROM and TO are required";
        return result;
    }

    // 1. Load and detect the input label.
    ProductType product_type = ProductType::Unknown;
    try {
        result.input_label = load_input_label(options.from, product_type);
    } catch (const std::exception& e) {
        result.message = std::string("Failed to read input label: ") + e.what();
        return result;
    }

    // 2. Optional debug dump of the input JSON label.
    if (!options.data_dump_path.empty()) {
        std::ofstream f(options.data_dump_path);
        if (f) f << result.input_label.dump(2);
    }

    // 3. Resolve the spec directory and the instrument spec (infer, or override).
    std::string spec_dir = resolve_spec_dir(options.spec_dir);
    if (spec_dir.empty()) {
        result.message = "Could not locate import spec directory. Set MINISET_APPDATA "
                         "or pass --spec-dir.";
        return result;
    }

    DispatchResult dispatch_result;
    if (!options.template_override.empty()) {
        dispatch_result =
            resolve_template_override(options.template_override, product_type, spec_dir);
    } else {
        dispatch_result = dispatch(result.input_label, product_type, spec_dir);
    }
    if (!dispatch_result.ok) {
        result.message = dispatch_result.message;
        return result;
    }
    result.instrument_spec = dispatch_result.spec_name;

    // 4. Load + apply the spec to build the json:ISIS3 label.
    TransformContext ctx;
    try {
        TranslationSpec spec = load_spec(dispatch_result.spec_path);
        result.isis_label =
            apply_spec(spec, result.input_label, TransformRegistry::builtin(), ctx);
    } catch (const std::exception& e) {
        result.message = std::string("Translation failed: ") + e.what();
        return result;
    }

    // 5. Apply the TARGET override into Instrument.TargetName if requested.
    if (!options.target_override.empty() &&
        result.isis_label.contains("IsisCube") &&
        result.isis_label["IsisCube"].contains("Instrument")) {
        result.isis_label["IsisCube"]["Instrument"]["TargetName"] = options.target_override;
    }

    // 6. Determine output format (infer from extension when Auto).
    OutputFormat format = options.format;
    if (format == OutputFormat::Auto) {
        format = infer_format_from_extension(options.to);
        if (format == OutputFormat::Auto) {
            result.message = "Cannot infer output format from '" + options.to +
                             "'. Use --format cub|gtiff|cog.";
            return result;
        }
    }

    // 7. Write the output raster + label.
    std::string err;
    if (!write_output(options.from, options.to, format, result.isis_label,
                      options.creation_options, ctx, err)) {
        result.message = err;
        return result;
    }

    result.ok = true;
    result.message = "Imported " + options.from + " -> " + options.to;
    return result;
}

}  // namespace isisimport
