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

#include "isisimport/transform_registry.hpp"

#include <cctype>
#include <string>

#include "isisimport/input_label.hpp"

namespace isisimport {

namespace {

/// Read a scalar from the input label as a string, tolerating numeric JSON.
std::string label_string(const nlohmann::json& label, const std::string& path) {
    nlohmann::json v = resolve_input_path(label, path);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return std::string();
}

long long label_int(const nlohmann::json& label, const std::string& path, long long dflt) {
    nlohmann::json v = resolve_input_path(label, path);
    if (v.is_number()) return static_cast<long long>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
        }
    }
    return dflt;
}

/// MRO CTX prefix/suffix crop. Reproduces MroCTX.tpl arithmetic:
///   Samples = LINE_SAMPLES - endPix - suf - 1
/// where (endPix, suf) depend on SPATIAL_SUMMING/SAMPLING_FACTOR (sumMode) and
/// EDIT_MODE_ID/SAMPLE_FIRST_PIXEL (editMode). Sets a crop window on ctx so the
/// writer subsets the source raster; returns the cropped sample count for Core.
nlohmann::json mroctx_crop(const nlohmann::json& /*input_value*/,
                           const nlohmann::json& /*args*/,
                           TransformContext& ctx) {
    const nlohmann::json& label = *ctx.input_label;

    long long lineSamples = label_int(label, "IMAGE.LINE_SAMPLES", 0);
    long long lines = label_int(label, "IMAGE.LINES", 0);

    // sumMode: SPATIAL_SUMMING, falling back to SAMPLING_FACTOR.
    std::string sumMode = label_string(label, "SPATIAL_SUMMING");
    if (sumMode.empty()) sumMode = label_string(label, "SAMPLING_FACTOR");

    // editMode: EDIT_MODE_ID, falling back to SAMPLE_FIRST_PIXEL.
    std::string editMode = label_string(label, "EDIT_MODE_ID");
    if (editMode.empty()) editMode = label_string(label, "SAMPLE_FIRST_PIXEL");

    long long startPix = 0;
    long long endPix = 0;
    long long suf = 0;
    if (sumMode == "1") {
        startPix = 0;
        if (editMode == "0") {
            endPix = 37;
            suf = 18;
        } else {
            endPix = 15;
            suf = 0;
        }
    } else if (sumMode == "2") {
        if (editMode == "0") {
            startPix = 7;
            endPix = 18;
            suf = 9;
        } else {
            startPix = 0;
            endPix = 7;
            suf = 0;
        }
    }

    long long samples = lineSamples - endPix - suf - 1;
    if (samples < 0) samples = 0;

    // Crop window in source pixels: drop the leading prefix and trailing
    // (endPix + suf + 1) columns. startPix marks the first valid sample.
    if (lineSamples > 0 && samples > 0) {
        ctx.has_window = true;
        ctx.win_xoff = static_cast<int>(startPix);
        ctx.win_yoff = 0;
        ctx.win_xsize = static_cast<int>(samples);
        ctx.win_ysize = static_cast<int>(lines);
    }

    return nlohmann::json(samples);
}

/// Dawn FC NaifFrameCode. Reproduces DawnFC.tpl:
///   FC1 -> -203110 - FILTER_NUMBER,  FC2 -> -203120 - FILTER_NUMBER
nlohmann::json dawnfc_naifframecode(const nlohmann::json& /*input_value*/,
                                    const nlohmann::json& /*args*/,
                                    TransformContext& ctx) {
    const nlohmann::json& label = *ctx.input_label;
    std::string instId = label_string(label, "INSTRUMENT_ID");
    long long filt = label_int(label, "FILTER_NUMBER", 0);
    long long base = 0;
    if (instId == "FC1") {
        base = -203110;
    } else if (instId == "FC2") {
        base = -203120;
    } else {
        return nlohmann::json();  // unknown -> omit
    }
    return nlohmann::json(base - filt);
}

/// Lowercase helper.
std::string to_lower_str(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

/// Capitalize: lowercase the whole string, then uppercase the first character.
/// Reproduces isisimport's `capitalize` inja callback.
nlohmann::json capitalize(const nlohmann::json& input_value,
                          const nlohmann::json& /*args*/, TransformContext& /*ctx*/) {
    if (!input_value.is_string()) return input_value;
    std::string s = to_lower_str(input_value.get<std::string>());
    if (!s.empty()) s[0] = static_cast<char>(std::toupper((unsigned char)s[0]));
    return nlohmann::json(s);
}

/// Chandrayaan-2 TMC view code from the product file name (char at index 10:
/// 'f'->FORE, 'a'->AFT, 'n'->NADIR). Returns the view code, or "" if unknown.
std::string ch2tmc_view_code(const nlohmann::json& label) {
    std::string fn = label_string(label,
        "Product_Observational.File_Area_Observational.File.file_name");
    if (fn.size() <= 10) return std::string();
    char s = static_cast<char>(std::tolower((unsigned char)fn[10]));
    if (s == 'f') return "FORE";
    if (s == 'a') return "AFT";
    if (s == 'n') return "NADIR";
    return std::string();
}

/// Chandrayaan-2 TMC InstrumentId: CH2_TMC_<viewcode> (or CH2_TMC if no view).
nlohmann::json ch2tmc_instrument_id(const nlohmann::json& /*input_value*/,
                                    const nlohmann::json& /*args*/,
                                    TransformContext& ctx) {
    std::string view = ch2tmc_view_code(*ctx.input_label);
    return nlohmann::json(view.empty() ? "CH2_TMC" : "CH2_TMC_" + view);
}

/// Chandrayaan-2 TMC NaifFrameCode keyed on the view code.
nlohmann::json ch2tmc_naifframecode(const nlohmann::json& /*input_value*/,
                                    const nlohmann::json& /*args*/,
                                    TransformContext& ctx) {
    std::string view = ch2tmc_view_code(*ctx.input_label);
    if (view == "AFT") return nlohmann::json(-152212);
    if (view == "FORE") return nlohmann::json(-152211);
    if (view == "NADIR") return nlohmann::json(-152210);
    return nlohmann::json();
}

/// Strip a trailing 'Z' from a time string (added when an image is reingested).
/// Reproduces isisimport's RemoveStartTimeZ inja callback. Operates on the
/// resolved input value; returns null (→ keyword uses its default) if absent.
nlohmann::json remove_start_time_z(const nlohmann::json& input_value,
                                   const nlohmann::json& /*args*/,
                                   TransformContext& /*ctx*/) {
    if (!input_value.is_string()) return input_value;
    std::string s = input_value.get<std::string>();
    if (!s.empty() && s.back() == 'Z') s.pop_back();
    return nlohmann::json(s);
}

}  // namespace

void TransformRegistry::registerFn(const std::string& name, TransformFn fn) {
    fns_[name] = std::move(fn);
}

const TransformFn* TransformRegistry::find(const std::string& name) const {
    auto it = fns_.find(name);
    return it == fns_.end() ? nullptr : &it->second;
}

const TransformRegistry& TransformRegistry::builtin() {
    static const TransformRegistry registry = [] {
        TransformRegistry r;
        r.registerFn("mroctx_crop", &mroctx_crop);
        r.registerFn("dawnfc_naifframecode", &dawnfc_naifframecode);
        r.registerFn("remove_start_time_z", &remove_start_time_z);
        r.registerFn("capitalize", &capitalize);
        r.registerFn("ch2tmc_instrument_id", &ch2tmc_instrument_id);
        r.registerFn("ch2tmc_naifframecode", &ch2tmc_naifframecode);
        return r;
    }();
    return registry;
}

}  // namespace isisimport
