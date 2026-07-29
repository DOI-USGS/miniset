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

// image_import: import a PDS3/PDS4 image to an ISIS-compatible output (cube,
// GeoTIFF, COG) with a json:ISIS3 label. Also installed as the ISIS-compat
// alias isisimport (symlink; see cmake/MinisetApps.cmake). Accepts ISIS-style
// KEY=VALUE and GNU --flag/positional parameters.

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "../external/argparse.hpp"
#include "isisimport/import_image.hpp"

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Basename of argv[0], so each alias reports the name it was invoked as.
std::string program_name(const char* argv0) {
    std::string p = argv0 ? argv0 : "image_import";
    std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Reserved parameter names (lowercased) that may be given ISIS-style KEY=VALUE.
bool is_reserved(const std::string& key_lower) {
    static const std::vector<std::string> reserved = {
        "from", "to", "template", "target", "data", "format", "co", "spec-dir"};
    return std::find(reserved.begin(), reserved.end(), key_lower) != reserved.end();
}

// Translate ISIS-style KEY=VALUE tokens into GNU --key value pairs so one
// argparse definition handles both styles. `CO=k=v` keeps the inner '='.
std::vector<std::string> normalize_args(int argc, char** argv) {
    std::vector<std::string> out;
    out.emplace_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string tok = argv[i];
        if (tok.size() >= 2 && tok[0] == '-') {  // leave --flags / negatives
            out.push_back(tok);
            continue;
        }
        std::size_t eq = tok.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = tok.substr(0, eq);
            std::string val = tok.substr(eq + 1);
            std::string key_lower = to_lower(key);
            if (is_reserved(key_lower)) {
                out.push_back("--" + key_lower);
                out.push_back(val);
                continue;
            }
        }
        out.push_back(tok);  // bare token -> positional
    }
    return out;
}

isisimport::OutputFormat parse_format(const std::string& f) {
    std::string lf = to_lower(f);
    if (lf == "cub" || lf == "cube" || lf == "isis3") return isisimport::OutputFormat::Cube;
    if (lf == "gtiff" || lf == "tif" || lf == "tiff") return isisimport::OutputFormat::GTiff;
    if (lf == "cog") return isisimport::OutputFormat::Cog;
    return isisimport::OutputFormat::Auto;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string prog = program_name(argv[0]);

    argparse::ArgumentParser program(prog, "1.0");
    program.add_description(
        "Import a PDS3/PDS4 planetary image to an ISIS-compatible output "
        "(ISIS3 cube, GeoTIFF, or COG) with a json:ISIS3 label.\n"
        "Accepts ISIS-style (FROM=in.IMG TO=out.cub) and GNU-style "
        "(--from in.IMG --to out.cub, or positional) parameters.");

    program.add_argument("--from").default_value(std::string()).help("Input PDS image (local or /vsi*)");
    program.add_argument("--to").default_value(std::string()).help("Output path (local or /vsi*)");
    program.add_argument("--template").default_value(std::string())
        .help("Manual instrument spec override (name or path); default is inferred");
    program.add_argument("--target").default_value(std::string())
        .help("Override the target name in the Instrument group");
    program.add_argument("--data").default_value(std::string())
        .help("Dump the parsed input label JSON to this path");
    program.add_argument("--format").default_value(std::string())
        .help("Output format: cub|gtiff|cog (default: infer from TO extension)");
    program.add_argument("--spec-dir").default_value(std::string())
        .help("Override the import spec directory");
    program.add_argument("--co").append().default_value(std::vector<std::string>())
        .help("GDAL creation option KEY=VALUE (repeatable)");
    program.add_argument("pos_from").remaining().help("Positional FROM [TO]");

    std::vector<std::string> args = normalize_args(argc, argv);
    try {
        program.parse_args(args);
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n" << program;
        return 1;
    }

    isisimport::ImportOptions opts;
    opts.from = program.get<std::string>("--from");
    opts.to = program.get<std::string>("--to");
    opts.template_override = program.get<std::string>("--template");
    opts.target_override = program.get<std::string>("--target");
    opts.data_dump_path = program.get<std::string>("--data");
    opts.spec_dir = program.get<std::string>("--spec-dir");

    try {  // positional from/to when not given as flags
        auto pos = program.get<std::vector<std::string>>("pos_from");
        if (opts.from.empty() && pos.size() >= 1) opts.from = pos[0];
        if (opts.to.empty() && pos.size() >= 2) opts.to = pos[1];
    } catch (...) {
    }

    std::string fmt = program.get<std::string>("--format");
    if (!fmt.empty()) opts.format = parse_format(fmt);

    for (const auto& co : program.get<std::vector<std::string>>("--co")) {
        std::size_t eq = co.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Invalid --co (expected KEY=VALUE): " << co << "\n";
            return 1;
        }
        opts.creation_options.emplace_back(co.substr(0, eq), co.substr(eq + 1));
    }

    if (opts.from.empty() || opts.to.empty()) {
        std::cerr << "Error: FROM and TO are required.\n" << program;
        return 1;
    }

    isisimport::ImportResult result = isisimport::import_image(opts);
    if (!result.ok) {
        std::cerr << prog << " failed: " << result.message << "\n";
        return 1;
    }
    std::cout << result.message << "\n";
    std::cout << "  spec: " << result.instrument_spec << "\n";
    return 0;
}
