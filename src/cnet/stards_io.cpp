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

#include "cnet/stards_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "stards.h"

namespace cnet {

namespace {

using star::NDArray;
using star::StarDataset;

// Column key names. Point columns are prefixed "p.", measures "m.", the CSR
// offset/index arrays "x.". The network header lives in StarDS metadata.
// One StarDS array per ControlNet SoA vector keeps the mapping 1:1 and each
// column independently (block) compressed + sliceable.

// ---- data-array write helpers (separately-stored, block-compressed,
//      sliceable) ----------------------------------------------------------
template <typename T>
void putCol(StarDataset& ds, const std::string& key, const std::vector<T>& v) {
    NDArray<T> arr({v.size()}, T{});
    if (!v.empty()) std::copy(v.begin(), v.end(), arr.data().begin());
    ds.put(key, std::move(arr));
}

// enum vectors are stored as int32 arrays.
template <typename E>
void putEnumCol(StarDataset& ds, const std::string& key, const std::vector<E>& v) {
    std::vector<int32_t> tmp(v.size());
    for (size_t i = 0; i < v.size(); ++i) tmp[i] = static_cast<int32_t>(v[i]);
    putCol(ds, key, tmp);
}

// Header strings go in the metadata block (small, near-constant, read at open).
void putStr(StarDataset& ds, const std::string& key, const std::string& s) {
    ds.meta.put(key, NDArray<std::string>({1}, s));
}

// ---- data-array read helpers ----------------------------------------------
template <typename T>
std::vector<T> getCol(StarDataset& ds, const std::string& key) {
    if (!ds.contains(key)) return {};
    NDArray<T> arr = ds.get<T>(key);
    return std::vector<T>(arr.data().begin(), arr.data().end());
}

template <typename E>
std::vector<E> getEnumCol(StarDataset& ds, const std::string& key) {
    std::vector<int32_t> tmp = getCol<int32_t>(ds, key);
    std::vector<E> out(tmp.size());
    for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<E>(tmp[i]);
    return out;
}

std::string getStr(StarDataset& ds, const std::string& key) {
    if (ds.meta.contains(key)) {  // cnet/2: header in metadata block
        NDArray<std::string> arr = ds.meta.get(key)->as<std::string>();
        return arr.size() ? arr.flat(0) : std::string();
    }
    if (ds.contains(key)) {  // cnet/1: header as a 1-elt data string column
        NDArray<std::string> arr = ds.get<std::string>(key);
        return arr.size() ? arr.flat(0) : std::string();
    }
    return std::string();
}

// Read a half-open row range [begin,end) from a 1-D numeric column via
// get_slice, which fetches only the covering compressed blocks (coalesced
// ranged reads) rather than the whole column. Returns {} for absent columns.
template <typename T>
std::vector<T> sliceCol(StarDataset& ds, const std::string& key, size_t begin, size_t end) {
    if (!ds.contains(key) || end <= begin) return {};
    NDArray<T> arr = ds.get_slice<T>(key, {star::Slice{begin, end}});
    return std::vector<T>(arr.data().begin(), arr.data().end());
}

template <typename E>
std::vector<E> sliceEnumCol(StarDataset& ds, const std::string& key, size_t begin, size_t end) {
    std::vector<int32_t> tmp = sliceCol<int32_t>(ds, key, begin, end);
    std::vector<E> out(tmp.size());
    for (size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<E>(tmp[i]);
    return out;
}

// Slice a string column. The vendored StarDS get_slice cannot decode
// variable-width strings, so this reads the full (cached) column and copies the
// requested window. Callers pass an already-loaded full column to avoid re-read.
std::vector<std::string> windowStr(const std::vector<std::string>& full, size_t begin, size_t end) {
    if (end > full.size()) end = full.size();
    if (end <= begin) return {};
    return std::vector<std::string>(full.begin() + begin, full.begin() + end);
}

// ---- dictionary encoding for repeated string columns ----------------------
// Control-net string columns are massively repetitive: serial numbers reference
// a few thousand images across millions of measures; chooserName/datetime are
// near-constant. Store each as a dictionary (unique values, first-seen order) +
// a parallel int32 index column. The index column is a fixed-width numeric array
// that block-compresses (and shuffle-blocks) well and — unlike raw strings — is
// sliceable. Reconstruction is dict[index[i]].
struct Dict {
    std::vector<std::string> values;   // unique, first-seen order
    std::vector<int32_t> index;        // per-row -> values[]
};
Dict buildDict(const std::vector<std::string>& col) {
    Dict d;
    std::unordered_map<std::string, int32_t> seen;
    d.index.reserve(col.size());
    for (const std::string& s : col) {
        auto it = seen.find(s);
        int32_t id;
        if (it == seen.end()) { id = (int32_t)d.values.size(); seen.emplace(s, id); d.values.push_back(s); }
        else id = it->second;
        d.index.push_back(id);
    }
    return d;
}
// Write a dictionary-encoded string column: the small dict value table goes in
// the metadata block (<key>.dict, grabbed whole), the per-row int32 index goes
// in a sliceable data array (<key>.idx). (Putting .idx in metadata too was
// tried and measured LARGER, so the index stays a block-compressed data array.)
void putDictCol(StarDataset& ds, const std::string& key, const std::vector<std::string>& col) {
    Dict d = buildDict(col);
    if (d.values.empty()) d.values.push_back(std::string());  // benign 1-elt dict for a 0-row column
    size_t nd = d.values.size();
    ds.meta.put(key + ".dict", NDArray<std::string>(std::move(d.values), {nd}));
    putCol(ds, key + ".idx", d.index);
}
// Load a dict value table (metadata in cnet/2; data-array in early builds; the
// raw column itself in cnet/1).
std::vector<std::string> loadDict(StarDataset& ds, const std::string& key) {
    if (ds.meta.contains(key + ".dict")) {
        NDArray<std::string> a = ds.meta.get(key + ".dict")->as<std::string>();
        return std::vector<std::string>(a.data().begin(), a.data().end());
    }
    if (ds.contains(key + ".dict")) {
        NDArray<std::string> a = ds.get<std::string>(key + ".dict");
        return std::vector<std::string>(a.data().begin(), a.data().end());
    }
    return {};
}
// Load a dict index array from a data array (cnet/2) or, for the brief variant
// that stored it in metadata, from there.
std::vector<int32_t> loadIdx(StarDataset& ds, const std::string& key) {
    if (ds.contains(key + ".idx")) return getCol<int32_t>(ds, key + ".idx");
    if (ds.meta.contains(key + ".idx")) {
        NDArray<int32_t> a = ds.meta.get(key + ".idx")->as<int32_t>();
        std::vector<int32_t> v(a.data().begin(), a.data().end());
        if (ds.meta.contains(key + ".idxn"))
            v.resize(static_cast<size_t>(ds.meta.get(key + ".idxn")->as<uint64_t>().flat(0)));
        return v;
    }
    return {};
}
// Whether a dict-encoded column is present (either idx location).
bool hasIdx(StarDataset& ds, const std::string& key) {
    return ds.contains(key + ".idx") || ds.meta.contains(key + ".idx");
}
// Read a dictionary-encoded column into a flat string vector. Falls back to a
// raw (non-dictionary) string column at `key` for cnet/1 back-compat.
std::vector<std::string> getDictCol(StarDataset& ds, const std::string& key) {
    if (!hasIdx(ds, key)) {
        if (ds.contains(key)) {  // legacy cnet/1 raw string column
            NDArray<std::string> a = ds.get<std::string>(key);
            return std::vector<std::string>(a.data().begin(), a.data().end());
        }
        return {};
    }
    std::vector<std::string> dict = loadDict(ds, key);
    std::vector<int32_t> idx = loadIdx(ds, key);
    std::vector<std::string> out(idx.size());
    for (size_t i = 0; i < idx.size(); ++i)
        out[i] = (idx[i] >= 0 && (size_t)idx[i] < dict.size()) ? dict[idx[i]] : std::string();
    return out;
}

// The set of columns, written and read in the same order via a single macro so
// the two paths cannot drift. X(kind, key, member, type).
//
// String columns split into two kinds:
//   str  — stored raw (pointId is unique per point; a dictionary would only add
//          overhead).
//   dict — dictionary-encoded (<key>.dict + <key>.idx). Used for the massively
//          repetitive columns: image serial numbers (thousands of images across
//          millions of measures), chooser names, datetimes, source-file paths.
//          The index column is fixed-width int32 → block-compresses/shuffles and
//          is sliceable, unlike raw variable-width strings.
// Column kinds:
//   str  — raw string column (pointId; unique per point, no dictionary benefit).
//   dict — dictionary-encoded string column: the small unique-value TABLE lives
//          in the metadata block (grabbed whole), the per-row int32 index is a
//          sliceable data array. This is the only column class routed to .meta;
//          the network header also lives there. Everything else — presence
//          flags, enums, CSR offsets, and value columns — stays as regular
//          sliceable data arrays.
//   enum/u8/i32/u32/f64 — data arrays.
#define CNET_POINT_COLUMNS(X)                                                    \
    X(str,  "p.id",                     pointId)                                 \
    X(enum, "p.type",                   pointType)                              \
    X(dict, "p.chooserName",            chooserName)                            \
    X(dict, "p.datetime",               datetime)                              \
    X(u8,   "p.editLock",               editLock)                              \
    X(u8,   "p.hasEditLock",            hasEditLock)                            \
    X(u8,   "p.ignore",                 ignore)                                 \
    X(u8,   "p.hasIgnore",              hasIgnore)                              \
    X(u8,   "p.jigsawRejected",         jigsawRejected)                        \
    X(u8,   "p.hasJigsawRejected",      hasJigsawRejected)                     \
    X(i32,  "p.referenceIndex",         referenceIndex)                        \
    X(i32,  "p.hasReferenceIndex",      hasReferenceIndex)                     \
    X(enum, "p.aprioriSurfPointSource", aprioriSurfPointSource)               \
    X(dict, "p.aprioriSurfPointSourceFile", aprioriSurfPointSourceFile)       \
    X(enum, "p.aprioriRadiusSource",    aprioriRadiusSource)                  \
    X(dict, "p.aprioriRadiusSourceFile", aprioriRadiusSourceFile)            \
    X(f64,  "p.aprioriX",               aprioriX)                             \
    X(f64,  "p.aprioriY",               aprioriY)                             \
    X(f64,  "p.aprioriZ",               aprioriZ)                             \
    X(f64,  "p.hasApriori",             hasApriori)                           \
    X(f64,  "p.adjustedX",              adjustedX)                            \
    X(f64,  "p.adjustedY",              adjustedY)                            \
    X(f64,  "p.adjustedZ",              adjustedZ)                            \
    X(f64,  "p.hasAdjusted",            hasAdjusted)                          \
    X(u32,  "p.measureStart",           measureStart)                         \
    X(u32,  "p.measureCount",           measureCount)

#define CNET_MEASURE_COLUMNS(X)                                                  \
    X(dict, "m.serialNumber",           serialNumber)                          \
    X(enum, "m.measureType",            measureType)                           \
    X(f64,  "m.sample",                 sample)                                \
    X(f64,  "m.hasSample",              hasSample)                             \
    X(f64,  "m.line",                   line)                                  \
    X(f64,  "m.hasLine",                hasLine)                               \
    X(f64,  "m.sampleResidual",         sampleResidual)                        \
    X(f64,  "m.hasSampleResidual",      hasSampleResidual)                     \
    X(f64,  "m.lineResidual",           lineResidual)                          \
    X(f64,  "m.hasLineResidual",        hasLineResidual)                       \
    X(dict, "m.chooserName",            measureChooserName)                    \
    X(dict, "m.datetime",               measureDatetime)                       \
    X(u8,   "m.editLock",               measureEditLock)                       \
    X(u8,   "m.hasEditLock",            hasMeasureEditLock)                    \
    X(u8,   "m.ignore",                 measureIgnore)                         \
    X(u8,   "m.hasIgnore",              hasMeasureIgnore)                      \
    X(u8,   "m.jigsawRejected",         measureJigsawRejected)                 \
    X(u8,   "m.hasJigsawRejected",      hasMeasureJigsawRejected)              \
    X(f64,  "m.diameter",               diameter)                              \
    X(f64,  "m.hasDiameter",            hasDiameter)                           \
    X(f64,  "m.aprioriSample",          aprioriSample)                         \
    X(f64,  "m.hasAprioriSample",       hasAprioriSample)                      \
    X(f64,  "m.aprioriLine",            aprioriLine)                           \
    X(f64,  "m.hasAprioriLine",         hasAprioriLine)                        \
    X(f64,  "m.sampleSigma",            sampleSigma)                           \
    X(f64,  "m.hasSampleSigma",         hasSampleSigma)                        \
    X(f64,  "m.lineSigma",              lineSigma)                             \
    X(f64,  "m.hasLineSigma",           hasLineSigma)

// CSR-packed variable-length arrays (values + offsets), all sliceable data.
#define CNET_CSR_COLUMNS(X)                                                      \
    X(f64,  "x.aprioriCovar",           aprioriCovar)                          \
    X(u32,  "x.aprioriCovarOffset",     aprioriCovarOffset)                    \
    X(f64,  "x.adjustedCovar",          adjustedCovar)                         \
    X(u32,  "x.adjustedCovarOffset",    adjustedCovarOffset)                   \
    X(i32,  "x.measureLogType",         measureLogType)                        \
    X(f64,  "x.measureLogValue",        measureLogValue)                       \
    X(u32,  "x.measureLogOffset",       measureLogOffset)

// Dispatch a column of the named `kind` to put/get.
#define PUT_str(key, member)  putCol(ds, key, net.member)
#define PUT_dict(key, member) putDictCol(ds, key, net.member)
#define PUT_enum(key, member) putEnumCol(ds, key, net.member)
#define PUT_u8(key, member)   putCol(ds, key, net.member)
#define PUT_i32(key, member)  putCol(ds, key, net.member)
#define PUT_u32(key, member)  putCol(ds, key, net.member)
#define PUT_f64(key, member)  putCol(ds, key, net.member)

#define GET_str(key, member)  net.member = getCol<std::string>(ds, key)
#define GET_dict(key, member) net.member = getDictCol(ds, key)
#define GET_enum(key, member) net.member = getEnumCol<decltype(net.member)::value_type>(ds, key)
#define GET_u8(key, member)   net.member = getCol<uint8_t>(ds, key)
#define GET_i32(key, member)  net.member = getCol<int32_t>(ds, key)
#define GET_u32(key, member)  net.member = getCol<uint32_t>(ds, key)
#define GET_f64(key, member)  net.member = getCol<double>(ds, key)

}  // namespace

void write_control_net_stards(const ControlNet& net, const std::string& path) {
    // Normalized StarDS control-net layout ("cnet/2"):
    //   - one array per SoA column (no per-measure duplication of point data),
    //   - repetitive string columns dictionary-encoded: the small unique-value
    //     dict TABLE goes in the metadata block (grabbed whole in one decompress),
    //     the per-row int32 index stays a sliceable data array,
    //   - the network header lives in the metadata block too,
    //   - all other columns (flags, enums, CSR offsets, and value columns) are
    //     regular sliceable data arrays,
    //   - GZIP_SHUFFLE_BLOCK compression: byte-shuffle within each block so the
    //     slowly-varying high bytes of float64 coordinates compress well AND each
    //     block stays self-contained (still sliceable via get_slice).
    star::StarConfig cfg;
    cfg.compression = star::CompressionAlgorithm::GZIP_SHUFFLE_BLOCK;
    auto dsp = StarDataset::create(path, cfg);
    StarDataset& ds = *dsp;

    putStr(ds, "h.networkId", net.header.networkId);
    putStr(ds, "h.targetName", net.header.targetName);
    putStr(ds, "h.created", net.header.created);
    putStr(ds, "h.lastModified", net.header.lastModified);
    putStr(ds, "h.description", net.header.description);
    putStr(ds, "h.userName", net.header.userName);
    putStr(ds, "h.format", "cnet/2");

#define X(kind, key, member) PUT_##kind(key, member);
    CNET_POINT_COLUMNS(X)
    CNET_MEASURE_COLUMNS(X)
    CNET_CSR_COLUMNS(X)
#undef X

    dsp->close();  // flush to disk; avoids the destructor's best-effort reflush
}

ControlNet read_control_net_stards(const std::string& path) {
    auto dsp = StarDataset::open(path, star::FileMode::READ_ONLY);
    StarDataset& ds = *dsp;

    ControlNet net;
    net.header.networkId = getStr(ds, "h.networkId");
    net.header.targetName = getStr(ds, "h.targetName");
    net.header.created = getStr(ds, "h.created");
    net.header.lastModified = getStr(ds, "h.lastModified");
    net.header.description = getStr(ds, "h.description");
    net.header.userName = getStr(ds, "h.userName");

#define X(kind, key, member) GET_##kind(key, member);
    CNET_POINT_COLUMNS(X)
    CNET_MEASURE_COLUMNS(X)
    CNET_CSR_COLUMNS(X)
#undef X

    return net;
}

// ===========================================================================
// Lazy reader
// ===========================================================================

struct StardsControlNetReader::Impl {
    std::shared_ptr<StarDataset> ds;
    NetworkHeader header;

    // Point-level shape, read fully at open (small: one entry per point).
    std::vector<std::string> ids;              // pointId
    std::vector<uint32_t> measureStart;        // per point
    std::vector<uint32_t> measureCount;        // per point
    std::vector<uint32_t> aprioriCovarOffset;  // size numPoints + 1 (CSR)
    std::vector<uint32_t> adjustedCovarOffset; // size numPoints + 1
    size_t totalMeasures = 0;

    // measureLogOffset is per-measure (size numMeasures + 1); needed to map a
    // measure-row window onto the flattened log arrays. Small relative to the
    // log values themselves, so read fully once.
    std::vector<uint32_t> measureLogOffset;

    // String columns can't be block-sliced by the vendored StarDS, so cache the
    // full columns once (lazily) and window into them.
    mutable std::vector<std::string> serialNumberFull;
    mutable std::vector<std::string> chooserFull, datetimeFull;
    mutable std::vector<std::string> mChooserFull, mDatetimeFull;
    mutable std::vector<std::string> aprSurfFileFull, aprRadFileFull;
    mutable bool pointStrsLoaded = false;
    mutable bool measureStrsLoaded = false;

    // These string columns are dictionary-encoded in cnet/2; getDictCol also
    // reads the raw cnet/1 form, so the reader handles both formats.
    void loadPointStrings() const {
        if (pointStrsLoaded) return;
        chooserFull = getDictCol(*ds, "p.chooserName");
        datetimeFull = getDictCol(*ds, "p.datetime");
        aprSurfFileFull = getDictCol(*ds, "p.aprioriSurfPointSourceFile");
        aprRadFileFull = getDictCol(*ds, "p.aprioriRadiusSourceFile");
        pointStrsLoaded = true;
    }
    void loadMeasureStrings() const {
        if (measureStrsLoaded) return;
        serialNumberFull = getDictCol(*ds, "m.serialNumber");
        mChooserFull = getDictCol(*ds, "m.chooserName");
        mDatetimeFull = getDictCol(*ds, "m.datetime");
        measureStrsLoaded = true;
    }
};

StardsControlNetReader::StardsControlNetReader() : impl_(std::make_unique<Impl>()) {}
StardsControlNetReader::~StardsControlNetReader() = default;
StardsControlNetReader::StardsControlNetReader(StardsControlNetReader&&) noexcept = default;
StardsControlNetReader& StardsControlNetReader::operator=(StardsControlNetReader&&) noexcept = default;

StardsControlNetReader StardsControlNetReader::open(const std::string& path) {
    StardsControlNetReader r;
    Impl& im = *r.impl_;
    im.ds = StarDataset::open(path, star::FileMode::READ_ONLY);
    StarDataset& ds = *im.ds;

    im.header.networkId = getStr(ds, "h.networkId");
    im.header.targetName = getStr(ds, "h.targetName");
    im.header.created = getStr(ds, "h.created");
    im.header.lastModified = getStr(ds, "h.lastModified");
    im.header.description = getStr(ds, "h.description");
    im.header.userName = getStr(ds, "h.userName");

    // Point index columns (small: one per point).
    im.ids = getCol<std::string>(ds, "p.id");
    im.measureStart = getCol<uint32_t>(ds, "p.measureStart");
    im.measureCount = getCol<uint32_t>(ds, "p.measureCount");
    im.aprioriCovarOffset = getCol<uint32_t>(ds, "x.aprioriCovarOffset");
    im.adjustedCovarOffset = getCol<uint32_t>(ds, "x.adjustedCovarOffset");
    im.measureLogOffset = getCol<uint32_t>(ds, "x.measureLogOffset");

    // Total measures = serialNumber length; take it from the offset array tail
    // when present (numMeasures + 1), else fall back to measureStart/Count.
    if (im.measureLogOffset.size() >= 1)
        im.totalMeasures = im.measureLogOffset.size() - 1;
    else if (!im.measureStart.empty())
        im.totalMeasures = im.measureStart.back() + im.measureCount.back();
    return r;
}

size_t StardsControlNetReader::numPoints() const { return impl_->ids.size(); }
size_t StardsControlNetReader::numMeasures() const { return impl_->totalMeasures; }
const NetworkHeader& StardsControlNetReader::header() const { return impl_->header; }
const std::string& StardsControlNetReader::pointId(size_t i) const { return impl_->ids.at(i); }

void StardsControlNetReader::measureRange(size_t i, uint32_t& first, uint32_t& count) const {
    first = impl_->measureStart.at(i);
    count = impl_->measureCount.at(i);
}

ControlNet StardsControlNetReader::readPoints(size_t start, size_t count) const {
    const Impl& im = *impl_;
    ControlNet net;
    net.header = im.header;
    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);

    if (start >= numPoints() || count == 0) return net;
    size_t endPt = std::min(start + count, numPoints());
    StarDataset& ds = *im.ds;

    // Point row window [start, endPt) and the measure window it owns.
    const size_t pBegin = start, pEnd = endPt;
    const size_t mBegin = im.measureStart[start];
    const size_t mEnd = im.measureStart[endPt - 1] + im.measureCount[endPt - 1];

    // --- Point columns: numeric sliced via get_slice, strings windowed. -----
    net.pointType = sliceEnumCol<PointType>(ds, "p.type", pBegin, pEnd);
    net.editLock = sliceCol<uint8_t>(ds, "p.editLock", pBegin, pEnd);
    net.hasEditLock = sliceCol<uint8_t>(ds, "p.hasEditLock", pBegin, pEnd);
    net.ignore = sliceCol<uint8_t>(ds, "p.ignore", pBegin, pEnd);
    net.hasIgnore = sliceCol<uint8_t>(ds, "p.hasIgnore", pBegin, pEnd);
    net.jigsawRejected = sliceCol<uint8_t>(ds, "p.jigsawRejected", pBegin, pEnd);
    net.hasJigsawRejected = sliceCol<uint8_t>(ds, "p.hasJigsawRejected", pBegin, pEnd);
    net.referenceIndex = sliceCol<int32_t>(ds, "p.referenceIndex", pBegin, pEnd);
    net.hasReferenceIndex = sliceCol<int32_t>(ds, "p.hasReferenceIndex", pBegin, pEnd);
    net.aprioriSurfPointSource = sliceEnumCol<AprioriSource>(ds, "p.aprioriSurfPointSource", pBegin, pEnd);
    net.aprioriRadiusSource = sliceEnumCol<AprioriSource>(ds, "p.aprioriRadiusSource", pBegin, pEnd);
    net.aprioriX = sliceCol<double>(ds, "p.aprioriX", pBegin, pEnd);
    net.aprioriY = sliceCol<double>(ds, "p.aprioriY", pBegin, pEnd);
    net.aprioriZ = sliceCol<double>(ds, "p.aprioriZ", pBegin, pEnd);
    net.hasApriori = sliceCol<double>(ds, "p.hasApriori", pBegin, pEnd);
    net.adjustedX = sliceCol<double>(ds, "p.adjustedX", pBegin, pEnd);
    net.adjustedY = sliceCol<double>(ds, "p.adjustedY", pBegin, pEnd);
    net.adjustedZ = sliceCol<double>(ds, "p.adjustedZ", pBegin, pEnd);
    net.hasAdjusted = sliceCol<double>(ds, "p.hasAdjusted", pBegin, pEnd);

    im.loadPointStrings();
    net.pointId = windowStr(im.ids, pBegin, pEnd);
    net.chooserName = windowStr(im.chooserFull, pBegin, pEnd);
    net.datetime = windowStr(im.datetimeFull, pBegin, pEnd);
    net.aprioriSurfPointSourceFile = windowStr(im.aprSurfFileFull, pBegin, pEnd);
    net.aprioriRadiusSourceFile = windowStr(im.aprRadFileFull, pBegin, pEnd);

    // Re-base measure ownership so the slice is self-contained.
    net.measureStart.resize(pEnd - pBegin);
    net.measureCount.resize(pEnd - pBegin);
    for (size_t p = pBegin; p < pEnd; ++p) {
        net.measureStart[p - pBegin] = im.measureStart[p] - static_cast<uint32_t>(mBegin);
        net.measureCount[p - pBegin] = im.measureCount[p];
    }

    // --- CSR covariance windows (variable-length; slice the flattened data). -
    auto rebuildCsr = [&](const std::vector<uint32_t>& off, const char* dataKey,
                          std::vector<double>& outData, std::vector<uint32_t>& outOff) {
        size_t vBegin = off[pBegin], vEnd = off[pEnd];
        outData = sliceCol<double>(ds, dataKey, vBegin, vEnd);
        outOff.resize(pEnd - pBegin + 1);
        outOff[0] = 0;
        for (size_t p = pBegin; p < pEnd; ++p)
            outOff[p - pBegin + 1] = off[p + 1] - static_cast<uint32_t>(vBegin);
    };
    rebuildCsr(im.aprioriCovarOffset, "x.aprioriCovar", net.aprioriCovar, net.aprioriCovarOffset);
    rebuildCsr(im.adjustedCovarOffset, "x.adjustedCovar", net.adjustedCovar, net.adjustedCovarOffset);

    // --- Measure columns for [mBegin, mEnd): numeric sliced, strings windowed.
    net.measureType = sliceEnumCol<MeasureType>(ds, "m.measureType", mBegin, mEnd);
    net.sample = sliceCol<double>(ds, "m.sample", mBegin, mEnd);
    net.hasSample = sliceCol<double>(ds, "m.hasSample", mBegin, mEnd);
    net.line = sliceCol<double>(ds, "m.line", mBegin, mEnd);
    net.hasLine = sliceCol<double>(ds, "m.hasLine", mBegin, mEnd);
    net.sampleResidual = sliceCol<double>(ds, "m.sampleResidual", mBegin, mEnd);
    net.hasSampleResidual = sliceCol<double>(ds, "m.hasSampleResidual", mBegin, mEnd);
    net.lineResidual = sliceCol<double>(ds, "m.lineResidual", mBegin, mEnd);
    net.hasLineResidual = sliceCol<double>(ds, "m.hasLineResidual", mBegin, mEnd);
    net.measureEditLock = sliceCol<uint8_t>(ds, "m.editLock", mBegin, mEnd);
    net.hasMeasureEditLock = sliceCol<uint8_t>(ds, "m.hasEditLock", mBegin, mEnd);
    net.measureIgnore = sliceCol<uint8_t>(ds, "m.ignore", mBegin, mEnd);
    net.hasMeasureIgnore = sliceCol<uint8_t>(ds, "m.hasIgnore", mBegin, mEnd);
    net.measureJigsawRejected = sliceCol<uint8_t>(ds, "m.jigsawRejected", mBegin, mEnd);
    net.hasMeasureJigsawRejected = sliceCol<uint8_t>(ds, "m.hasJigsawRejected", mBegin, mEnd);
    net.diameter = sliceCol<double>(ds, "m.diameter", mBegin, mEnd);
    net.hasDiameter = sliceCol<double>(ds, "m.hasDiameter", mBegin, mEnd);
    net.aprioriSample = sliceCol<double>(ds, "m.aprioriSample", mBegin, mEnd);
    net.hasAprioriSample = sliceCol<double>(ds, "m.hasAprioriSample", mBegin, mEnd);
    net.aprioriLine = sliceCol<double>(ds, "m.aprioriLine", mBegin, mEnd);
    net.hasAprioriLine = sliceCol<double>(ds, "m.hasAprioriLine", mBegin, mEnd);
    net.sampleSigma = sliceCol<double>(ds, "m.sampleSigma", mBegin, mEnd);
    net.hasSampleSigma = sliceCol<double>(ds, "m.hasSampleSigma", mBegin, mEnd);
    net.lineSigma = sliceCol<double>(ds, "m.lineSigma", mBegin, mEnd);
    net.hasLineSigma = sliceCol<double>(ds, "m.hasLineSigma", mBegin, mEnd);

    im.loadMeasureStrings();
    net.serialNumber = windowStr(im.serialNumberFull, mBegin, mEnd);
    net.measureChooserName = windowStr(im.mChooserFull, mBegin, mEnd);
    net.measureDatetime = windowStr(im.mDatetimeFull, mBegin, mEnd);

    // --- Measure-log CSR window over [mBegin, mEnd). ------------------------
    if (!im.measureLogOffset.empty()) {
        size_t lBegin = im.measureLogOffset[mBegin], lEnd = im.measureLogOffset[mEnd];
        net.measureLogType = sliceCol<int32_t>(ds, "x.measureLogType", lBegin, lEnd);
        net.measureLogValue = sliceCol<double>(ds, "x.measureLogValue", lBegin, lEnd);
        net.measureLogOffset.resize(mEnd - mBegin + 1);
        net.measureLogOffset[0] = 0;
        for (size_t m = mBegin; m < mEnd; ++m)
            net.measureLogOffset[m - mBegin + 1] =
                im.measureLogOffset[m + 1] - static_cast<uint32_t>(lBegin);
    }
    return net;
}

ControlNet StardsControlNetReader::readMeasures(size_t rowStart, size_t rowCount) const {
    // Snap the raw measure window to the whole points that own it. StarDS stores
    // points normalized (point columns are per-point, not per-measure), so a
    // measure window alone can't carry point data; return the owning points.
    const Impl& im = *impl_;
    if (rowStart >= im.totalMeasures || rowCount == 0) return readPoints(0, 0);
    size_t rowEnd = std::min(rowStart + rowCount, im.totalMeasures);

    // Find first point whose measures overlap [rowStart, rowEnd).
    size_t pStart = 0;
    while (pStart + 1 < im.measureStart.size() && im.measureStart[pStart + 1] <= rowStart) ++pStart;
    size_t pEnd = pStart;
    while (pEnd < numPoints() && im.measureStart[pEnd] < rowEnd) ++pEnd;
    if (pEnd <= pStart) pEnd = pStart + 1;
    return readPoints(pStart, pEnd - pStart);
}

ControlNet StardsControlNetReader::readAll() const {
    return readPoints(0, numPoints());
}

// ===========================================================================
// Hero-banner adjusted-XYZ streaming reader
// ===========================================================================
// Lives here (not in wasm_bindings.cpp) so stards.h — whose emscripten build
// defines a file-scope EM_ASYNC_JS fetch glue that must appear in exactly ONE
// translation unit — is included by this .cpp only. The WASM bindings call this
// plain-C++ API. See the header for the streaming contract.

struct HeroPointsReader::Impl {
    std::shared_ptr<StarDataset> ds;
    std::string kx, ky, kz;  // resolved coordinate array keys
    size_t total = 0;        // total adjusted points available
};

HeroPointsReader::HeroPointsReader() : impl_(std::make_unique<Impl>()) {}
HeroPointsReader::~HeroPointsReader() = default;
HeroPointsReader::HeroPointsReader(HeroPointsReader&&) noexcept = default;
HeroPointsReader& HeroPointsReader::operator=(HeroPointsReader&&) noexcept = default;

HeroPointsReader HeroPointsReader::open(const std::string& path) {
    // WASM has no pthreads; keep StarDS serial. (No-op / harmless natively.)
    star::setNumThreads(1);
    HeroPointsReader r;
    Impl& im = *r.impl_;
    // open() routes /vsicurl/, s3://, /vsis3/, and local paths; for a remote URL
    // it reads only the header/index via one ranged GET (no whole-file download).
    im.ds = StarDataset::open(path, star::FileMode::READ_ONLY);

    // Resolve the coordinate keys and the point count in one pass. Use
    // array_length (array namespace only — no metadata probe, no extra fetch) and
    // treat a throw as "key absent": this both picks the layout (compact
    // adjX/adjY/adjZ vs full-cnet p.adjustedX/Y/Z) and reads the count from the
    // index. Avoiding contains()/meta lookups here also keeps the number of
    // network suspends minimal under the WASM (ASYNCIFY + fetch) backend.
    auto lenOf = [&](const char* key) -> size_t {
        try { return im.ds->array_length(key); } catch (...) { return 0; }
    };
    size_t nx = lenOf("adjX");
    if (nx > 0) {
        im.kx = "adjX"; im.ky = "adjY"; im.kz = "adjZ";
        im.total = std::min({nx, lenOf("adjY"), lenOf("adjZ")});
    } else {
        im.kx = "p.adjustedX"; im.ky = "p.adjustedY"; im.kz = "p.adjustedZ";
        im.total = std::min({lenOf("p.adjustedX"), lenOf("p.adjustedY"), lenOf("p.adjustedZ")});
    }
    return r;
}

size_t HeroPointsReader::count() const { return impl_->total; }

size_t HeroPointsReader::readXYZ(size_t start, size_t n,
                                 std::vector<float>& outXYZ, double& outMaxRadius) const {
    const Impl& im = *impl_;
    outMaxRadius = 0.0;
    outXYZ.clear();

    size_t begin = start;
    size_t end = start + n;
    if (end > im.total) end = im.total;
    if (begin >= end) return 0;

    // get_slice fetches only the covering compressed blocks (coalesced ranged
    // reads) — over /vsicurl this is one small HTTP range per batch, not the file.
    NDArray<double> ax = im.ds->get_slice<double>(im.kx, {star::Slice{begin, end}});
    NDArray<double> ay = im.ds->get_slice<double>(im.ky, {star::Slice{begin, end}});
    NDArray<double> az = im.ds->get_slice<double>(im.kz, {star::Slice{begin, end}});
    const size_t got = ax.size();

    outXYZ.resize(got * 3);
    double maxR2 = 0.0;
    for (size_t i = 0; i < got; ++i) {
        double x = ax.flat(i), y = ay.flat(i), z = az.flat(i);
        outXYZ[i * 3 + 0] = static_cast<float>(x);
        outXYZ[i * 3 + 1] = static_cast<float>(y);
        outXYZ[i * 3 + 2] = static_cast<float>(z);
        double r2 = x * x + y * y + z * z;
        if (r2 > maxR2) maxR2 = r2;
    }
    outMaxRadius = std::sqrt(maxR2);
    return got;
}

}  // namespace cnet
