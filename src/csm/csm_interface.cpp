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


#include "csm/csm_interface.hpp"
#include "utils/logging.hpp"
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <dlfcn.h>
#endif

#include <cstdlib>

#include <gdal_priv.h>
#include <cpl_string.h>
#include <csm/SettableEllipsoid.h>
#include <csm/Plugin.h>
#include <csm/Isd.h>
#include <csm/Model.h>
#include <csm/Warning.h>
#include <csm/Error.h>

#include <nlohmann/json.hpp>

namespace csm {

// Helper: Load/find USGSCSM plugin
// Consolidates duplicate plugin loading logic from createCsmFromISD, loadFromIsd, and createCsmFromStateString
// Native builds: Dynamically load plugin library via dlopen
// WASM builds: Plugin is statically linked, just find it
static const ::csm::Plugin* loadUsgsPlugin([[maybe_unused]] const char* additional_path = nullptr) {
    static const ::csm::Plugin* cached_plugin = nullptr;

    // Return cached plugin if already loaded
    if (cached_plugin) {
        return cached_plugin;
    }

#ifdef __EMSCRIPTEN__
    // WASM: Plugin is statically linked and already registered
    cached_plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
    if (!cached_plugin) {
        throw std::runtime_error("Could not find UsgsAstroPluginCSM plugin (should be statically linked in WASM)");
    }
#else
    // Native: Load plugin library dynamically via dlopen
    static void* plugin_handle = nullptr;

    if (!plugin_handle) {
        std::string plugin_path;
        const char* conda_prefix = std::getenv("CONDA_PREFIX");

        if (conda_prefix) {
#ifdef __APPLE__
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.dylib";
#else
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.so";
#endif
            plugin_handle = dlopen(plugin_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        }

        // Fallback paths
        if (!plugin_handle) {
            std::vector<const char*> fallback_paths = {
                "../lib/csmplugins/libusgscsm.dylib",
                "../lib/csmplugins/libusgscsm.so",
                "./libusgscsm.dylib",
                "./libusgscsm.so",
                "libusgscsm.dylib",
                "libusgscsm.so"
            };

            // Add caller-specified path if provided
            if (additional_path) {
                fallback_paths.insert(fallback_paths.begin(), additional_path);
            }

            for (const char* path : fallback_paths) {
                plugin_handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
                if (plugin_handle) break;
            }
        }

        if (!plugin_handle) {
            std::string error_msg = "Failed to load USGSCSM plugin library";
            if (conda_prefix) {
                error_msg += " (tried " + plugin_path + ")";
            }
            error_msg += ": " + std::string(dlerror());
            throw std::runtime_error(error_msg);
        }
    }

    // Find plugin
    cached_plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
    if (!cached_plugin) {
        throw std::runtime_error("Could not find UsgsAstroPluginCSM plugin");
    }
#endif

    return cached_plugin;
}


std::pair<double, double> getRadii(const ::csm::RasterGM* camera) {
    if (!camera) throw std::invalid_argument("Null camera");

    // Try to cast to SettableEllipsoid to get ellipsoid parameters
    const ::csm::SettableEllipsoid* settable =
        dynamic_cast<const ::csm::SettableEllipsoid*>(camera);

    if (settable) {
        ::csm::Ellipsoid ellipsoid = settable->getEllipsoid();
        return {ellipsoid.getSemiMajorRadius(), ellipsoid.getSemiMinorRadius()};
    }

    // Fallback to WGS84 if cast fails
    return {6378137.0, 6356752.314245};
}

SensorState getSensorState(const ::csm::RasterGM* sensor, double line, double sample) {
    if (!sensor) throw std::invalid_argument("Null sensor");
    
    SensorState state;
    ::csm::ImageCoord img_pt(line, sample);
    
    state.sensor_time = sensor->getImageTime(img_pt);
    
    ::csm::EcefLocus locus = sensor->imageToRemoteImagingLocus(img_pt);
    state.sensor_pos = Vec3(locus.point.x, locus.point.y, locus.point.z);
    state.look_vec = Vec3(locus.direction.x, locus.direction.y, locus.direction.z);
    
    return state;
}

::csm::RasterGM* createCsmFromISD(const std::string& isd_file_or_json) {
#ifdef __EMSCRIPTEN__
    // WASM: Plugin is statically linked, already registered via Embind
    // Accept JSON string and write to virtual filesystem
    std::string temp_file = "/tmp/model.json";
    std::ofstream ofs(temp_file);
    if (!ofs.is_open()) {
        throw std::runtime_error("Failed to create temporary ISD file in virtual filesystem");
    }
    ofs << isd_file_or_json;
    ofs.close();

    // Create ISD from temporary file
    ::csm::Isd isd(temp_file);
#else
    // Native: USGSCSM reads the JSON file directly when filename is set
    ::csm::Isd isd(isd_file_or_json);
#endif

    // Load plugin (cached after first call)
    const ::csm::Plugin* plugin = loadUsgsPlugin();

    // Try all available sensor models until one works
    // USGS CSM plugin supports: FRAME, LINE_SCANNER, PROJECTED, SAR, PUSH_FRAME
    const char* model_names[] = {
        "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL",  // Most common (CTX, HiRISE, etc.)
        "USGS_ASTRO_FRAME_SENSOR_MODEL",         // Frame cameras (MDIS, etc.)
        "USGS_ASTRO_PUSH_FRAME_SENSOR_MODEL",    // Push frame
        "USGS_ASTRO_PROJECTED_SENSOR_MODEL",     // Projected/ortho
        "USGS_ASTRO_SAR_SENSOR_MODEL",           // SAR
        nullptr
    };

    ::csm::Model* model = nullptr;
    ::csm::WarningList warnings;
    std::string last_error;

    for (const char** model_name = model_names; *model_name != nullptr; ++model_name) {
        try {
            // Check if this model can be constructed
            if (plugin->canModelBeConstructedFromISD(isd, *model_name)) {
                model = plugin->constructModelFromISD(isd, *model_name, &warnings);
                if (model) break;  // Success!
            }
        } catch (const ::csm::Error& e) {
            last_error = std::string(e.getMessage());
            continue;  // Try next model
        }
    }

    if (!model) {
        throw std::runtime_error("Failed to construct any sensor model from ISD. Last error: " + last_error);
    }

    // Cast to RasterGM
    ::csm::RasterGM* raster_model = dynamic_cast<::csm::RasterGM*>(model);
    if (!raster_model) {
        delete model;
        throw std::runtime_error("Model is not a RasterGM");
    }

    // Print warnings if any
    for (const auto& warning : warnings) {
        // Could log these, but for now just ignore
        (void)warning;
    }

    return raster_model;
}

::csm::RasterGM* loadFromIsd(const std::string& isd_json) {
    // This function explicitly handles in-memory ISD JSON strings
    // It does NOT accept file paths - use createCsmFromISD for that

#ifdef __EMSCRIPTEN__
    // WASM: Write JSON to virtual filesystem
    std::string temp_file = "/tmp/isd_from_json.json";
    std::ofstream ofs(temp_file);
    if (!ofs.is_open()) {
        throw std::runtime_error("Failed to create temporary ISD file in virtual filesystem");
    }
    ofs << isd_json;
    ofs.close();

    ::csm::Isd isd(temp_file);
#else
    // Native: Write JSON to temporary file for USGSCSM to read
    // USGSCSM requires a file path, not in-memory JSON
    std::string temp_file = "/tmp/miniset_isd_" + std::to_string(std::rand()) + ".json";
    std::ofstream ofs(temp_file);
    if (!ofs.is_open()) {
        throw std::runtime_error("Failed to create temporary ISD file: " + temp_file);
    }
    ofs << isd_json;
    ofs.close();

    ::csm::Isd isd(temp_file);
#endif

    // Load plugin (cached after first call)
    const ::csm::Plugin* plugin = loadUsgsPlugin();

    // Try all available sensor models
    const char* model_names[] = {
        "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL",
        "USGS_ASTRO_FRAME_SENSOR_MODEL",
        "USGS_ASTRO_PUSH_FRAME_SENSOR_MODEL",
        "USGS_ASTRO_PROJECTED_SENSOR_MODEL",
        "USGS_ASTRO_SAR_SENSOR_MODEL",
        nullptr
    };

    ::csm::Model* model = nullptr;
    ::csm::WarningList warnings;
    std::string last_error;

    for (const char** model_name = model_names; *model_name != nullptr; ++model_name) {
        try {
            if (plugin->canModelBeConstructedFromISD(isd, *model_name)) {
                model = plugin->constructModelFromISD(isd, *model_name, &warnings);
                if (model) break;
            }
        } catch (const ::csm::Error& e) {
            last_error = std::string(e.getMessage());
            continue;
        }
    }

    if (!model) {
        throw std::runtime_error("Failed to construct sensor model from ISD JSON. Last error: " + last_error);
    }

    // Cast to RasterGM
    ::csm::RasterGM* raster_model = dynamic_cast<::csm::RasterGM*>(model);
    if (!raster_model) {
        // OWNERSHIP: Caller responsible for cleanup on failure - delete allocated model before throwing
        delete model;
        throw std::runtime_error("Model is not a RasterGM");
    }
    // OWNERSHIP: Caller now owns raster_model - must call deleteModel() to free

    return raster_model;
}

::csm::RasterGM* createCsmFromStateString(const std::string& state_string) {
    // Load plugin (cached after first call)
    const ::csm::Plugin* plugin = loadUsgsPlugin();
    MINISET_LOG_DEBUG("Plugin found, ptr: ", reinterpret_cast<uintptr_t>(plugin));

    // Construct model from state string
    ::csm::WarningList warnings;
    ::csm::Model* model = nullptr;

    MINISET_LOG_DEBUG("About to call constructModelFromState, state length: ", state_string.length());
    MINISET_LOG_DEBUG("First 100 chars of state: ", state_string.substr(0, 100));
    MINISET_LOG_DEBUG("=== IMMEDIATELY BEFORE constructModelFromState CALL ===");

    try {
        model = plugin->constructModelFromState(state_string, &warnings);
        MINISET_LOG_DEBUG("=== IMMEDIATELY AFTER constructModelFromState CALL ===");
        MINISET_LOG_DEBUG("constructModelFromState succeeded, model ptr: ", reinterpret_cast<uintptr_t>(model));
    } catch (const ::csm::Error& e) {
        MINISET_LOG_ERROR("CSM Error caught: ", e.getMessage());
        throw std::runtime_error(std::string("Failed to construct model from state: ") + e.getMessage());
    } catch (const std::exception& e) {
        MINISET_LOG_ERROR("std::exception caught: ", e.what());
        throw;
    } catch (...) {
        MINISET_LOG_ERROR("Unknown exception caught in constructModelFromState");
        throw std::runtime_error("Unknown error in constructModelFromState");
    }

    if (!model) {
        MINISET_LOG_ERROR("constructModelFromState returned null");
        throw std::runtime_error("Failed to construct model from state string");
    }

    // Cast to RasterGM
    ::csm::RasterGM* raster_model = dynamic_cast<::csm::RasterGM*>(model);
    if (!raster_model) {
        // OWNERSHIP: Caller responsible for cleanup on failure - delete allocated model before throwing
        delete model;
        throw std::runtime_error("Model is not a RasterGM");
    }
    // OWNERSHIP: Caller now owns raster_model - must call deleteModel() to free

    return raster_model;
}

::csm::RasterGM* createCsmFromAttachedSpice(const std::string& image_path) {
    MINISET_LOG_DEBUG("createCsmFromAttachedSpice: ", image_path);

    // Check file size in VFS
    struct stat st;
    if (stat(image_path.c_str(), &st) == 0) {
        MINISET_LOG_DEBUG("File size in VFS: ", st.st_size, " bytes");
    } else {
        MINISET_LOG_ERROR("File not found in VFS via stat()");
    }

    GDALDataset* dataset = (GDALDataset*)GDALOpen(image_path.c_str(), GA_ReadOnly);
    if (!dataset) {
        MINISET_LOG_ERROR("GDALOpen failed for: ", image_path);
        throw std::runtime_error("Failed to open image: " + image_path);
    }

    MINISET_LOG_DEBUG("GDALOpen succeeded, dataset size: ", dataset->GetRasterXSize(), "x", dataset->GetRasterYSize());

    try {
        nlohmann::json csm_state_json;

        // Check driver type to determine metadata structure
        const char* driver_name = dataset->GetDriver()->GetDescription();
        bool is_isis3_cube = (strcmp(driver_name, "ISIS3") == 0);

        MINISET_LOG_DEBUG("Driver: ", driver_name, ", is_isis3: ", is_isis3_cube);

        if (is_isis3_cube) {
            // ISIS3 cube: GDAL returns entire label as single JSON string
            // Need to parse it and extract String_CSMState object
            CSLConstList metadata = dataset->GetMetadata("json:ISIS3");
            if (!metadata || !metadata[0]) {
                throw std::runtime_error(
                    "No json:ISIS3 metadata found in ISIS3 cube."
                );
            }

            // Parse the full ISIS label JSON
            nlohmann::json full_json;
            try {
                full_json = nlohmann::json::parse(metadata[0]);
            } catch (const nlohmann::json::parse_error& e) {
                throw std::runtime_error(std::string("Failed to parse ISIS3 label JSON: ") + e.what());
            }

            // Look for String_CSMState at top level of the ISIS label
            if (full_json.contains("String_CSMState")) {
                csm_state_json = full_json["String_CSMState"];
            } else {
                throw std::runtime_error(
                    "No CSM state found in ISIS3 cube metadata (String_CSMState). "
                    "Cube may not have been processed with ISIS csminit."
                );
            }
        } else {
            // GeoTIFF, COG, or other format: GDAL exposes each ISIS3 label object as separate metadata item
            MINISET_LOG_DEBUG("Reading metadata from GeoTIFF");

            // Debug: List all metadata domains
            char** domains = dataset->GetMetadataDomainList();
            if (domains) {
                MINISET_LOG_DEBUG("Available metadata domains:");
                for (int i = 0; domains[i] != nullptr; i++) {
                    MINISET_LOG_DEBUG("  - ", domains[i]);
                }
                CSLDestroy(domains);
            } else {
                MINISET_LOG_DEBUG("No metadata domains found");
            }
            const char* csm_state_json_str = dataset->GetMetadataItem("String_CSMState", "json:ISIS3");

            if (!csm_state_json_str) {
                // GetMetadataItem returned null - GDAL might be returning entire label as one blob
                // Try reading the full json:ISIS3 metadata and extracting String_CSMState
                MINISET_LOG_DEBUG("GetMetadataItem returned null, trying full metadata");
                CSLConstList metadata = dataset->GetMetadata("json:ISIS3");
                if (!metadata || !metadata[0]) {
                    throw std::runtime_error(
                        "No json:ISIS3 metadata found in image. "
                        "Image may not have been processed with ISIS csminit."
                    );
                }

                // CSL metadata comes as key=value pairs
                // We need to look for the "doc" key that contains the full JSON
                MINISET_LOG_DEBUG("Metadata string: ", std::string(metadata[0]).substr(0, 200));

                // Parse metadata - GDAL returns it as "doc={JSON...}"
                const char* json_str = metadata[0];
                if (strncmp(json_str, "doc=", 4) == 0) {
                    json_str += 4;  // Skip the "doc=" prefix
                }

                nlohmann::json full_json;
                try {
                    full_json = nlohmann::json::parse(json_str);
                    MINISET_LOG_DEBUG("Parsed full ISIS3 label from GeoTIFF");
                } catch (const nlohmann::json::parse_error& e) {
                    throw std::runtime_error(std::string("Failed to parse ISIS3 label JSON: ") + e.what());
                }

                // Extract String_CSMState from the full label
                if (full_json.contains("String_CSMState")) {
                    MINISET_LOG_DEBUG("Found String_CSMState at top level");
                    csm_state_json = full_json["String_CSMState"];
                } else if (full_json.contains("IsisCube") && full_json["IsisCube"].contains("String_CSMState")) {
                    MINISET_LOG_DEBUG("Found String_CSMState under IsisCube");
                    csm_state_json = full_json["IsisCube"]["String_CSMState"];
                } else {
                    MINISET_LOG_ERROR("String_CSMState not found in parsed JSON");
                    throw std::runtime_error(
                        "No CSM state found in ISIS3 label (String_CSMState). "
                        "Image may not have been processed with ISIS csminit."
                    );
                }
            } else {
                // GetMetadataItem succeeded - parse String_CSMState directly
                MINISET_LOG_DEBUG("Found String_CSMState via GetMetadataItem, parsing JSON");
                try {
                    csm_state_json = nlohmann::json::parse(csm_state_json_str);
                    MINISET_LOG_DEBUG("Parsed String_CSMState JSON:\n", csm_state_json.dump(2));
                } catch (const nlohmann::json::parse_error& e) {
                    MINISET_LOG_ERROR("JSON parse error: ", e.what());
                    throw std::runtime_error(std::string("Failed to parse String_CSMState JSON: ") + e.what());
                }
            }
        }

        // Extract CSM state data
        // GeoTIFFs always have _data field with hex-encoded state
        // ISIS3 cubes may have _data, or may have StartByte/Bytes pointing to data in file
        std::string csm_state_hex;
        MINISET_LOG_DEBUG("CSM state JSON:\n", csm_state_json.dump(2));

        bool has_data = csm_state_json.contains("_data");
        MINISET_LOG_DEBUG("has_data: ", has_data);
        if (!has_data) {
            bool has_startbyte = csm_state_json.contains("StartByte");
            bool has_bytes = csm_state_json.contains("Bytes");
            MINISET_LOG_DEBUG("has_startbyte: ", has_startbyte, ", has_bytes: ", has_bytes);
        }

        if (has_data) {
            // Data is embedded in JSON as hex string
            csm_state_hex = csm_state_json["_data"].get<std::string>();
            MINISET_LOG_DEBUG("Extracted hex data, length: ", csm_state_hex.length());
        } else if (csm_state_json.contains("StartByte") && csm_state_json.contains("Bytes")) {
            // Data is in the file - need to read it (typical for ISIS3 cubes)
            int start_byte = csm_state_json["StartByte"].get<int>();
            int num_bytes = csm_state_json["Bytes"].get<int>();

            // Close GDAL dataset before opening file for binary read
            GDALClose(dataset);
            dataset = nullptr;

            // Open cube file for binary reading
            std::ifstream cube_file(image_path, std::ios::binary);
            if (!cube_file) {
                throw std::runtime_error("Failed to open cube file for reading CSM state data: " + image_path);
            }

            // Seek to CSM state data location
            cube_file.seekg(start_byte);
            if (!cube_file) {
                throw std::runtime_error("Failed to seek to CSM state data in cube file");
            }

            // Read the raw CSM state bytes
            std::vector<unsigned char> raw_bytes(num_bytes);
            cube_file.read(reinterpret_cast<char*>(raw_bytes.data()), num_bytes);
            if (!cube_file || cube_file.gcount() != num_bytes) {
                throw std::runtime_error("Failed to read CSM state data from cube file");
            }
            cube_file.close();

            // Convert raw bytes to hex string (lowercase to match GeoTIFF format)
            csm_state_hex.reserve(num_bytes * 2);
            const char hex_chars[] = "0123456789abcdef";
            for (unsigned char byte : raw_bytes) {
                csm_state_hex += hex_chars[(byte >> 4) & 0xF];
                csm_state_hex += hex_chars[byte & 0xF];
            }
        } else {
            throw std::runtime_error(
                "String_CSMState metadata found but missing both _data field and StartByte/Bytes fields"
            );
        }

        if (csm_state_hex.empty()) {
            throw std::runtime_error("CSM state data is empty");
        }

        MINISET_LOG_DEBUG("About to decode hex to binary");

        // Decode hex string to binary CSM state
        int num_bytes = 0;
        GByte* decoded_bytes = CPLHexToBinary(csm_state_hex.c_str(), &num_bytes);

        if (!decoded_bytes || num_bytes == 0) {
            MINISET_LOG_ERROR("CPLHexToBinary failed or returned 0 bytes");
            throw std::runtime_error("Failed to decode hex CSM state string");
        }

        MINISET_LOG_DEBUG("Decoded ", num_bytes, " bytes");

        // Convert to std::string and free GDAL memory
        std::string state_string(reinterpret_cast<char*>(decoded_bytes), num_bytes);
        MINISET_LOG_DEBUG("State string created, length: ", state_string.length());

        // Check for null bytes in the state string (debug info)
        size_t null_count = 0;
        for (size_t i = 0; i < state_string.length(); i++) {
            if (state_string[i] == '\0') null_count++;
        }
        MINISET_LOG_DEBUG("State string contains ", null_count, " null bytes");

        // Print first 200 bytes as hex to inspect (debug info)
        std::ostringstream hex_stream;
        hex_stream << std::hex << std::setfill('0');
        for (size_t i = 0; i < std::min(size_t(200), state_string.length()); i++) {
            hex_stream << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(state_string[i]));
        }
        MINISET_LOG_DEBUG("First 200 bytes (hex): ", hex_stream.str());

        CPLFree(decoded_bytes);

        // Close dataset if still open
        if (dataset) {
            GDALClose(dataset);
        }

        MINISET_LOG_DEBUG("About to call createCsmFromStateString");

        // Use existing function to create model from state string
        ::csm::RasterGM* result = createCsmFromStateString(state_string);

        MINISET_LOG_DEBUG("createCsmFromStateString returned: ", reinterpret_cast<uintptr_t>(result));

        return result;

    } catch (...) {
        if (dataset) {
            GDALClose(dataset);
        }
        throw;  // Re-throw exception after cleanup
    }
}

} // namespace csm
