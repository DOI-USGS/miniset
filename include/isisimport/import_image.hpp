#ifndef MINISET_ISISIMPORT_IMPORT_IMAGE_HPP
#define MINISET_ISISIMPORT_IMPORT_IMAGE_HPP

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace isisimport {

/// Output raster format. `Auto` infers the driver from the output extension.
enum class OutputFormat { Auto, Cube, GTiff, Cog };

/// Everything needed to run one import. Mirrors the ISIS `isisimport` parameters
/// plus miniset extensions (output format, GDAL creation options, spec dir).
struct ImportOptions {
    std::string from;               ///< Input PDS3/PDS4 path (local or /vsi*).
    std::string to;                 ///< Output path (local or /vsi*).
    std::string template_override;  ///< Manual spec name/path; empty = infer.
    std::string target_override;    ///< Overrides Instrument.TargetName if set.
    std::string data_dump_path;     ///< If set, dump the input-label JSON here.
    OutputFormat format = OutputFormat::Auto;
    std::vector<std::pair<std::string, std::string>> creation_options;  ///< GDAL -co.
    std::string spec_dir;           ///< Override bundled spec directory.
};

/// Result of an import. `ok` is false on failure with `message` explaining why.
struct ImportResult {
    bool ok = false;
    std::string message;
    std::string instrument_spec;    ///< Name of the spec that was applied.
    std::string output_path;
    nlohmann::ordered_json isis_label;  ///< The json:ISIS3 label that was written.
    nlohmann::json input_label;     ///< The parsed input (PDS) label.
};

/// Import a planetary image, writing an ISIS-compatible output with a json:ISIS3
/// label. Reads only the JSON specs needed to resolve the instrument.
ImportResult import_image(const ImportOptions& options);

/// Infer an OutputFormat from an output file extension (.cub/.lbl -> Cube,
/// .tif/.tiff -> GTiff). Returns Auto if the extension is unrecognized.
OutputFormat infer_format_from_extension(const std::string& path);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_IMPORT_IMAGE_HPP
