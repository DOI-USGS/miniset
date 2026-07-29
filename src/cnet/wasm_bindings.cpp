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

// WASM control-network bindings.
//   - readNetProtobuf:        parse a legacy protobuf .net entirely in C++.
//   - readControlNetParquet:  parse a Parquet control net entirely in C++ via the
//                             header-only miniparquet reader (no hyparquet, no JS
//                             Parquet lib). This is the primary Parquet path.
//   - cnetReadColumns:        (legacy) build a ControlNet from Parquet columns
//                             parsed in JS and handed over as typed-array columns.
//                             Retained as a fallback; the native reader supersedes it.
// All return an opaque uintptr_t handle (like the CSM model bindings); JS reads
// the network back via the accessor functions and frees it with cnetDelete.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "cnet/column_source.hpp"
#include "cnet/control_net.hpp"
#include "cnet/control_net_reader.hpp"
#include "cnet/miniparquet_io.hpp"
#include "cnet/net_protobuf.hpp"
#include "cnet/stards_io.hpp"  // HeroPointsReader (stards.h is included only there)

using emscripten::val;

namespace {

// --- Pull typed columns out of a JS object of arrays -----------------------
std::vector<double> dblCol(const val& o, const char* key) {
    val v = o[key];
    if (v.isUndefined() || v.isNull()) return {};
    return emscripten::convertJSArrayToNumberVector<double>(v);
}
std::vector<int32_t> intCol(const val& o, const char* key) {
    val v = o[key];
    if (v.isUndefined() || v.isNull()) return {};
    std::vector<double> d = emscripten::convertJSArrayToNumberVector<double>(v);
    return std::vector<int32_t>(d.begin(), d.end());
}
std::vector<std::string> strCol(const val& o, const char* key) {
    val v = o[key];
    std::vector<std::string> out;
    if (v.isUndefined() || v.isNull()) return out;
    unsigned n = v["length"].as<unsigned>();
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        val e = v[i];
        out.push_back((e.isUndefined() || e.isNull()) ? std::string() : e.as<std::string>());
    }
    return out;
}
std::string hdr(const val& o, const char* key) {
    val v = o[key];
    return (v.isUndefined() || v.isNull()) ? std::string() : v.as<std::string>();
}

}  // namespace

/// Parse a protobuf .net from a JS byte array (Uint8Array). Pure C++.
uintptr_t readNetProtobuf_JS(val bytes) {
    std::vector<uint8_t> buf = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
    auto* net = new cnet::ControlNet(cnet::read_net_protobuf(buf));
    return reinterpret_cast<uintptr_t>(net);
}

/// Parse a Parquet control network from a JS byte array (Uint8Array) entirely in
/// C++ via the header-only miniparquet reader — no JS Parquet library. This is
/// the primary Parquet path in WASM; the file bytes are decoded in-memory.
uintptr_t readControlNetParquet_JS(val bytes) {
    std::vector<uint8_t> buf = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
    auto* net = new cnet::ControlNet(cnet::read_control_net_miniparquet_bytes(buf));
    return reinterpret_cast<uintptr_t>(net);
}

/// Build a ControlNet from a JS object holding the Parquet columns (parsed by a
/// JS Parquet library). Keys match the ColumnBatch field names. Legacy fallback.
uintptr_t cnetReadColumns_JS(val cols) {
    cnet::ColumnBatch b;
    b.networkId = hdr(cols, "networkId");
    b.targetName = hdr(cols, "targetName");
    b.created = hdr(cols, "created");
    b.lastModified = hdr(cols, "lastModified");
    b.description = hdr(cols, "description");
    b.userName = hdr(cols, "userName");

    b.id = strCol(cols, "id");
    b.type = intCol(cols, "type");
    b.chooserName = strCol(cols, "chooserName");
    b.datetime = strCol(cols, "datetime");
    b.editLock = intCol(cols, "editLock"); b.hasEditLock = intCol(cols, "hasEditLock");
    b.ignore = intCol(cols, "ignore"); b.hasIgnore = intCol(cols, "hasIgnore");
    b.jigsawRejected = intCol(cols, "jigsawRejected"); b.hasJigsawRejected = intCol(cols, "hasJigsawRejected");
    b.referenceIndex = intCol(cols, "referenceIndex"); b.hasReferenceIndex = intCol(cols, "hasReferenceIndex");
    b.aprioriSurfPointSource = intCol(cols, "aprioriSurfPointSource");
    b.aprioriRadiusSource = intCol(cols, "aprioriRadiusSource");
    b.aprioriSurfPointSourceFile = strCol(cols, "aprioriSurfPointSourceFile");
    b.aprioriRadiusSourceFile = strCol(cols, "aprioriRadiusSourceFile");
    b.aprioriX = dblCol(cols, "aprioriX"); b.aprioriY = dblCol(cols, "aprioriY"); b.aprioriZ = dblCol(cols, "aprioriZ");
    b.hasApriori = dblCol(cols, "hasApriori");
    b.adjustedX = dblCol(cols, "adjustedX"); b.adjustedY = dblCol(cols, "adjustedY"); b.adjustedZ = dblCol(cols, "adjustedZ");
    b.hasAdjusted = dblCol(cols, "hasAdjusted");
    b.aprioriCovar = dblCol(cols, "aprioriCovar"); b.aprioriCovarLen = intCol(cols, "aprioriCovarLen");
    b.adjustedCovar = dblCol(cols, "adjustedCovar"); b.adjustedCovarLen = intCol(cols, "adjustedCovarLen");

    b.serialnumber = strCol(cols, "serialnumber");
    b.measureType = intCol(cols, "measure_type");
    b.sample = dblCol(cols, "sample"); b.hasSample = dblCol(cols, "hasSample");
    b.line = dblCol(cols, "line"); b.hasLine = dblCol(cols, "hasLine");
    b.sampleResidual = dblCol(cols, "sampleResidual"); b.hasSampleResidual = dblCol(cols, "hasSampleResidual");
    b.lineResidual = dblCol(cols, "lineResidual"); b.hasLineResidual = dblCol(cols, "hasLineResidual");
    b.measureChooserName = strCol(cols, "measure_chooserName");
    b.measureDatetime = strCol(cols, "measure_datetime");
    b.measureEditLock = intCol(cols, "measure_editLock"); b.hasMeasureEditLock = intCol(cols, "hasMeasureEditLock");
    b.measureIgnore = intCol(cols, "measure_ignore"); b.hasMeasureIgnore = intCol(cols, "hasMeasureIgnore");
    b.measureJigsawRejected = intCol(cols, "measure_jigsawRejected"); b.hasMeasureJigsawRejected = intCol(cols, "hasMeasureJigsawRejected");
    b.diameter = dblCol(cols, "diameter"); b.hasDiameter = dblCol(cols, "hasDiameter");
    b.aprioriSample = dblCol(cols, "apriorisample"); b.hasAprioriSample = dblCol(cols, "hasAprioriSample");
    b.aprioriLine = dblCol(cols, "aprioriline"); b.hasAprioriLine = dblCol(cols, "hasAprioriLine");
    b.sampleSigma = dblCol(cols, "samplesigma"); b.hasSampleSigma = dblCol(cols, "hasSampleSigma");
    b.lineSigma = dblCol(cols, "linesigma"); b.hasLineSigma = dblCol(cols, "hasLineSigma");
    b.measureLogType = intCol(cols, "measure_logType"); b.measureLogValue = dblCol(cols, "measure_logValue");
    b.measureLogLen = intCol(cols, "measureLogLen");

    auto* net = new cnet::ControlNet(cnet::control_net_from_columns(b));
    return reinterpret_cast<uintptr_t>(net);
}

int cnetNumPoints_JS(uintptr_t h) { return (int)reinterpret_cast<cnet::ControlNet*>(h)->numPoints(); }
int cnetNumMeasures_JS(uintptr_t h) { return (int)reinterpret_cast<cnet::ControlNet*>(h)->numMeasures(); }

/// Return one point (and its measures) as a JS object for inspection in JS.
val cnetPoint_JS(uintptr_t h, int i) {
    auto* net = reinterpret_cast<cnet::ControlNet*>(h);
    val p = val::object();
    p.set("id", net->pointId[i]);
    p.set("type", (int)net->pointType[i]);
    p.set("numMeasures", (int)net->measureCount[i]);
    if (net->hasApriori[i]) {
        val a = val::object();
        a.set("x", net->aprioriX[i]); a.set("y", net->aprioriY[i]); a.set("z", net->aprioriZ[i]);
        p.set("apriori", a);
    }
    val measures = val::array();
    uint32_t s = net->measureStart[i];
    for (uint32_t m = 0; m < net->measureCount[i]; ++m) {
        val mv = val::object();
        mv.set("serialNumber", net->serialNumber[s + m]);
        mv.set("type", (int)net->measureType[s + m]);
        if (net->hasSample[s + m]) mv.set("sample", net->sample[s + m]);
        if (net->hasLine[s + m]) mv.set("line", net->line[s + m]);
        measures.set(m, mv);
    }
    p.set("measures", measures);
    return p;
}

val cnetHeader_JS(uintptr_t h) {
    auto* net = reinterpret_cast<cnet::ControlNet*>(h);
    val o = val::object();
    o.set("networkId", net->header.networkId);
    o.set("targetName", net->header.targetName);
    o.set("userName", net->header.userName);
    return o;
}

void cnetDelete_JS(uintptr_t h) { delete reinterpret_cast<cnet::ControlNet*>(h); }

// --- Lazy / remote reader over a URL (or local FS path) --------------------
// Opens a control-network Parquet by URL and reads only shape up front (footer +
// the id column) via Emscripten Fetch range requests — the file is NOT fully
// downloaded. readPoints then pulls only the requested points' row window with
// further ranged GETs. This is the cloud-optimized path: inspect/parse a
// GB-scale remote .parquet without downloading it whole. `url` may be
// "https://…", "/vsicurl/https://…", or a path in the WASM FS.
uintptr_t openControlNetReader_JS(std::string url) {
    auto* r = new cnet::ControlNetReader(cnet::ControlNetReader::open(url));
    return reinterpret_cast<uintptr_t>(r);
}
int cnetReaderNumPoints_JS(uintptr_t h) {
    return (int)reinterpret_cast<cnet::ControlNetReader*>(h)->numPoints();
}
int cnetReaderNumRows_JS(uintptr_t h) {
    return (int)reinterpret_cast<cnet::ControlNetReader*>(h)->numRows();
}
bool cnetReaderSupportsLazy_JS(uintptr_t h) {
    return reinterpret_cast<cnet::ControlNetReader*>(h)->supportsLazy();
}
val cnetReaderHeader_JS(uintptr_t h) {
    auto* r = reinterpret_cast<cnet::ControlNetReader*>(h);
    val o = val::object();
    o.set("networkId", r->header().networkId);
    o.set("targetName", r->header().targetName);
    o.set("userName", r->header().userName);
    return o;
}
std::string cnetReaderPointId_JS(uintptr_t h, int i) {
    return reinterpret_cast<cnet::ControlNetReader*>(h)->pointId(i);
}
// Read points [start, start+count) as a fresh ControlNet handle (free with
// cnetDelete). Only the covering row window is fetched.
uintptr_t cnetReaderReadPoints_JS(uintptr_t h, int start, int count) {
    auto* r = reinterpret_cast<cnet::ControlNetReader*>(h);
    auto* net = new cnet::ControlNet(r->readPoints((size_t)start, (size_t)count));
    return reinterpret_cast<uintptr_t>(net);
}
// Read the whole network via the reader (materializes all rows).
uintptr_t cnetReaderReadAll_JS(uintptr_t h) {
    auto* r = reinterpret_cast<cnet::ControlNetReader*>(h);
    auto* net = new cnet::ControlNet(r->readAll());
    return reinterpret_cast<uintptr_t>(net);
}
void cnetReaderClose_JS(uintptr_t h) { delete reinterpret_cast<cnet::ControlNetReader*>(h); }

// --- Hero-banner point reader (STREAMING over /vsicurl/) --------------------
// The docs banner renders the body-centered adjusted XYZ points of a control
// net as a rotating globe, streaming them in over time (rather than one giant
// read that blocks) so points fade into the render batch-by-batch. This is a
// three-call handle API mirroring the ControlNetReader lazy pattern:
//
//   h = openPointsXYZ(url)           open a StarDS net BY URL, keep it resident
//   n = pointsCount(h)               total adjusted points (from the index only)
//   b = pointsReadXYZ(h, start, k)   read window [start, start+k) as Float32Array
//   closePointsXYZ(h)                free the handle
//
// `url` is a StarDS path: a remote net over "/vsicurl/https://..." (or plain
// "https://...", "s3://...", "/vsis3/bucket/key") is read WITHOUT downloading
// the whole file — StarDS's WASM backend fetches only the covering byte ranges
// per call (open reads just the header/index via one ranged GET; each
// pointsReadXYZ pulls only the covering compressed blocks via get_slice). Local
// WASM-FS paths work too. This requires the module to be built with ENABLE_CURL
// + -sASYNCIFY (see CMakeLists.txt): under emscripten StarDS awaits the JS
// global fetch() and ASYNCIFY makes that look blocking to read_at().
//
// The net may be a compact XYZ-only .stards (keys adjX/adjY/adjZ) or a full
// normalized cnet (keys p.adjustedX/Y/Z) — both resolve here.
//
// Coordinates are returned RAW (body-centered body-fixed metres, interleaved
// x,y,z). Because the cloud covers the whole globe, its natural centre is the
// body origin (0,0,0), so no centroid pass is needed and each batch is
// independent — exactly what streaming wants. Each batch also reports the max
// radius within it so JS can frame the camera from the first batch (the globe
// radius is ~constant, so the framing stabilises immediately).
//
// The actual StarDS work lives in cnet::HeroPointsReader (src/cnet/stards_io.cpp)
// so that stards.h — which under emscripten emits a file-scope EM_ASYNC_JS fetch
// glue that must appear in exactly ONE translation unit — is included there only;
// this binding just marshals to/from JS.

uintptr_t openPointsXYZ_JS(std::string url) {
    auto* r = new cnet::HeroPointsReader(cnet::HeroPointsReader::open(url));
    return reinterpret_cast<uintptr_t>(r);
}

int pointsCount_JS(uintptr_t h) {
    return static_cast<int>(reinterpret_cast<cnet::HeroPointsReader*>(h)->count());
}

// Read window [start, start+count) as { positions: Float32Array(xyz*3), count,
// radius }. Only the covering compressed blocks are fetched — the streaming
// primitive. Clamps to the available range; an empty window returns count 0.
val pointsReadXYZ_JS(uintptr_t h, int start, int count) {
    auto* r = reinterpret_cast<cnet::HeroPointsReader*>(h);
    val out = val::object();

    size_t begin = start < 0 ? 0 : static_cast<size_t>(start);
    size_t want = count < 0 ? 0 : static_cast<size_t>(count);

    std::vector<float> xyz;
    double radius = 0.0;
    size_t n = r->readXYZ(begin, want, xyz, radius);

    // Copy into a JS Float32Array (typed_memory_view would alias freed C++ memory).
    val f32 = val::global("Float32Array").new_(val(xyz.size()));
    if (!xyz.empty())
        f32.call<void>("set", val(emscripten::typed_memory_view(xyz.size(), xyz.data())));
    out.set("positions", f32);
    out.set("count", static_cast<double>(n));
    out.set("radius", radius);
    return out;
}

void closePointsXYZ_JS(uintptr_t h) { delete reinterpret_cast<cnet::HeroPointsReader*>(h); }

EMSCRIPTEN_BINDINGS(miniset_cnet) {
    emscripten::function("readNetProtobuf", &readNetProtobuf_JS);
    emscripten::function("readControlNetParquet", &readControlNetParquet_JS);
    emscripten::function("cnetReadColumns", &cnetReadColumns_JS);
    emscripten::function("cnetNumPoints", &cnetNumPoints_JS);
    emscripten::function("cnetNumMeasures", &cnetNumMeasures_JS);
    emscripten::function("cnetPoint", &cnetPoint_JS);
    emscripten::function("cnetHeader", &cnetHeader_JS);
    emscripten::function("cnetDelete", &cnetDelete_JS);
    // Lazy / remote reader.
    emscripten::function("openControlNetReader", &openControlNetReader_JS);
    emscripten::function("cnetReaderNumPoints", &cnetReaderNumPoints_JS);
    emscripten::function("cnetReaderNumRows", &cnetReaderNumRows_JS);
    emscripten::function("cnetReaderSupportsLazy", &cnetReaderSupportsLazy_JS);
    emscripten::function("cnetReaderHeader", &cnetReaderHeader_JS);
    emscripten::function("cnetReaderPointId", &cnetReaderPointId_JS);
    emscripten::function("cnetReaderReadPoints", &cnetReaderReadPoints_JS);
    emscripten::function("cnetReaderReadAll", &cnetReaderReadAll_JS);
    emscripten::function("cnetReaderClose", &cnetReaderClose_JS);
    // Hero-banner adjusted-XYZ point reader (StarDS), streaming handle API.
    emscripten::function("openPointsXYZ", &openPointsXYZ_JS);
    emscripten::function("pointsCount", &pointsCount_JS);
    emscripten::function("pointsReadXYZ", &pointsReadXYZ_JS);
    emscripten::function("closePointsXYZ", &closePointsXYZ_JS);
}
