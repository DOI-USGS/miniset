#include "csm/csm_interface.hpp"
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <iostream>
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
    // Native: Load USGSCSM plugin library dynamically if not already loaded
    static void* plugin_handle = nullptr;
    if (!plugin_handle) {
        // Build plugin path from CONDA_PREFIX environment variable
        std::string plugin_path;
        const char* conda_prefix = std::getenv("CONDA_PREFIX");

        if (conda_prefix) {
            // Default: $CONDA_PREFIX/lib/csmplugins/libusgscsm.dylib (or .so)
#ifdef __APPLE__
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.dylib";
#else
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.so";
#endif
            plugin_handle = dlopen(plugin_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        }

        // Fallback paths if CONDA_PREFIX not set or plugin not found there
        if (!plugin_handle) {
            const char* fallback_paths[] = {
                "../lib/csmplugins/libusgscsm.dylib",
                "../lib/csmplugins/libusgscsm.so",
                "./libusgscsm.dylib",
                "./libusgscsm.so",
                "libusgscsm.dylib",
                "libusgscsm.so",
                nullptr
            };

            for (const char** path = fallback_paths; *path != nullptr; ++path) {
                plugin_handle = dlopen(*path, RTLD_LAZY | RTLD_GLOBAL);
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

    // USGSCSM reads the JSON file directly when filename is set
    // The ISD object just needs the filename, and the plugin handles parsing
    ::csm::Isd isd(isd_file_or_json);
#endif

    // Find the USGS CSM plugin
    const ::csm::Plugin* plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
    if (!plugin) {
        throw std::runtime_error("Could not find UsgsAstroPluginCSM plugin");
    }

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

    if (!model) {
        throw std::runtime_error("Failed to construct model from ISD");
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

    // Load USGSCSM plugin if not already loaded
    static void* plugin_handle = nullptr;
    if (!plugin_handle) {
        const char* conda_prefix = std::getenv("CONDA_PREFIX");
        std::string plugin_path;

        if (conda_prefix) {
#ifdef __APPLE__
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.dylib";
#else
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.so";
#endif
            plugin_handle = dlopen(plugin_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        }

        if (!plugin_handle) {
            const char* fallback_paths[] = {
                "../lib/csmplugins/libusgscsm.dylib",
                "../lib/csmplugins/libusgscsm.so",
                "./libusgscsm.dylib",
                "./libusgscsm.so",
                "libusgscsm.dylib",
                "libusgscsm.so",
                nullptr
            };

            for (const char** path = fallback_paths; *path != nullptr; ++path) {
                plugin_handle = dlopen(*path, RTLD_LAZY | RTLD_GLOBAL);
                if (plugin_handle) break;
            }
        }

        if (!plugin_handle) {
            throw std::runtime_error("Failed to load USGSCSM plugin library: " + std::string(dlerror()));
        }
    }

    ::csm::Isd isd(temp_file);
#endif

    // Find the USGS CSM plugin
    const ::csm::Plugin* plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
    if (!plugin) {
        throw std::runtime_error("Could not find UsgsAstroPluginCSM plugin");
    }

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
        delete model;
        throw std::runtime_error("Model is not a RasterGM");
    }

    return raster_model;
}

::csm::RasterGM* createCsmFromStateString(const std::string& state_string) {
#ifdef __EMSCRIPTEN__
    // WASM: Plugin is statically linked, already registered
    const ::csm::Plugin* plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
#else
    // Native: Ensure plugin is loaded
    static void* plugin_handle = nullptr;
    if (!plugin_handle) {
        const char* conda_prefix = std::getenv("CONDA_PREFIX");
        std::string plugin_path;

        if (conda_prefix) {
#ifdef __APPLE__
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.dylib";
#else
            plugin_path = std::string(conda_prefix) + "/lib/csmplugins/libusgscsm.so";
#endif
            plugin_handle = dlopen(plugin_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        }

        if (!plugin_handle) {
            throw std::runtime_error("Failed to load USGSCSM plugin library");
        }
    }

    const ::csm::Plugin* plugin = ::csm::Plugin::findPlugin("UsgsAstroPluginCSM");
#endif

    if (!plugin) {
        throw std::runtime_error("Could not find UsgsAstroPluginCSM plugin");
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[C++] Plugin found, ptr: ' + $0); }, reinterpret_cast<uintptr_t>(plugin));
#endif

    // Construct model from state string
    ::csm::WarningList warnings;
    ::csm::Model* model = nullptr;

#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[C++] About to call constructModelFromState, state length: ' + $0); }, state_string.length());
    EM_ASM({ console.log('[C++] First 100 chars of state: ' + UTF8ToString($0)); }, state_string.substr(0, 100).c_str());
    EM_ASM({ console.log('[C++] Warnings ptr: ' + $0); }, reinterpret_cast<uintptr_t>(&warnings));
    EM_ASM({ console.log('[C++] === IMMEDIATELY BEFORE constructModelFromState CALL ==='); });
#endif

    try {
        model = plugin->constructModelFromState(state_string, &warnings);
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] === IMMEDIATELY AFTER constructModelFromState CALL ==='); });
#endif
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] constructModelFromState succeeded, model ptr: ' + $0); }, reinterpret_cast<uintptr_t>(model));
#endif
    } catch (const ::csm::Error& e) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.error('[C++] CSM Error caught: ' + UTF8ToString($0)); }, e.getMessage().c_str());
#endif
        throw std::runtime_error(std::string("Failed to construct model from state: ") + e.getMessage());
    } catch (const std::exception& e) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.error('[C++] std::exception caught: ' + UTF8ToString($0)); }, e.what());
#endif
        throw;
    } catch (...) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.error('[C++] Unknown exception caught in constructModelFromState'); });
#endif
        throw std::runtime_error("Unknown error in constructModelFromState");
    }

    if (!model) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.error('[C++] constructModelFromState returned null'); });
#endif
        throw std::runtime_error("Failed to construct model from state string");
    }

    // Cast to RasterGM
    ::csm::RasterGM* raster_model = dynamic_cast<::csm::RasterGM*>(model);
    if (!raster_model) {
        delete model;
        throw std::runtime_error("Model is not a RasterGM");
    }

    return raster_model;
}

::csm::RasterGM* createCsmFromAttachedSpice(const std::string& image_path) {
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[C++] createCsmFromAttachedSpice: ' + UTF8ToString($0)); }, image_path.c_str());

    // Check file size in VFS
    struct stat st;
    if (stat(image_path.c_str(), &st) == 0) {
        EM_ASM({ console.log('[C++] File size in VFS: ' + $0 + ' bytes'); }, st.st_size);
    } else {
        EM_ASM({ console.error('[C++] File not found in VFS via stat()'); });
    }
#endif

    GDALDataset* dataset = (GDALDataset*)GDALOpen(image_path.c_str(), GA_ReadOnly);
    if (!dataset) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.error('[C++] GDALOpen failed for: ' + UTF8ToString($0)); }, image_path.c_str());
#endif
        throw std::runtime_error("Failed to open image: " + image_path);
    }

#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[C++] GDALOpen succeeded, dataset size: ' + $0 + 'x' + $1); },
            dataset->GetRasterXSize(), dataset->GetRasterYSize());
#endif

    try {
        nlohmann::json csm_state_json;

        // Check driver type to determine metadata structure
        const char* driver_name = dataset->GetDriver()->GetDescription();
        bool is_isis3_cube = (strcmp(driver_name, "ISIS3") == 0);

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] Driver: ' + UTF8ToString($0) + ', is_isis3: ' + $1); }, driver_name, is_isis3_cube);
#endif

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
#ifdef __EMSCRIPTEN__
            EM_ASM({ console.log('[C++] Reading metadata from GeoTIFF'); });

            // Debug: List all metadata domains
            char** domains = dataset->GetMetadataDomainList();
            if (domains) {
                EM_ASM({ console.log('[C++] Available metadata domains:'); });
                for (int i = 0; domains[i] != nullptr; i++) {
                    EM_ASM({ console.log('  - ' + UTF8ToString($0)); }, domains[i]);
                }
                CSLDestroy(domains);
            } else {
                EM_ASM({ console.log('[C++] No metadata domains found'); });
            }

            // Debug: Try to get metadata from json:ISIS3 domain
            CSLConstList isis3_metadata = dataset->GetMetadata("json:ISIS3");
            if (isis3_metadata) {
                EM_ASM({ console.log('[C++] json:ISIS3 metadata exists, listing keys:'); });
                for (int i = 0; isis3_metadata[i] != nullptr; i++) {
                    // Just log first 100 chars of each key
                    char preview[101];
                    strncpy(preview, isis3_metadata[i], 100);
                    preview[100] = '\0';
                    EM_ASM({ console.log('  ' + UTF8ToString($0) + '...'); }, preview);
                }
            } else {
                EM_ASM({ console.log('[C++] json:ISIS3 metadata domain is empty/null'); });
            }
#endif
            const char* csm_state_json_str = dataset->GetMetadataItem("String_CSMState", "json:ISIS3");

            if (!csm_state_json_str) {
                // GetMetadataItem returned null - GDAL might be returning entire label as one blob
                // Try reading the full json:ISIS3 metadata and extracting String_CSMState
#ifdef __EMSCRIPTEN__
                EM_ASM({ console.log('[C++] GetMetadataItem returned null, trying full metadata'); });
#endif
                CSLConstList metadata = dataset->GetMetadata("json:ISIS3");
                if (!metadata || !metadata[0]) {
                    throw std::runtime_error(
                        "No json:ISIS3 metadata found in image. "
                        "Image may not have been processed with ISIS csminit."
                    );
                }

                // CSL metadata comes as key=value pairs
                // We need to look for the "doc" key that contains the full JSON
#ifdef __EMSCRIPTEN__
                EM_ASM({ console.log('[C++] Metadata string: ' + UTF8ToString($0)); }, metadata[0]);
#endif

                // Parse metadata - GDAL returns it as "doc={JSON...}"
                const char* json_str = metadata[0];
                if (strncmp(json_str, "doc=", 4) == 0) {
                    json_str += 4;  // Skip the "doc=" prefix
                }

                nlohmann::json full_json;
                try {
                    full_json = nlohmann::json::parse(json_str);
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[C++] Parsed full ISIS3 label from GeoTIFF'); });

                    // Debug: show top-level keys
                    std::string keys_str = "Top-level keys: ";
                    for (auto it = full_json.begin(); it != full_json.end(); ++it) {
                        keys_str += it.key() + ", ";
                    }
                    EM_ASM({ console.log('[C++] ' + UTF8ToString($0)); }, keys_str.c_str());

                    // Debug: check for IsisCube and its keys
                    if (full_json.contains("IsisCube")) {
                        std::string isis_keys = "IsisCube keys: ";
                        for (auto it = full_json["IsisCube"].begin(); it != full_json["IsisCube"].end(); ++it) {
                            isis_keys += it.key() + ", ";
                        }
                        EM_ASM({ console.log('[C++] ' + UTF8ToString($0)); }, isis_keys.c_str());
                    }
#endif
                } catch (const nlohmann::json::parse_error& e) {
                    throw std::runtime_error(std::string("Failed to parse ISIS3 label JSON: ") + e.what());
                }

                // Extract String_CSMState from the full label
                if (full_json.contains("String_CSMState")) {
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[C++] Found String_CSMState at top level'); });
#endif
                    csm_state_json = full_json["String_CSMState"];
                } else if (full_json.contains("IsisCube") && full_json["IsisCube"].contains("String_CSMState")) {
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[C++] Found String_CSMState under IsisCube'); });
#endif
                    csm_state_json = full_json["IsisCube"]["String_CSMState"];
                } else {
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.error('[C++] String_CSMState not found in parsed JSON'); });
#endif
                    throw std::runtime_error(
                        "No CSM state found in ISIS3 label (String_CSMState). "
                        "Image may not have been processed with ISIS csminit."
                    );
                }
            } else {
                // GetMetadataItem succeeded - parse String_CSMState directly
#ifdef __EMSCRIPTEN__
                EM_ASM({ console.log('[C++] Found String_CSMState via GetMetadataItem, parsing JSON'); });
#endif
                try {
                    csm_state_json = nlohmann::json::parse(csm_state_json_str);
                    std::cout << "Parsed String_CSMState JSON: " << std::endl << csm_state_json.dump(2) << std::endl;  // Debug print
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[C++] JSON parsed successfully'); });
#endif
                } catch (const nlohmann::json::parse_error& e) {
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.error('[C++] JSON parse error: ' + UTF8ToString($0)); }, e.what());
#endif
                    throw std::runtime_error(std::string("Failed to parse String_CSMState JSON: ") + e.what());
                }
            }
        }

        // Extract CSM state data
        // GeoTIFFs always have _data field with hex-encoded state
        // ISIS3 cubes may have _data, or may have StartByte/Bytes pointing to data in file
        std::string csm_state_hex;
        std::cout << "CSM state JSON: " << std::endl << csm_state_json.dump(2) << std::endl;  // Debug print of the JSON content

        bool has_data = csm_state_json.contains("_data");
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] has_data: ' + $0); }, has_data);
        if (!has_data) {
            bool has_startbyte = csm_state_json.contains("StartByte");
            bool has_bytes = csm_state_json.contains("Bytes");
            EM_ASM({ console.log('[C++] has_startbyte: ' + $0 + ', has_bytes: ' + $1); }, has_startbyte, has_bytes);
        }
#endif

        if (has_data) {
            // Data is embedded in JSON as hex string
            csm_state_hex = csm_state_json["_data"].get<std::string>();
#ifdef __EMSCRIPTEN__
            EM_ASM({ console.log('[C++] Extracted hex data, length: ' + $0); }, csm_state_hex.length());
#endif
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

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] About to decode hex to binary'); });
#endif

        // Decode hex string to binary CSM state
        int num_bytes = 0;
        GByte* decoded_bytes = CPLHexToBinary(csm_state_hex.c_str(), &num_bytes);

        if (!decoded_bytes || num_bytes == 0) {
#ifdef __EMSCRIPTEN__
            EM_ASM({ console.error('[C++] CPLHexToBinary failed or returned 0 bytes'); });
#endif
            throw std::runtime_error("Failed to decode hex CSM state string");
        }

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] Decoded ' + $0 + ' bytes'); }, num_bytes);
#endif

        // Convert to std::string and free GDAL memory
        std::string state_string(reinterpret_cast<char*>(decoded_bytes), num_bytes);

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] State string created, length: ' + $0); }, state_string.length());

        // Check for null bytes in the state string
        size_t null_count = 0;
        for (size_t i = 0; i < state_string.length(); i++) {
            if (state_string[i] == '\0') null_count++;
        }
        EM_ASM({ console.log('[C++] State string contains ' + $0 + ' null bytes'); }, null_count);

        // Print first 200 bytes as hex to inspect
        std::string hex_preview;
        for (size_t i = 0; i < std::min(size_t(200), state_string.length()); i++) {
            char buf[3];
            sprintf(buf, "%02x", (unsigned char)state_string[i]);
            hex_preview += buf;
        }
        EM_ASM({ console.log('[C++] First 200 bytes (hex): ' + UTF8ToString($0)); }, hex_preview.c_str());
#endif

        CPLFree(decoded_bytes);

        // Close dataset if still open
        if (dataset) {
            GDALClose(dataset);
        }

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] About to call createCsmFromStateString'); });
#endif

        // Use existing function to create model from state string
        ::csm::RasterGM* result = createCsmFromStateString(state_string);

#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[C++] createCsmFromStateString returned: ' + $0); }, reinterpret_cast<uintptr_t>(result));
#endif

        return result;

    } catch (...) {
        if (dataset) {
            GDALClose(dataset);
        }
        throw;  // Re-throw exception after cleanup
    }
}

} // namespace csm
