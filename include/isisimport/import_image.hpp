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
