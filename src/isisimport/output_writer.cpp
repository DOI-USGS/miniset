#include "isisimport/output_writer.hpp"

#include <string>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>

namespace isisimport {

namespace {

/// Map an OutputFormat to a GDAL driver short name.
const char* driver_name(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::GTiff:
            return "GTiff";
        case OutputFormat::Cog:
            return "COG";
        case OutputFormat::Cube:
        default:
            return "ISIS3";
    }
}

/// Build a GDAL creation-option string list from KEY=VALUE pairs.
char** build_options(const std::vector<std::pair<std::string, std::string>>& co) {
    char** opts = nullptr;
    for (const auto& kv : co) {
        opts = CSLSetNameValue(opts, kv.first.c_str(), kv.second.c_str());
    }
    return opts;
}

/// Map a GDAL data type to the ISIS Core Pixels Type name.
const char* isis_pixel_type(GDALDataType dt) {
    switch (dt) {
        case GDT_Byte:
            return "UnsignedByte";
        case GDT_Int16:
            return "SignedWord";
        case GDT_UInt16:
            return "UnsignedWord";
        case GDT_Int32:
            return "SignedInteger";
        case GDT_UInt32:
            return "UnsignedInteger";
        case GDT_Float32:
            return "Real";
        case GDT_Float64:
            return "Double";
        default:
            return "Real";
    }
}

/// Build the IsisCube.Core object from scratch, describing the output raster the
/// way ISIS's cube reader expects: integer Dimensions, a Pixels group with the
/// data type / byte order / base / multiplier. Core is inserted first so it
/// leads the IsisCube object (ordering is not semantically required).
///
/// We build this ourselves (rather than trusting the driver-generated Core) so
/// the label is identical across ISIS3/GTiff/COG and always well-formed: valid
/// integer dimensions and a real Format, never the driver's placeholder
/// StartByte token or string-typed values that ISIS's ALE server rejects.
void build_core(nlohmann::ordered_json& isis_label, GDALDatasetH ds) {
    using ojson = nlohmann::ordered_json;
    const int bands = GDALGetRasterCount(ds);

    ojson dims = ojson::object();
    dims["_type"] = "group";
    dims["Samples"] = GDALGetRasterXSize(ds);
    dims["Lines"] = GDALGetRasterYSize(ds);
    dims["Bands"] = bands;

    ojson pixels = ojson::object();
    pixels["_type"] = "group";
    GDALDataType dt = bands > 0 ? GDALGetRasterDataType(GDALGetRasterBand(ds, 1))
                                : GDT_Float32;
    pixels["Type"] = isis_pixel_type(dt);
    pixels["ByteOrder"] = "Lsb";
    double base = 0.0, mult = 1.0;
    if (bands > 0) {
        GDALRasterBandH b = GDALGetRasterBand(ds, 1);
        int has_off = 0, has_scale = 0;
        double off = GDALGetRasterOffset(b, &has_off);
        double scale = GDALGetRasterScale(b, &has_scale);
        if (has_off) base = off;
        if (has_scale) mult = scale;
    }
    pixels["Base"] = base;
    pixels["Multiplier"] = mult;

    ojson core = ojson::object();
    core["_type"] = "object";
    core["StartByte"] = 1;
    core["Format"] = "BandSequential";
    core["Dimensions"] = dims;
    core["Pixels"] = pixels;

    isis_label["IsisCube"]["Core"] = core;
}

}  // namespace

bool write_output(const std::string& from, const std::string& to, OutputFormat format,
                  const nlohmann::ordered_json& isis_label,
                  const std::vector<std::pair<std::string, std::string>>& creation_options,
                  const TransformContext& ctx, std::string& err) {
    GDALAllRegister();

    const char* drv_name = driver_name(format);
    if (GDALGetDriverByName(drv_name) == nullptr) {
        err = std::string("GDAL driver not available: ") + drv_name;
        return false;
    }

    GDALDatasetH src = GDALOpen(from.c_str(), GA_ReadOnly);
    if (src == nullptr) {
        err = "Failed to open input: " + from;
        return false;
    }

    // Wrap the source in an in-memory VRT: optionally crop (MRO CTX prefix/
    // suffix), then STRIP georeferencing. `isisimport` treats the input as raw
    // pixels — ISIS assigns geometry later (spiceinit/camera model) — so any SRS
    // on the source (e.g. a map-projected Kaguya product) must not become an ISIS
    // `Mapping` group, which GDAL would otherwise synthesize from the geotransform.
    GDALDatasetH working = src;
    GDALDatasetH cropped = nullptr;
    {
        char** cargs = nullptr;
        cargs = CSLAddString(cargs, "-of");
        cargs = CSLAddString(cargs, "VRT");
        if (ctx.has_window) {
            cargs = CSLAddString(cargs, "-srcwin");
            cargs = CSLAddString(cargs, std::to_string(ctx.win_xoff).c_str());
            cargs = CSLAddString(cargs, std::to_string(ctx.win_yoff).c_str());
            cargs = CSLAddString(cargs, std::to_string(ctx.win_xsize).c_str());
            cargs = CSLAddString(cargs, std::to_string(ctx.win_ysize).c_str());
        }
        GDALTranslateOptions* copts = GDALTranslateOptionsNew(cargs, nullptr);
        CSLDestroy(cargs);
        cropped = GDALTranslate("", src, copts, nullptr);
        GDALTranslateOptionsFree(copts);
        if (cropped == nullptr) {
            err = "Failed to prepare source raster";
            GDALClose(src);
            return false;
        }
        // Clear projection + geotransform so no Mapping group is generated.
        GDALDataset* mem = GDALDataset::FromHandle(cropped);
        if (mem) {
            mem->SetSpatialRef(nullptr);
            // Reset to the identity geotransform (no georeferencing).
            double identity[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
            mem->SetGeoTransform(identity);
        }
        working = cropped;
    }

    char** opts = build_options(creation_options);
    bool ok = false;

    if (format == OutputFormat::Cube) {
        // ISIS3 cube: the driver only merges custom label groups when they are
        // present on the SOURCE before CreateCopy, and it builds Core itself.
        // (GDAL's ISIS3 writer keeps the standard mission groups but does drop
        // some free-form Archive keywords — a documented driver limitation.)
        std::string label_str = isis_label.dump(2);
        char* md[2] = {const_cast<char*>(label_str.c_str()), nullptr};
        GDALSetMetadata(working, md, "json:ISIS3");

        GDALDriverH drv = GDALGetDriverByName("ISIS3");
        GDALDatasetH out = GDALCreateCopy(drv, to.c_str(), working, /*bStrict=*/FALSE,
                                          opts, nullptr, nullptr);
        ok = out != nullptr;
        if (out) GDALClose(out);
    } else {
        // GTiff/COG: GDAL stores the json:ISIS3 domain verbatim, so we convert
        // pixels first, then author the complete label (mission groups from the
        // spec + a Core we synthesize) and overwrite the domain. This keeps the
        // full ISIS-matching label, unlike the cube driver.
        char** targs = nullptr;
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, drv_name);
        for (const auto& kv : creation_options) {
            targs = CSLAddString(targs, "-co");
            targs = CSLAddString(targs, (kv.first + "=" + kv.second).c_str());
        }
        GDALTranslateOptions* topts = GDALTranslateOptionsNew(targs, nullptr);
        CSLDestroy(targs);
        GDALDatasetH out = topts ? GDALTranslate(to.c_str(), working, topts, nullptr) : nullptr;
        if (topts) GDALTranslateOptionsFree(topts);
        if (out != nullptr) {
            nlohmann::ordered_json label = isis_label;
            build_core(label, out);
            std::string label_str = label.dump(2);
            char* md[2] = {const_cast<char*>(label_str.c_str()), nullptr};
            GDALSetMetadata(out, md, "json:ISIS3");
            GDALClose(out);
            ok = true;
        }
    }

    CSLDestroy(opts);
    if (cropped) GDALClose(cropped);
    GDALClose(src);

    if (!ok) {
        const char* last = CPLGetLastErrorMsg();
        err = std::string("Write to ") + drv_name + " failed: " +
              (last ? last : "unknown error");
    }
    return ok;
}

}  // namespace isisimport
