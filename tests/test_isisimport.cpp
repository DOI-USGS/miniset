// isisimport tests. All fixtures are synthesized in memory (/vsimem) by writing
// hand-authored PDS3/PDS4 instrument labels + tiny pixel blobs with GDAL's
// VSIFileFromMemBuffer — the same pattern GDAL's own driver tests use. No image
// files are stored in the repo, and nothing touches disk.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "isisimport/dispatcher.hpp"
#include "isisimport/import_image.hpp"
#include "isisimport/input_label.hpp"
#include "isisimport/spec_paths.hpp"
#include "isisimport/translation_spec.hpp"
#include "isisimport/transform_registry.hpp"

using json = nlohmann::json;

namespace {

std::string spec_dir() {
    if (const char* env = std::getenv("MINISET_TEST_SPEC_DIR")) return env;
    return "";  // fall through to the build-tree MINISET_APPDATA_BUILD define
}

// Write a byte string into a /vsimem path (GDAL takes ownership of the buffer).
void vsimem_put(const std::string& path, const std::string& bytes) {
    GByte* buf = static_cast<GByte*>(CPLMalloc(bytes.size()));
    std::memcpy(buf, bytes.data(), bytes.size());
    VSIFCloseL(VSIFileFromMemBuffer(path.c_str(), buf, bytes.size(), TRUE));
}

void vsimem_rm(const std::string& path) { VSIUnlink(path.c_str()); }

// A minimal MRO CTX-style PDS3 label + detached pixel blob in /vsimem.
// Returns the label path to import from.
std::string make_ctx_pds3(int lines, int samples) {
    std::string lbl =
        "PDS_VERSION_ID = \"PDS3\"\n"
        "^IMAGE = \"ctx.img\"\n"
        "SPACECRAFT_NAME = MARS_RECONNAISSANCE_ORBITER\n"
        "INSTRUMENT_ID = CTX\n"
        "TARGET_NAME = MARS\n"
        "MISSION_PHASE_NAME = ESP\n"
        "START_TIME = 2025-09-01T01:54:27.497\n"
        "SPACECRAFT_CLOCK_START_COUNT = \"0866066657:172\"\n"
        "LINE_EXPOSURE_DURATION = 1.877 <MSEC>\n"
        "SPATIAL_SUMMING = 1\n"
        "EDIT_MODE_ID = \"1\"\n"
        "SAMPLE_FIRST_PIXEL = 0\n"
        "OBJECT = IMAGE\n"
        "  LINES = " + std::to_string(lines) + "\n"
        "  LINE_SAMPLES = " + std::to_string(samples) + "\n"
        "  SAMPLE_BITS = 8\n"
        "  SAMPLE_TYPE = UNSIGNED_INTEGER\n"
        "END_OBJECT = IMAGE\n"
        "END\n";
    vsimem_put("/vsimem/ctx.lbl", lbl);
    vsimem_put("/vsimem/ctx.img", std::string(static_cast<size_t>(lines) * samples, '\x2a'));
    return "/vsimem/ctx.lbl";
}

// A minimal Kaguya TC-style PDS3 label + pixel blob in /vsimem.
std::string make_kaguya_pds3(int lines, int samples) {
    std::string lbl =
        "PDS_VERSION_ID = \"PDS3\"\n"
        "^IMAGE = \"tc.img\"\n"
        "MISSION_NAME = SELENE\n"
        "SPACECRAFT_NAME = \"SELENE-M\"\n"
        "INSTRUMENT_NAME = \"Terrain Camera\"\n"
        "INSTRUMENT_ID = TC\n"
        "TARGET_NAME = MOON\n"
        "OBSERVATION_MODE_ID = \"NORMAL\"\n"
        "PRODUCT_ID = TC_TEST\n"
        "DATA_SET_ID = TC_MAP\n"
        "OBJECT = IMAGE\n"
        "  LINES = " + std::to_string(lines) + "\n"
        "  LINE_SAMPLES = " + std::to_string(samples) + "\n"
        "  SAMPLE_BITS = 16\n"
        "  SAMPLE_TYPE = MSB_INTEGER\n"
        "  SCALING_FACTOR = 2.0e-05\n"
        "END_OBJECT = IMAGE\n"
        "END\n";
    vsimem_put("/vsimem/tc.lbl", lbl);
    vsimem_put("/vsimem/tc.img",
               std::string(static_cast<size_t>(lines) * samples * 2, '\x01'));
    return "/vsimem/tc.lbl";
}

// A minimal Chandrayaan-2 TMC2 PDS4 label + detached raster in /vsimem.
std::string make_ch2_pds4(int lines, int samples) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Product_Observational xmlns=\"http://pds.nasa.gov/pds4/pds/v1\">\n"
        "  <Identification_Area>\n"
        "    <product_class>Product_Observational</product_class>\n"
        "  </Identification_Area>\n"
        "  <Observation_Area>\n"
        "    <Time_Coordinates>\n"
        "      <start_date_time>2023-02-15T00:32:19.2651Z</start_date_time>\n"
        "      <stop_date_time>2023-02-15T00:40:02.2215Z</stop_date_time>\n"
        "    </Time_Coordinates>\n"
        "    <Investigation_Area>\n"
        "      <name>Chandrayaan-2</name>\n"
        "    </Investigation_Area>\n"
        "    <Observing_System>\n"
        "      <Observing_System_Component>\n"
        "        <name>Chandrayaan 2 Orbiter</name>\n"
        "      </Observing_System_Component>\n"
        "      <Observing_System_Component>\n"
        "        <name>terrain mapping camera</name>\n"
        "      </Observing_System_Component>\n"
        "    </Observing_System>\n"
        "    <Target_Identification>\n"
        "      <name>Moon</name>\n"
        "    </Target_Identification>\n"
        "    <Mission_Area>\n"
        "      <isda:isda_Product_Parameters xmlns:isda=\"https://isda.issdc.gov.in/pds4/isda/v1\">\n"
        "        <isda:isda_job_id>TEST_JOB</isda:isda_job_id>\n"
        "        <isda:isda_imaging_orbit_number>1548</isda:isda_imaging_orbit_number>\n"
        "        <isda:isda_line_exposure_duration unit=\"ms\">3.236</isda:isda_line_exposure_duration>\n"
        "        <isda:isda_focal_length unit=\"mm\">140</isda:isda_focal_length>\n"
        "      </isda:isda_Product_Parameters>\n"
        "    </Mission_Area>\n"
        "  </Observation_Area>\n"
        "  <File_Area_Observational>\n"
        "    <File>\n"
        "      <file_name>ch2_tmc_nrf_20230215T0032192651_d_img_n18.img</file_name>\n"
        "    </File>\n"
        "    <Array_2D_Image>\n"
        "      <offset unit=\"byte\">0</offset>\n"
        "      <axes>2</axes>\n"
        "      <axis_index_order>Last Index Fastest</axis_index_order>\n"
        "      <Element_Array>\n"
        "        <data_type>UnsignedMSB2</data_type>\n"
        "      </Element_Array>\n"
        "      <Axis_Array>\n"
        "        <axis_name>Line</axis_name>\n"
        "        <elements>" + std::to_string(lines) + "</elements>\n"
        "        <sequence_number>1</sequence_number>\n"
        "      </Axis_Array>\n"
        "      <Axis_Array>\n"
        "        <axis_name>Sample</axis_name>\n"
        "        <elements>" + std::to_string(samples) + "</elements>\n"
        "        <sequence_number>2</sequence_number>\n"
        "      </Axis_Array>\n"
        "    </Array_2D_Image>\n"
        "  </File_Area_Observational>\n"
        "</Product_Observational>\n";
    vsimem_put("/vsimem/ch2_tmc_nrf_20230215T0032192651_d_img_n18.xml", xml);
    vsimem_put("/vsimem/ch2_tmc_nrf_20230215T0032192651_d_img_n18.img",
               std::string(static_cast<size_t>(lines) * samples * 2, '\x00'));
    return "/vsimem/ch2_tmc_nrf_20230215T0032192651_d_img_n18.xml";
}

// Extract a group's keyword from an imported label.
const nlohmann::ordered_json& group(const isisimport::ImportResult& r, const char* g) {
    return r.isis_label["IsisCube"][g];
}

}  // namespace

// ---------------------------------------------------------------------------
// Unit-level tests (no I/O)
// ---------------------------------------------------------------------------

TEST(IsisImportInputLabel, ResolvePathAndValueUnwrap) {
    json label = {
        {"INSTRUMENT_ID", "TC"},
        {"IMAGE", {{"LINE_SAMPLES", 3}}},
        {"LINE_EXPOSURE_DURATION", {{"value", 1.877}, {"unit", "MSEC"}}},
        {"ELEM", {{"_text", "140"}, {"attrib_unit", "mm"}}},
    };
    EXPECT_EQ(isisimport::resolve_input_path(label, "INSTRUMENT_ID"), "TC");
    EXPECT_EQ(isisimport::resolve_input_path(label, "IMAGE.LINE_SAMPLES"), 3);
    EXPECT_EQ(isisimport::resolve_input_path(label, "LINE_EXPOSURE_DURATION"), 1.877);
    EXPECT_EQ(isisimport::resolve_input_path(label, "ELEM"), "140");  // _text unwrap
    EXPECT_TRUE(isisimport::resolve_input_path(label, "NOPE.MISSING").is_null());
}

TEST(IsisImportTranslation, ApplySpecCoreSemantics) {
    isisimport::TranslationSpec spec;
    isisimport::GroupSpec g;
    g.name = "Instrument";

    isisimport::KeywordSpec kConst;
    kConst.out = "SpacecraftName";
    kConst.has_const = true;
    kConst.const_value = "KAGUYA";
    g.keywords.push_back(kConst);

    isisimport::KeywordSpec kMap;
    kMap.out = "InstrumentId";
    kMap.from = {"INSTRUMENT_ID"};
    g.keywords.push_back(kMap);

    isisimport::KeywordSpec kEnum;
    kEnum.out = "TargetName";
    kEnum.from = {"TARGET_NAME"};
    kEnum.has_enum = true;
    kEnum.enum_map = {{"MOON", "Moon"}, {"*", "Sky"}};
    g.keywords.push_back(kEnum);

    isisimport::KeywordSpec kUnit;
    kUnit.out = "Center";
    kUnit.has_const = true;
    kUnit.const_value = 640;
    kUnit.unit = "nm";
    g.keywords.push_back(kUnit);

    isisimport::KeywordSpec kMissing;
    kMissing.out = "Absent";
    kMissing.from = {"DOES_NOT_EXIST"};
    g.keywords.push_back(kMissing);

    spec.groups.push_back(g);

    json input = {{"INSTRUMENT_ID", "TC"}, {"TARGET_NAME", "MOON"}};
    isisimport::TransformContext ctx;
    auto out = isisimport::apply_spec(spec, input, isisimport::TransformRegistry::builtin(), ctx);

    const auto& inst = out["IsisCube"]["Instrument"];
    EXPECT_EQ(inst["SpacecraftName"], "KAGUYA");
    EXPECT_EQ(inst["InstrumentId"], "TC");
    EXPECT_EQ(inst["TargetName"], "Moon");
    EXPECT_EQ(inst["Center"]["value"], 640);
    EXPECT_EQ(inst["Center"]["unit"], "nm");
    EXPECT_FALSE(inst.contains("Absent"));
}

TEST(IsisImportTranslation, GroupOrderPreserved) {
    // Groups must emit in spec order (matching ISIS), not alphabetical.
    isisimport::TranslationSpec spec;
    for (const char* name : {"Instrument", "Archive", "BandBin", "Kernels"}) {
        isisimport::GroupSpec g;
        g.name = name;
        isisimport::KeywordSpec k;
        k.out = "K";
        k.has_const = true;
        k.const_value = 1;
        g.keywords.push_back(k);
        spec.groups.push_back(g);
    }
    json input = json::object();
    isisimport::TransformContext ctx;
    auto out = isisimport::apply_spec(spec, input, isisimport::TransformRegistry::builtin(), ctx);
    std::vector<std::string> order;
    for (auto it = out["IsisCube"].begin(); it != out["IsisCube"].end(); ++it) {
        if (it.key() != "_type") order.push_back(it.key());
    }
    EXPECT_EQ(order, (std::vector<std::string>{"Instrument", "Archive", "BandBin", "Kernels"}));
}

TEST(IsisImportDispatch, InfersInstruments) {
    std::string dir = isisimport::resolve_spec_dir(spec_dir());
    if (dir.empty()) GTEST_SKIP() << "spec dir not resolvable";

    auto pds3 = [&](json label) {
        return isisimport::dispatch(label, isisimport::ProductType::PDS3, dir);
    };
    EXPECT_EQ(pds3({{"SPACECRAFT_NAME", "SELENE-M"}, {"INSTRUMENT_ID", "TC"}}).spec_name, "KaguyaTC");
    EXPECT_EQ(pds3({{"SPACECRAFT_NAME", "MARS_RECONNAISSANCE_ORBITER"}, {"INSTRUMENT_ID", "CTX"}}).spec_name, "MroCTX");
    EXPECT_EQ(pds3({{"MISSION_NAME", "MARS EXPLORATION ROVER"}, {"INSTRUMENT_ID", "MI"}}).spec_name, "MerMI");
    EXPECT_EQ(pds3({{"SPACECRAFT_NAME", "DAWN"}, {"INSTRUMENT_ID", "FC2"}}).spec_name, "DawnFC");
}

// ---------------------------------------------------------------------------
// End-to-end tests over in-memory (/vsimem) fixtures
// ---------------------------------------------------------------------------

TEST(IsisImportEndToEnd, CtxPds3ToVsimemCube) {
    GDALAllRegister();
    std::string from = make_ctx_pds3(/*lines=*/4, /*samples=*/16);
    isisimport::ImportOptions opts;
    opts.from = from;
    opts.to = "/vsimem/ctx_out.cub";
    opts.format = isisimport::OutputFormat::Cube;
    opts.spec_dir = spec_dir();

    isisimport::ImportResult res = isisimport::import_image(opts);
    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_EQ(res.instrument_spec, "MroCTX");
    EXPECT_EQ(group(res, "Instrument")["SpacecraftName"], "Mars_Reconnaissance_Orbiter");
    EXPECT_EQ(group(res, "Instrument")["InstrumentId"], "CTX");
    EXPECT_EQ(group(res, "Kernels")["NaifFrameCode"], -74021);

    // Output cube is a real in-memory dataset readable by GDAL.
    GDALDatasetH ds = GDALOpen("/vsimem/ctx_out.cub", GA_ReadOnly);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    vsimem_rm("/vsimem/ctx.lbl");
    vsimem_rm("/vsimem/ctx.img");
    vsimem_rm("/vsimem/ctx_out.cub");
}

TEST(IsisImportEndToEnd, KaguyaPds3ToVsimemGeoTiff) {
    GDALAllRegister();
    std::string from = make_kaguya_pds3(/*lines=*/2, /*samples=*/3);
    isisimport::ImportOptions opts;
    opts.from = from;
    opts.to = "/vsimem/tc_out.tif";  // format inferred from extension
    opts.spec_dir = spec_dir();

    isisimport::ImportResult res = isisimport::import_image(opts);
    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_EQ(res.instrument_spec, "KaguyaTC");
    EXPECT_EQ(group(res, "Instrument")["SpacecraftName"], "KAGUYA");
    EXPECT_EQ(group(res, "BandBin")["Center"], "640nm");

    GDALDatasetH ds = GDALOpen("/vsimem/tc_out.tif", GA_ReadOnly);
    ASSERT_NE(ds, nullptr);
    EXPECT_STREQ(GDALGetDriverShortName(GDALGetDatasetDriver(ds)), "GTiff");
    GDALClose(ds);

    vsimem_rm("/vsimem/tc.lbl");
    vsimem_rm("/vsimem/tc.img");
    vsimem_rm("/vsimem/tc_out.tif");
}

TEST(IsisImportEndToEnd, Chandrayaan2Pds4ToVsimemCube) {
    GDALAllRegister();
    std::string from = make_ch2_pds4(/*lines=*/8, /*samples=*/8);
    isisimport::ImportOptions opts;
    opts.from = from;
    opts.to = "/vsimem/ch2_out.cub";
    opts.format = isisimport::OutputFormat::Cube;
    opts.spec_dir = spec_dir();

    isisimport::ImportResult res = isisimport::import_image(opts);
    ASSERT_TRUE(res.ok) << res.message;
    EXPECT_EQ(res.instrument_spec, "Chandrayaan2TMC2");
    EXPECT_EQ(group(res, "Instrument")["SpacecraftName"], "Chandrayaan-2");
    EXPECT_EQ(group(res, "Instrument")["InstrumentId"], "CH2_TMC_FORE");  // file name char 10 = 'f'
    EXPECT_EQ(group(res, "Instrument")["TargetName"], "Moon");
    EXPECT_EQ(group(res, "Instrument")["StartTime"], "2023-02-15T00:32:19.2651");  // Z stripped
    EXPECT_EQ(group(res, "Kernels")["NaifFrameCode"], -152211);

    vsimem_rm("/vsimem/ch2_tmc_nrf_20230215T0032192651_d_img_n18.xml");
    vsimem_rm("/vsimem/ch2_tmc_nrf_20230215T0032192651_d_img_n18.img");
    vsimem_rm("/vsimem/ch2_out.cub");
}

TEST(IsisImportEndToEnd, FormatInference) {
    EXPECT_EQ(isisimport::infer_format_from_extension("x.cub"), isisimport::OutputFormat::Cube);
    EXPECT_EQ(isisimport::infer_format_from_extension("x.lbl"), isisimport::OutputFormat::Cube);
    EXPECT_EQ(isisimport::infer_format_from_extension("x.tif"), isisimport::OutputFormat::GTiff);
    EXPECT_EQ(isisimport::infer_format_from_extension("x.tiff"), isisimport::OutputFormat::GTiff);
    EXPECT_EQ(isisimport::infer_format_from_extension("x.foo"), isisimport::OutputFormat::Auto);
}
