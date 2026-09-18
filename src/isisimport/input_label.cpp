#include "isisimport/input_label.hpp"

#include <sstream>
#include <stdexcept>

#include <cpl_conv.h>
#include <cpl_minixml.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_priv.h>

namespace isisimport {

namespace {

/// Sanitize a PDS/PDS4 key to match ISIS key references: a leading `^` (pointer)
/// becomes a `ptr` prefix, and `:` becomes `_`.
std::string sanitize_key(const std::string& key) {
    std::string out = key;
    if (!out.empty() && out[0] == '^') {
        out = "ptr" + out.substr(1);
    }
    for (char& c : out) {
        if (c == ':') c = '_';
    }
    return out;
}

/// Recursively sanitize all object keys in a JSON tree (in place copy).
nlohmann::json sanitize_keys(const nlohmann::json& in) {
    if (in.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = in.begin(); it != in.end(); ++it) {
            out[sanitize_key(it.key())] = sanitize_keys(it.value());
        }
        return out;
    }
    if (in.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : in) out.push_back(sanitize_keys(e));
        return out;
    }
    return in;
}

/// Convert a cpl_minixml element node into JSON, mirroring ISIS XmlToJson:
/// element name -> object key; repeated siblings -> array; attributes folded in
/// (prefixed "attrib_"); text content becomes the value (or "_text" when the
/// element also has attributes/children). Empty elements become null.
nlohmann::json xml_node_to_json(const CPLXMLNode* node) {
    // Collect attributes, child elements, and text.
    nlohmann::json obj = nlohmann::json::object();
    std::string text;
    bool has_children = false;
    bool has_attribs = false;

    for (const CPLXMLNode* child = node->psChild; child; child = child->psNext) {
        if (child->eType == CXT_Attribute) {
            has_attribs = true;
            const char* val = child->psChild ? child->psChild->pszValue : "";
            obj[std::string("attrib_") + sanitize_key(child->pszValue)] = val ? val : "";
        } else if (child->eType == CXT_Element) {
            has_children = true;
            const std::string key = sanitize_key(child->pszValue);
            nlohmann::json child_json = xml_node_to_json(child);
            if (obj.contains(key)) {
                if (!obj[key].is_array()) {
                    nlohmann::json arr = nlohmann::json::array();
                    arr.push_back(obj[key]);
                    obj[key] = arr;
                }
                obj[key].push_back(child_json);
            } else {
                obj[key] = child_json;
            }
        } else if (child->eType == CXT_Text) {
            text += child->pszValue ? child->pszValue : "";
        }
    }

    if (!has_children && !has_attribs) {
        // Leaf: value is the text (or null if empty).
        if (text.empty()) return nlohmann::json();
        return text;
    }
    if (!text.empty()) {
        // Trim surrounding whitespace before storing element text.
        size_t b = text.find_first_not_of(" \t\r\n");
        size_t e = text.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) obj["_text"] = text.substr(b, e - b + 1);
    }
    return obj;
}

nlohmann::json load_pds3(GDALDatasetH ds) {
    CSLConstList md = GDALGetMetadata(ds, "json:PDS");
    if (md == nullptr || md[0] == nullptr) {
        throw std::runtime_error("GDAL PDS driver produced no json:PDS label");
    }
    // The domain is a single JSON string (possibly split across list entries).
    std::string joined;
    for (int i = 0; md[i] != nullptr; ++i) joined += md[i];
    nlohmann::json parsed = nlohmann::json::parse(joined);
    return sanitize_keys(parsed);
}

nlohmann::json load_pds4(const std::string& from) {
    // Parse the XML label directly (no Qt) via cpl_minixml.
    CPLXMLNode* root = CPLParseXMLFile(from.c_str());
    if (root == nullptr) {
        throw std::runtime_error("Failed to parse PDS4 XML label: " + from);
    }
    // Skip to the first real element. CPL reports the <?xml?> declaration and
    // <?xml-model?> processing instructions as CXT_Element nodes named "?xml"/
    // "?xml-model", so skip anything that is not an element or whose name starts
    // with '?'.
    const CPLXMLNode* elem = root;
    while (elem && (elem->eType != CXT_Element ||
                    (elem->pszValue && elem->pszValue[0] == '?'))) {
        elem = elem->psNext;
    }
    if (elem == nullptr) {
        CPLDestroyXMLNode(root);
        throw std::runtime_error("PDS4 XML has no root element: " + from);
    }
    nlohmann::json out = nlohmann::json::object();
    out[sanitize_key(elem->pszValue)] = xml_node_to_json(elem);
    CPLDestroyXMLNode(root);
    return out;
}

/// Detect the product family from the parsed label, following fileTemplate.tpl.
ProductType detect_type(const nlohmann::json& label) {
    auto exists = [&](const std::string& path) {
        return !resolve_input_path(label, path).is_null();
    };
    if (exists("Product_Observational.product_class") ||
        exists("Product_Observational.Identification_Area.product_class")) {
        return ProductType::PDS4;
    }
    if (label.contains("QUBE")) return ProductType::Qube;
    if (label.contains("PDS_VERSION_ID") ||
        label.contains("CCSD3ZF0000100000001NJPL3IF0PDS200000001")) {
        return ProductType::PDS3;
    }
    return ProductType::Unknown;
}

}  // namespace

nlohmann::json resolve_input_path(const nlohmann::json& label,
                                  const std::string& dotted_path) {
    const nlohmann::json* cur = &label;
    std::stringstream ss(dotted_path);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (cur->is_object() && cur->contains(part)) {
            cur = &(*cur)[part];
        } else if (cur->is_array()) {
            // Allow numeric indices into arrays (e.g. Axis_Array.1).
            try {
                size_t idx = std::stoul(part);
                if (idx < cur->size()) {
                    cur = &(*cur)[idx];
                    continue;
                }
            } catch (...) {
            }
            return nlohmann::json();
        } else {
            return nlohmann::json();
        }
    }
    // Unwrap common keyword shapes to their scalar payload:
    //  - PDS3 json:PDS keyword: {"value": X, "unit": ...}  -> X
    //  - PDS4 element with attributes: {"_text": X, "attrib_*": ...} -> X
    // This lets specs reference either the element (e.g. isda_focal_length) or
    // its text child (isda_focal_length._text) interchangeably, matching ISIS.
    if (cur->is_object()) {
        if (cur->contains("value")) return (*cur)["value"];
        if (cur->contains("_text")) return (*cur)["_text"];
    }
    return *cur;
}

nlohmann::json load_input_label(const std::string& from, ProductType& out_type) {
    GDALAllRegister();

    // First try opening with GDAL (covers PDS3 and, when readable, PDS4).
    GDALDatasetH ds = GDALOpen(from.c_str(), GA_ReadOnly);

    nlohmann::json label;
    if (ds != nullptr) {
        const char* drv = GDALGetDriverShortName(GDALGetDatasetDriver(ds));
        if (drv != nullptr && std::string(drv) == "PDS4") {
            GDALClose(ds);
            label = load_pds4(from);
        } else {
            try {
                label = load_pds3(ds);
            } catch (...) {
                GDALClose(ds);
                throw;
            }
            GDALClose(ds);
        }
    } else {
        // GDAL couldn't open it as a raster; try parsing as a PDS4 XML label.
        label = load_pds4(from);
    }

    out_type = detect_type(label);
    return label;
}

}  // namespace isisimport
