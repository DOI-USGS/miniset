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

#include "isisimport/translation_spec.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "isisimport/input_label.hpp"

namespace isisimport {

namespace {

std::vector<std::string> split_path(const std::string& dotted) {
    std::vector<std::string> parts;
    std::stringstream ss(dotted);
    std::string p;
    while (std::getline(ss, p, '.')) parts.push_back(p);
    return parts;
}

std::string join_path(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '.';
        out += parts[i];
    }
    return out;
}

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open spec file: " + path);
    std::stringstream buf;
    buf << f.rdbuf();
    try {
        return nlohmann::json::parse(buf.str());
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse spec " + path + ": " + e.what());
    }
}

KeywordSpec parse_keyword(const nlohmann::json& j) {
    KeywordSpec k;
    k.out = j.at("out").get<std::string>();
    if (j.contains("from")) k.from = split_path(j["from"].get<std::string>());
    if (j.contains("fallback_from"))
        k.fallback_from = split_path(j["fallback_from"].get<std::string>());
    if (j.contains("default")) {
        k.has_default = true;
        k.default_value = j["default"];
    }
    if (j.contains("const")) {
        k.has_const = true;
        k.const_value = j["const"];
    }
    if (j.contains("unit")) k.unit = j["unit"].get<std::string>();
    if (j.contains("type")) k.type = j["type"].get<std::string>();
    if (j.contains("enum")) {
        k.has_enum = true;
        for (auto it = j["enum"].begin(); it != j["enum"].end(); ++it) {
            k.enum_map[it.key()] = it.value();
        }
    }
    if (j.contains("array")) k.array = j["array"].get<bool>();
    if (j.contains("transform")) k.transform = j["transform"].get<std::string>();
    if (j.contains("transform_args")) k.transform_args = j["transform_args"];
    return k;
}

GroupSpec parse_group(const nlohmann::json& j) {
    GroupSpec g;
    g.name = j.at("name").get<std::string>();
    if (j.contains("parent")) g.parent = j["parent"].get<std::string>();
    if (j.contains("keywords")) {
        for (const auto& kw : j["keywords"]) g.keywords.push_back(parse_keyword(kw));
    }
    return g;
}

/// Merge child groups over base groups: a child group with the same
/// (parent,name) replaces the base group; otherwise it is appended.
std::vector<GroupSpec> merge_groups(std::vector<GroupSpec> base,
                                    const std::vector<GroupSpec>& child) {
    for (const auto& cg : child) {
        bool replaced = false;
        for (auto& bg : base) {
            if (bg.name == cg.name && bg.parent == cg.parent) {
                bg = cg;
                replaced = true;
                break;
            }
        }
        if (!replaced) base.push_back(cg);
    }
    return base;
}

void parse_into(const nlohmann::json& j, TranslationSpec& spec) {
    if (j.contains("name")) spec.name = j["name"].get<std::string>();
    if (j.contains("product_type")) spec.product_type = j["product_type"].get<std::string>();
    if (j.contains("core")) {
        const auto& c = j["core"];
        if (c.contains("derive_from_gdal"))
            spec.core.derive_from_gdal = c["derive_from_gdal"].get<bool>();
        if (c.contains("transform")) {
            spec.core.transform = c["transform"].get<std::string>();
            spec.core.derive_from_gdal = false;
        }
        if (c.contains("transform_args")) spec.core.transform_args = c["transform_args"];
    }
    if (j.contains("groups")) {
        std::vector<GroupSpec> groups;
        for (const auto& g : j["groups"]) groups.push_back(parse_group(g));
        spec.groups = merge_groups(std::move(spec.groups), groups);
    }
}

/// Coerce a JSON value to the requested spec type.
nlohmann::json coerce_type(const nlohmann::json& v, const std::string& type) {
    if (type == "auto" || type.empty()) return v;
    try {
        std::string s = v.is_string() ? v.get<std::string>() : v.dump();
        if (type == "int") return nlohmann::json(std::stoll(s));
        if (type == "double") return nlohmann::json(std::stod(s));
        if (type == "string") return nlohmann::json(v.is_string() ? v.get<std::string>()
                                                                   : (v.is_null() ? "" : v.dump()));
    } catch (...) {
    }
    return v;
}

/// Trim surrounding ASCII whitespace from a string.
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Clean a resolved value for output: trim whitespace on strings (GDAL's json:PDS
/// can carry trailing spaces, e.g. "0 "), and unwrap per-element {value,unit}
/// objects to their scalar value (GDAL emits arrays-with-units this way; ISIS
/// flattens them to a plain value list with a single keyword unit). Recurses.
nlohmann::json clean_value(const nlohmann::json& v) {
    if (v.is_string()) return trim(v.get<std::string>());
    if (v.is_object() && v.contains("value")) return clean_value(v["value"]);
    if (v.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : v) out.push_back(clean_value(e));
        return out;
    }
    return v;
}

/// If a value is a string that parses cleanly as a number, return it as a JSON
/// number; otherwise return it unchanged. GDAL's ISIS3 cube writer only emits a
/// value+unit keyword when the value is numeric, so unit-bearing keywords whose
/// text came from the label (e.g. PDS4 element text "7") must be numeric.
nlohmann::json numeric_if_possible(const nlohmann::json& v) {
    if (!v.is_string()) return v;
    const std::string& s = v.get_ref<const std::string&>();
    if (s.empty()) return v;
    try {
        size_t pos = 0;
        // Prefer integer when the whole string is an integer.
        long long iv = std::stoll(s, &pos);
        if (pos == s.size()) return nlohmann::json(iv);
        double dv = std::stod(s, &pos);
        if (pos == s.size()) return nlohmann::json(dv);
    } catch (...) {
    }
    return v;
}

/// Apply an enum map: exact key match wins, else "*" fallthrough, else identity.
nlohmann::json apply_enum(const KeywordSpec& k, const nlohmann::json& v) {
    std::string key = v.is_string() ? v.get<std::string>()
                                    : (v.is_null() ? "" : v.dump());
    auto it = k.enum_map.find(key);
    if (it != k.enum_map.end()) return it->second;
    auto star = k.enum_map.find("*");
    if (star != k.enum_map.end()) return star->second;
    return v;  // identity
}

}  // namespace

TranslationSpec load_spec(const std::string& spec_path) {
    nlohmann::json j = read_json_file(spec_path);

    TranslationSpec spec;
    // Resolve a single `extends` base first so child groups override it.
    if (j.contains("extends") && j["extends"].is_string() &&
        !j["extends"].get<std::string>().empty()) {
        std::string base = j["extends"].get<std::string>();
        // Base lives next to the spec, named "<base>.json".
        std::string dir = spec_path.substr(0, spec_path.find_last_of("/\\") + 1);
        nlohmann::json bj = read_json_file(dir + base + ".json");
        parse_into(bj, spec);
    }
    parse_into(j, spec);
    return spec;
}

nlohmann::ordered_json apply_spec(const TranslationSpec& spec,
                                  const nlohmann::json& input_label,
                                  const TransformRegistry& registry,
                                  TransformContext& ctx) {
    using ojson = nlohmann::ordered_json;
    ctx.input_label = &input_label;

    // Run the Core transform (if any) early so it can set the crop window.
    // Its return value is not placed in the label; the ISIS3/GTiff driver
    // regenerates Core from the (cropped) raster.
    if (!spec.core.transform.empty()) {
        const TransformFn* fn = registry.find(spec.core.transform);
        if (fn == nullptr) {
            throw std::runtime_error("Unknown core transform: " + spec.core.transform);
        }
        (*fn)(nlohmann::json(), spec.core.transform_args, ctx);
    }

    ojson label = ojson::object();
    ojson& isis_cube = label["IsisCube"];
    isis_cube["_type"] = "object";

    for (const GroupSpec& g : spec.groups) {
        // Locate/create the parent object (default IsisCube).
        ojson* parent = &label;
        if (g.parent == "IsisCube") {
            parent = &isis_cube;
        } else if (label.contains(g.parent)) {
            parent = &label[g.parent];
        } else {
            label[g.parent] = ojson::object();
            label[g.parent]["_type"] = "object";
            parent = &label[g.parent];
        }

        ojson group = ojson::object();
        group["_type"] = "group";

        for (const KeywordSpec& k : g.keywords) {
            nlohmann::json value;
            bool have_value = false;

            if (k.has_const) {
                value = k.const_value;
                have_value = true;
            } else if (!k.from.empty()) {
                nlohmann::json resolved = resolve_input_path(input_label, join_path(k.from));
                if (resolved.is_null() && !k.fallback_from.empty()) {
                    resolved = resolve_input_path(input_label, join_path(k.fallback_from));
                }
                if (!resolved.is_null()) {
                    value = clean_value(resolved);
                    have_value = true;
                }
            }

            // Named transform (rare for pure instruments).
            if (!k.transform.empty()) {
                const TransformFn* fn = registry.find(k.transform);
                if (fn == nullptr) {
                    throw std::runtime_error("Unknown transform: " + k.transform);
                }
                value = (*fn)(have_value ? value : nlohmann::json(), k.transform_args, ctx);
                have_value = !value.is_null();
            }

            if (!have_value) {
                if (k.has_default) {
                    value = k.default_value;
                } else {
                    continue;  // omit keyword entirely (matches ISIS {% if exists %})
                }
            }

            if (k.has_enum) value = apply_enum(k, value);
            value = coerce_type(value, k.type);

            // Attach a unit as {value, unit} for scalars. GDAL's ISIS3 cube
            // writer only serializes scalar value+unit when the value is numeric
            // (it silently drops string-value+unit and array-value+unit). So for
            // a scalar we coerce a numeric-looking string value to a number, and
            // for arrays we emit the bare array (values preserved; ISIS just
            // doesn't get the unit). GTiff/COG keep the full form regardless.
            if (!k.unit.empty() && !value.is_array()) {
                ojson kv = ojson::object();
                kv["value"] = numeric_if_possible(value);
                kv["unit"] = k.unit;
                group[k.out] = kv;
            } else {
                group[k.out] = value;
            }
        }

        (*parent)[g.name] = group;
    }

    return label;
}

}  // namespace isisimport
