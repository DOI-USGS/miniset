#include "isisimport/dispatcher.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace isisimport {

namespace {

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

const char* subdir_for(ProductType t) {
    switch (t) {
        case ProductType::PDS4:
            return "PDS4";
        case ProductType::PDS3:
        case ProductType::Qube:
        default:
            return "PDS3";
    }
}

std::string to_string_value(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    return std::string();
}

/// First non-empty resolution of a list of candidate label paths.
std::string first_of(const nlohmann::json& label,
                     std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        std::string v = to_string_value(resolve_input_path(label, p));
        if (!v.empty()) return v;
    }
    return std::string();
}

nlohmann::json load_dispatch_table(const std::string& spec_dir, std::string& err) {
    std::string path = spec_dir + "/dispatch.json";
    std::ifstream f(path);
    if (!f) {
        err = "Cannot open dispatch table: " + path;
        return nlohmann::json();
    }
    std::stringstream buf;
    buf << f.rdbuf();
    try {
        return nlohmann::json::parse(buf.str());
    } catch (const std::exception& e) {
        err = std::string("Failed to parse dispatch.json: ") + e.what();
        return nlohmann::json();
    }
}

DispatchResult make_spec_result(const std::string& spec_dir, ProductType type,
                                const std::string& spec_name) {
    DispatchResult r;
    r.spec_name = spec_name;
    r.spec_path = spec_dir + "/specs/" + subdir_for(type) + "/" + spec_name + ".json";
    if (!file_exists(r.spec_path)) {
        r.ok = false;
        r.message = "Resolved spec not found: " + r.spec_path;
    } else {
        r.ok = true;
    }
    return r;
}

}  // namespace

DispatchResult dispatch(const nlohmann::json& input_label, ProductType product_type,
                        const std::string& spec_dir) {
    DispatchResult r;
    std::string err;
    nlohmann::json table = load_dispatch_table(spec_dir, err);
    if (table.is_null()) {
        r.message = err;
        return r;
    }

    // Read SpacecraftName and InstrumentId from the usual label locations.
    std::string spacecraft = first_of(input_label,
        {"SPACECRAFT_NAME", "MISSION_NAME", "INSTRUMENT_HOST_NAME",
         "QUBE.MISSION_NAME", "QUBE.SPACECRAFT_NAME",
         "Product_Observational.Observation_Area.Investigation_Area.name"});
    std::string instrument = first_of(input_label,
        {"INSTRUMENT_ID", "QUBE.INSTRUMENT_ID",
         "Product_Observational.Observation_Area.Observing_System."
         "Observing_System_Component.1.name"});

    if (spacecraft.empty()) {
        r.message = "Could not determine SpacecraftName from label for dispatch";
        return r;
    }

    // Normalize spacecraft -> spacecraft_id and apply instrument overrides/maps.
    std::string spacecraft_id = spacecraft;
    std::string instr = instrument;
    if (table.contains("spacecraft_id_map") &&
        table["spacecraft_id_map"].contains(spacecraft)) {
        const auto& entry = table["spacecraft_id_map"][spacecraft];
        if (entry.contains("id")) spacecraft_id = entry["id"].get<std::string>();
        if (entry.contains("instr_override")) {
            instr = entry["instr_override"].get<std::string>();
        } else if (entry.contains("instr_map") && entry["instr_map"].contains(instrument)) {
            instr = entry["instr_map"][instrument].get<std::string>();
        }
    }

    // Resolve <subdir>/<spacecraft_id>/<instr> -> spec name.
    std::string key = std::string(subdir_for(product_type)) + "/" + spacecraft_id + "/" + instr;
    if (!table.contains("spec_map") || !table["spec_map"].contains(key)) {
        r.message = "No spec mapping for [" + key + "] (spacecraft=" + spacecraft +
                    ", instrument=" + instrument + "). Use --template to override.";
        return r;
    }

    return make_spec_result(spec_dir, product_type,
                            table["spec_map"][key].get<std::string>());
}

DispatchResult resolve_template_override(const std::string& override_name,
                                         ProductType product_type,
                                         const std::string& spec_dir) {
    DispatchResult r;
    // If it looks like a path (contains a separator or ends in .json), use as-is.
    bool ends_json = override_name.size() > 5 &&
                     override_name.substr(override_name.size() - 5) == ".json";
    bool looks_like_path = override_name.find('/') != std::string::npos ||
                           override_name.find('\\') != std::string::npos || ends_json;
    if (looks_like_path) {
        r.spec_name = override_name;
        r.spec_path = override_name;
        if (!file_exists(r.spec_path)) {
            r.message = "Template spec not found: " + r.spec_path;
            return r;
        }
        r.ok = true;
        return r;
    }
    return make_spec_result(spec_dir, product_type, override_name);
}

}  // namespace isisimport
