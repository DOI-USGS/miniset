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

#include "core/types.hpp"
#include "utils/coordinate_transforms.hpp"
#include "stards.h"

namespace cnet {

namespace {

using star::NDArray;
using star::StarDataset;

// Open options for the cloud streaming readers (hero points, summary, lines).
// These always target the multi-GB remote net, where StarDS's default
// whole-file prefetch is pure waste: on open() the reader speculatively issues
// one GET of [0, prefetch_whole_below_bytes] (8 MiB) hoping the whole object
// fits its cache, then discards every byte when it learns the true size is far
// larger (stards.h ensure_whole_cached). That is ~8 MiB downloaded-and-thrown-
// away per open, before a single vertex can render. Setting the threshold to 0
// skips the prefetch entirely and goes straight to the index+ranged reads we
// actually use — the whole net never fits in memory anyway. (Small local/camera
// .stards still benefit from the default via the other open() paths.)
star::OpenOptions rangedOpenOptions() {
    star::OpenOptions opts;
    opts.prefetch_whole_below_bytes = 0;
    return opts;
}

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
    // rangedOpenOptions() suppresses the 8 MiB speculative whole-file prefetch,
    // which never fits (and is discarded) for the multi-GB net.
    im.ds = StarDataset::open(path, star::FileMode::READ_ONLY, rangedOpenOptions());

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

// ===========================================================================
// Gaussian-splat LOD summary ("cnet/3")
// ===========================================================================
//
// A compact overview of the point cloud: the adjusted points are partitioned
// into K contiguous runs (in existing point-array order — no reordering), and
// each run is summarized by one anisotropic 3D Gaussian (mean + 3x3 covariance)
// plus its point count and its [rangeStart, rangeCount) window. Because overlap
// tracks are long thin ribbons, an anisotropic Gaussian captures a ribbon
// segment with a single splat where a quadtree would need many cells. The range
// back-pointers let a client drill down from a splat to its exact points with
// one windowed get_slice read (HeroPointsReader::readXYZ / readPoints).

namespace {

// The StarDS layer + array/header keys for the summary. Keep in sync with the
// SummaryReader below and the WASM readSummary bindings.
constexpr const char* kSummaryLayer = "summary";
constexpr const char* kSummaryMethodSegment = "segment";
constexpr const char* kSummaryMethodTracks = "tracks";

// Per-splat streaming accumulator. Covariance is accumulated about a per-splat
// reference point (the first adjusted point in the run): at globe scale
// (~3.4e6 m) the raw second moment E[x^2] ~ 1.2e13 dwarfs the variance (a ribbon
// is ~1e3 m wide → var ~1e6), so accumulating x*x directly loses the variance in
// float64 rounding. Subtracting a reference keeps the sums O(extent^2).
struct SplatAccum {
    double refx = 0, refy = 0, refz = 0;  // reference origin (first point)
    bool haveRef = false;
    uint64_t n = 0;                        // adjusted points accumulated
    double sx = 0, sy = 0, sz = 0;         // Σ(x-ref)
    // Σ(x-ref)(y-ref) upper triangle: xx,xy,xz,yy,yz,zz
    double sxx = 0, sxy = 0, sxz = 0, syy = 0, syz = 0, szz = 0;

    void add(double x, double y, double z) {
        if (!haveRef) { refx = x; refy = y; refz = z; haveRef = true; }
        double dx = x - refx, dy = y - refy, dz = z - refz;
        sx += dx; sy += dy; sz += dz;
        sxx += dx * dx; sxy += dx * dy; sxz += dx * dz;
        syy += dy * dy; syz += dy * dz; szz += dz * dz;
        ++n;
    }
};

}  // namespace

namespace {

// Gather a column into a new vector following a point permutation `order`
// (out[i] = col[order[i]]). Empty/short columns (optional fields not populated)
// pass through unchanged so we never index out of range.
template <typename T>
std::vector<T> gatherPoints(const std::vector<T>& col, const std::vector<uint32_t>& order) {
    if (col.size() != order.size()) return col;  // not a per-point column here
    std::vector<T> out;
    out.reserve(order.size());
    for (uint32_t idx : order) out.push_back(col[idx]);
    return out;
}

// Apply a point permutation `order` (out point i = net point order[i]) to a
// whole net: gather every per-point column, move each point's measures with it
// (block-contiguous), and rebuild all CSR offsets. The result equals `net` as a
// SET of whole points; only order changes. `order` must be a permutation of
// [0, numPoints). Shared by reorder_points_by_track and the line writer.
ControlNet applyPointPermutation(const ControlNet& net, const std::vector<uint32_t>& order) {
    const size_t nPts = net.numPoints();

    // --- Gather every per-point column through `order`. --------------------
    ControlNet out;
    out.header = net.header;
    out.pointId = gatherPoints(net.pointId, order);
    out.pointType = gatherPoints(net.pointType, order);
    out.chooserName = gatherPoints(net.chooserName, order);
    out.datetime = gatherPoints(net.datetime, order);
    out.editLock = gatherPoints(net.editLock, order);
    out.hasEditLock = gatherPoints(net.hasEditLock, order);
    out.ignore = gatherPoints(net.ignore, order);
    out.hasIgnore = gatherPoints(net.hasIgnore, order);
    out.jigsawRejected = gatherPoints(net.jigsawRejected, order);
    out.hasJigsawRejected = gatherPoints(net.hasJigsawRejected, order);
    out.referenceIndex = gatherPoints(net.referenceIndex, order);
    out.hasReferenceIndex = gatherPoints(net.hasReferenceIndex, order);
    out.aprioriSurfPointSource = gatherPoints(net.aprioriSurfPointSource, order);
    out.aprioriSurfPointSourceFile = gatherPoints(net.aprioriSurfPointSourceFile, order);
    out.aprioriRadiusSource = gatherPoints(net.aprioriRadiusSource, order);
    out.aprioriRadiusSourceFile = gatherPoints(net.aprioriRadiusSourceFile, order);
    out.aprioriX = gatherPoints(net.aprioriX, order);
    out.aprioriY = gatherPoints(net.aprioriY, order);
    out.aprioriZ = gatherPoints(net.aprioriZ, order);
    out.hasApriori = gatherPoints(net.hasApriori, order);
    out.adjustedX = gatherPoints(net.adjustedX, order);
    out.adjustedY = gatherPoints(net.adjustedY, order);
    out.adjustedZ = gatherPoints(net.adjustedZ, order);
    out.hasAdjusted = gatherPoints(net.hasAdjusted, order);

    // --- 3. Rebuild measures (block-contiguous per new point order) + CSRs. -
    const bool haveApCovar = net.aprioriCovarOffset.size() == nPts + 1;
    const bool haveAdCovar = net.adjustedCovarOffset.size() == nPts + 1;
    const size_t nMeas = net.numMeasures();
    const bool haveLog = net.measureLogOffset.size() == nMeas + 1;

    out.measureStart.reserve(nPts);
    out.measureCount.reserve(nPts);
    if (haveApCovar) out.aprioriCovarOffset.reserve(nPts + 1);
    if (haveAdCovar) out.adjustedCovarOffset.reserve(nPts + 1);
    if (haveApCovar) out.aprioriCovarOffset.push_back(0);
    if (haveAdCovar) out.adjustedCovarOffset.push_back(0);
    if (haveLog) out.measureLogOffset.push_back(0);

    // Helper to append one measure (by old index mi) to the output measure arrays.
    auto appendMeasure = [&](size_t mi) {
        auto pushIf = [&](auto& dst, const auto& src) {
            if (mi < src.size()) dst.push_back(src[mi]);
        };
        pushIf(out.serialNumber, net.serialNumber);
        pushIf(out.measureType, net.measureType);
        pushIf(out.sample, net.sample); pushIf(out.hasSample, net.hasSample);
        pushIf(out.line, net.line); pushIf(out.hasLine, net.hasLine);
        pushIf(out.sampleResidual, net.sampleResidual); pushIf(out.hasSampleResidual, net.hasSampleResidual);
        pushIf(out.lineResidual, net.lineResidual); pushIf(out.hasLineResidual, net.hasLineResidual);
        pushIf(out.measureChooserName, net.measureChooserName);
        pushIf(out.measureDatetime, net.measureDatetime);
        pushIf(out.measureEditLock, net.measureEditLock); pushIf(out.hasMeasureEditLock, net.hasMeasureEditLock);
        pushIf(out.measureIgnore, net.measureIgnore); pushIf(out.hasMeasureIgnore, net.hasMeasureIgnore);
        pushIf(out.measureJigsawRejected, net.measureJigsawRejected); pushIf(out.hasMeasureJigsawRejected, net.hasMeasureJigsawRejected);
        pushIf(out.diameter, net.diameter); pushIf(out.hasDiameter, net.hasDiameter);
        pushIf(out.aprioriSample, net.aprioriSample); pushIf(out.hasAprioriSample, net.hasAprioriSample);
        pushIf(out.aprioriLine, net.aprioriLine); pushIf(out.hasAprioriLine, net.hasAprioriLine);
        pushIf(out.sampleSigma, net.sampleSigma); pushIf(out.hasSampleSigma, net.hasSampleSigma);
        pushIf(out.lineSigma, net.lineSigma); pushIf(out.hasLineSigma, net.hasLineSigma);
        // Per-measure log CSR: append this measure's [logOff[mi], logOff[mi+1]).
        if (haveLog) {
            uint32_t lb = net.measureLogOffset[mi], le = net.measureLogOffset[mi + 1];
            for (uint32_t li = lb; li < le; ++li) {
                if (li < net.measureLogType.size()) out.measureLogType.push_back(net.measureLogType[li]);
                if (li < net.measureLogValue.size()) out.measureLogValue.push_back(net.measureLogValue[li]);
            }
            out.measureLogOffset.push_back(static_cast<uint32_t>(out.measureLogType.size()));
        }
    };

    for (uint32_t oldP : order) {
        out.measureStart.push_back(static_cast<uint32_t>(out.numMeasures()));
        uint32_t s = net.measureStart[oldP], c = net.measureCount[oldP];
        out.measureCount.push_back(c);
        for (uint32_t m = 0; m < c; ++m) appendMeasure(static_cast<size_t>(s) + m);

        // Point-level covariance CSR: copy this point's [off[oldP], off[oldP+1]).
        if (haveApCovar) {
            uint32_t b = net.aprioriCovarOffset[oldP], e = net.aprioriCovarOffset[oldP + 1];
            for (uint32_t i = b; i < e; ++i)
                if (i < net.aprioriCovar.size()) out.aprioriCovar.push_back(net.aprioriCovar[i]);
            out.aprioriCovarOffset.push_back(static_cast<uint32_t>(out.aprioriCovar.size()));
        }
        if (haveAdCovar) {
            uint32_t b = net.adjustedCovarOffset[oldP], e = net.adjustedCovarOffset[oldP + 1];
            for (uint32_t i = b; i < e; ++i)
                if (i < net.adjustedCovar.size()) out.adjustedCovar.push_back(net.adjustedCovar[i]);
            out.adjustedCovarOffset.push_back(static_cast<uint32_t>(out.adjustedCovar.size()));
        }
    }
    return out;
}

// Track key per point: sorted serial numbers of its measures joined with a unit
// separator. Points observed in the same image set (same overlap) share a key.
std::string trackKey(const ControlNet& net, size_t p) {
    uint32_t s = net.measureStart[p], c = net.measureCount[p];
    std::vector<const std::string*> sns;
    sns.reserve(c);
    for (uint32_t m = 0; m < c; ++m) {
        size_t mi = static_cast<size_t>(s) + m;
        if (mi < net.serialNumber.size()) sns.push_back(&net.serialNumber[mi]);
    }
    std::sort(sns.begin(), sns.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });
    std::string k;
    for (const std::string* sn : sns) { k += *sn; k += '\x1f'; }
    return k;
}

}  // namespace

ControlNet reorder_points_by_track(const ControlNet& net) {
    const size_t nPts = net.numPoints();
    if (nPts == 0) return net;
    std::vector<std::string> key(nPts);
    for (size_t p = 0; p < nPts; ++p) key[p] = trackKey(net, p);
    std::vector<uint32_t> order(nPts);
    for (size_t i = 0; i < nPts; ++i) order[i] = static_cast<uint32_t>(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
    return applyPointPermutation(net, order);
}

GaussianSummary fit_gaussian_summary(const ControlNet& net, size_t k) {
    GaussianSummary out;
    const size_t nPts = net.numPoints();
    if (nPts == 0 || k == 0) return out;
    if (k > nPts) k = nPts;

    // Prefer adjusted (bundle-solved) coordinates; fall back to apriori when no
    // point has an adjusted coordinate (apriori-only nets are common — e.g. a net
    // that hasn't been bundle-adjusted yet). Whichever source is chosen, only
    // points whose corresponding has* flag is set contribute to a splat's fit.
    auto anySet = [&](const std::vector<double>& flag) {
        for (double f : flag) if (f != 0.0) return true;
        return false;
    };
    const bool adjOK = net.adjustedX.size() == nPts && net.adjustedY.size() == nPts &&
                       net.adjustedZ.size() == nPts && anySet(net.hasAdjusted);
    const bool aprOK = net.aprioriX.size() == nPts && net.aprioriY.size() == nPts &&
                       net.aprioriZ.size() == nPts && anySet(net.hasApriori);
    if (!adjOK && !aprOK) return out;  // no usable coordinates at all

    const std::vector<double>& X = adjOK ? net.adjustedX : net.aprioriX;
    const std::vector<double>& Y = adjOK ? net.adjustedY : net.aprioriY;
    const std::vector<double>& Z = adjOK ? net.adjustedZ : net.aprioriZ;
    const std::vector<double>& flag = adjOK ? net.hasAdjusted : net.hasApriori;
    const bool haveFlag = flag.size() == nPts;

    out.muX.reserve(k); out.muY.reserve(k); out.muZ.reserve(k);
    out.s0.reserve(k); out.s1.reserve(k); out.s2.reserve(k);
    out.s3.reserve(k); out.s4.reserve(k); out.s5.reserve(k);
    out.weight.reserve(k); out.rangeStart.reserve(k); out.rangeCount.reserve(k);

    // Partition the point index range [0, nPts) into k contiguous segments of
    // ~equal SIZE (by point count, not by adjusted-point count). Each segment's
    // range covers every point index in it — including un-adjusted points that
    // don't contribute to the fit — so the ranges stay a contiguous partition of
    // [0, nPts), which is what makes drill-down a single window.
    for (size_t seg = 0; seg < k; ++seg) {
        size_t begin = static_cast<size_t>((static_cast<double>(seg) * nPts) / k);
        size_t end = static_cast<size_t>((static_cast<double>(seg + 1) * nPts) / k);
        if (seg + 1 == k) end = nPts;   // last segment absorbs the remainder
        if (end <= begin) continue;      // possible when k≈nPts and rounding collides

        SplatAccum a;
        for (size_t p = begin; p < end; ++p) {
            if (haveFlag && flag[p] == 0.0) continue;  // skip points without this coord
            a.add(X[p], Y[p], Z[p]);
        }

        double muX, muY, muZ, c0, c1, c2, c3, c4, c5;
        if (a.n == 0) {
            // A segment with no adjusted points: emit a degenerate zero-weight
            // splat at the origin so the ranges still tile [0, nPts) exactly.
            muX = muY = muZ = 0.0;
            c0 = c1 = c2 = c3 = c4 = c5 = 0.0;
        } else {
            const double inv = 1.0 / static_cast<double>(a.n);
            const double mx = a.sx * inv, my = a.sy * inv, mz = a.sz * inv;
            muX = a.refx + mx; muY = a.refy + my; muZ = a.refz + mz;
            // Cov = E[(x-ref)(y-ref)] - m*m  (population covariance about the mean).
            c0 = a.sxx * inv - mx * mx;   // xx
            c1 = a.sxy * inv - mx * my;   // xy
            c2 = a.sxz * inv - mx * mz;   // xz
            c3 = a.syy * inv - my * my;   // yy
            c4 = a.syz * inv - my * mz;   // yz
            c5 = a.szz * inv - mz * mz;   // zz
            // Guard tiny negative diagonals from rounding.
            if (c0 < 0) c0 = 0; if (c3 < 0) c3 = 0; if (c5 < 0) c5 = 0;
        }

        out.muX.push_back(muX); out.muY.push_back(muY); out.muZ.push_back(muZ);
        out.s0.push_back(c0); out.s1.push_back(c1); out.s2.push_back(c2);
        out.s3.push_back(c3); out.s4.push_back(c4); out.s5.push_back(c5);
        out.weight.push_back(static_cast<uint32_t>(a.n));
        out.rangeStart.push_back(static_cast<uint32_t>(begin));
        out.rangeCount.push_back(static_cast<uint32_t>(end - begin));
    }
    return out;
}

namespace {

// Write the summary into an open dataset as the "summary" layer + header stamps.
void writeSummaryLayer(StarDataset& ds, const GaussianSummary& s,
                       const char* method, bool reordered) {
    auto layer = ds.create_layer(kSummaryLayer);
    auto putD = [&](const char* key, const std::vector<double>& v) {
        NDArray<double> arr({v.size()}, 0.0);
        if (!v.empty()) std::copy(v.begin(), v.end(), arr.data().begin());
        layer->put(key, std::move(arr));
    };
    auto putU = [&](const char* key, const std::vector<uint32_t>& v) {
        NDArray<uint32_t> arr({v.size()}, 0u);
        if (!v.empty()) std::copy(v.begin(), v.end(), arr.data().begin());
        layer->put(key, std::move(arr));
    };
    putD("summary.muX", s.muX); putD("summary.muY", s.muY); putD("summary.muZ", s.muZ);
    putD("summary.s0", s.s0); putD("summary.s1", s.s1); putD("summary.s2", s.s2);
    putD("summary.s3", s.s3); putD("summary.s4", s.s4); putD("summary.s5", s.s5);
    putU("summary.weight", s.weight);
    putU("summary.rangeStart", s.rangeStart);
    putU("summary.rangeCount", s.rangeCount);

    // Base-layer header stamps (metadata block) marking this as cnet/3.
    putStr(ds, "h.format", "cnet/3");
    putStr(ds, "h.summaryCount", std::to_string(s.size()));
    putStr(ds, "h.summaryMethod", method);
    putStr(ds, "h.pointsReordered", reordered ? "1" : "0");
}

}  // namespace

void write_control_net_stards_summarized(const ControlNet& net_in, const std::string& path,
                                         size_t k, bool byTracks) {
    // For "tracks" mode, reorder points so image-overlap tracks are contiguous;
    // the STORED net is then the reordered one and the splat ranges index it. For
    // "segment" mode the points keep their existing order. Bind `net` to whichever
    // we actually write (avoids copying in the common segment path).
    ControlNet reordered;
    if (byTracks) reordered = reorder_points_by_track(net_in);
    const ControlNet& net = byTracks ? reordered : net_in;

    star::StarConfig cfg;
    cfg.compression = star::CompressionAlgorithm::GZIP_SHUFFLE_BLOCK;
    auto dsp = StarDataset::create(path, cfg);
    StarDataset& ds = *dsp;

    // Base layer: identical to write_control_net_stards (cnet/2 arrays), but with
    // h.format overwritten to cnet/3 by writeSummaryLayer below.
    putStr(ds, "h.networkId", net.header.networkId);
    putStr(ds, "h.targetName", net.header.targetName);
    putStr(ds, "h.created", net.header.created);
    putStr(ds, "h.lastModified", net.header.lastModified);
    putStr(ds, "h.description", net.header.description);
    putStr(ds, "h.userName", net.header.userName);
    putStr(ds, "h.format", "cnet/2");  // overwritten to cnet/3 by writeSummaryLayer

#define X(kind, key, member) PUT_##kind(key, member);
    CNET_POINT_COLUMNS(X)
    CNET_MEASURE_COLUMNS(X)
    CNET_CSR_COLUMNS(X)
#undef X

    GaussianSummary summary = fit_gaussian_summary(net, k);
    writeSummaryLayer(ds, summary,
                      byTracks ? kSummaryMethodTracks : kSummaryMethodSegment,
                      byTracks);

    dsp->close();
}

// ===========================================================================
// Summary reader
// ===========================================================================

struct SummaryReader::Impl {
    std::shared_ptr<StarDataset> ds;
    GaussianSummary splats;
};

SummaryReader::SummaryReader() : impl_(std::make_unique<Impl>()) {}
SummaryReader::~SummaryReader() = default;
SummaryReader::SummaryReader(SummaryReader&&) noexcept = default;
SummaryReader& SummaryReader::operator=(SummaryReader&&) noexcept = default;

SummaryReader SummaryReader::open(const std::string& path) {
    star::setNumThreads(1);  // WASM has no pthreads; harmless natively.
    SummaryReader r;
    Impl& im = *r.impl_;
    im.ds = StarDataset::open(path, star::FileMode::READ_ONLY, rangedOpenOptions());
    if (!im.ds->has_layer(kSummaryLayer)) return r;  // no summary → count()==0

    auto layer = im.ds->get_layer(kSummaryLayer);
    // The layer arrays are small; read each whole. A missing array (older writer)
    // leaves that vector empty, which the getters below tolerate.
    auto getD = [&](const char* key) -> std::vector<double> {
        try {
            NDArray<double> a = layer->get<double>(key);
            return std::vector<double>(a.data().begin(), a.data().end());
        } catch (...) { return {}; }
    };
    auto getU = [&](const char* key) -> std::vector<uint32_t> {
        try {
            NDArray<uint32_t> a = layer->get<uint32_t>(key);
            return std::vector<uint32_t>(a.data().begin(), a.data().end());
        } catch (...) { return {}; }
    };
    GaussianSummary& s = im.splats;
    s.muX = getD("summary.muX"); s.muY = getD("summary.muY"); s.muZ = getD("summary.muZ");
    s.s0 = getD("summary.s0"); s.s1 = getD("summary.s1"); s.s2 = getD("summary.s2");
    s.s3 = getD("summary.s3"); s.s4 = getD("summary.s4"); s.s5 = getD("summary.s5");
    s.weight = getU("summary.weight");
    s.rangeStart = getU("summary.rangeStart");
    s.rangeCount = getU("summary.rangeCount");
    return r;
}

size_t SummaryReader::count() const { return impl_->splats.size(); }
const GaussianSummary& SummaryReader::splats() const { return impl_->splats; }

// ===========================================================================
// Polyline ("lines") LOD summary — the surface-parametric track model
// ===========================================================================
//
// Filaments are traced geometrically in the adjusted point cloud (crossing image
// boundaries freely), simplified to polylines, and stored as line-relative int16
// (lon,lat) on the biaxial ellipsoid — compact, on-surface, faithful to track
// shape. Points are reordered so each filament is a contiguous window, so the
// per-line rangeStart/rangeCount drives real-point drill-down. See the prototype
// notes in the design plan: ~92% coverage, ~4 MB, ~9 m resolution on a real net.

namespace {

constexpr const char* kLinesLayer = "lines";

// --- voxel grid over BCBF XYZ for O(1)-ish neighbor queries ----------------
// Cells packed into 63 bits (21 bits/axis, offset by 2^20). Cell size == the
// neighbor search radius, so a point's neighbors lie in its 27-cell stencil.
struct VoxelGrid {
    double cell;
    std::unordered_map<long long, std::vector<uint32_t>> cells;
    static long long pack(long long cx, long long cy, long long cz) {
        auto o = [](long long v) { return (unsigned long long)(v + 1048576); };
        return (long long)((o(cx) & 0x1FFFFF) | ((o(cy) & 0x1FFFFF) << 21) | ((o(cz) & 0x1FFFFF) << 42));
    }
    void cellOf(double x, double y, double z, long long& cx, long long& cy, long long& cz) const {
        cx = (long long)std::floor(x / cell);
        cy = (long long)std::floor(y / cell);
        cz = (long long)std::floor(z / cell);
    }
};

// Perpendicular distance of point p to segment a-b (3D).
double perpDist(const double* p, const double* a, const double* b) {
    double ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    double L2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
    if (L2 <= 0) {
        double d[3] = {p[0]-a[0], p[1]-a[1], p[2]-a[2]};
        return std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    }
    double t = ((p[0]-a[0])*ab[0] + (p[1]-a[1])*ab[1] + (p[2]-a[2])*ab[2]) / L2;
    if (t < 0) t = 0; if (t > 1) t = 1;
    double c[3] = {a[0]+t*ab[0], a[1]+t*ab[1], a[2]+t*ab[2]};
    double d[3] = {p[0]-c[0], p[1]-c[1], p[2]-c[2]};
    return std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
}

// Iterative 3D Douglas–Peucker; fills `keep` (1 = vertex retained).
void douglasPeucker(const std::vector<std::array<double,3>>& pts, double eps,
                    std::vector<char>& keep) {
    size_t n = pts.size();
    keep.assign(n, 0);
    if (n == 0) return;
    keep[0] = keep[n-1] = 1;
    if (n < 3) return;
    std::vector<std::pair<size_t,size_t>> st = {{0, n-1}};
    while (!st.empty()) {
        auto [a, b] = st.back(); st.pop_back();
        if (b <= a + 1) continue;
        double dmax = -1; size_t idx = a;
        for (size_t i = a+1; i < b; ++i) {
            double d = perpDist(pts[i].data(), pts[a].data(), pts[b].data());
            if (d > dmax) { dmax = d; idx = i; }
        }
        if (dmax > eps) { keep[idx] = 1; st.push_back({a, idx}); st.push_back({idx, b}); }
    }
}

}  // namespace

void write_control_net_stards_lines(const ControlNet& net_in, const std::string& path,
                                    const LineSummaryOptions& opts) {
    const size_t nPts0 = net_in.numPoints();

    // --- 1. Trace geometric filaments in the ADJUSTED point cloud. ----------
    // Build a voxel grid, then grow chains by greedy direction continuation:
    // from the current head, step to the unused neighbor that best continues the
    // heading (min turn), nearest first for the seed step.
    const double cell = opts.cellSize, sr2 = opts.cellSize * opts.cellSize;
    const double cosTurn = std::cos(opts.maxTurnDeg * M_PI / 180.0);
    const bool haveAdj = net_in.adjustedX.size() == nPts0 && net_in.hasAdjusted.size() == nPts0;

    VoxelGrid grid; grid.cell = cell;
    auto adjOf = [&](size_t i, double& x, double& y, double& z) {
        x = net_in.adjustedX[i]; y = net_in.adjustedY[i]; z = net_in.adjustedZ[i];
    };
    if (haveAdj) {
        for (uint32_t i = 0; i < nPts0; ++i) {
            if (net_in.hasAdjusted[i] == 0.0) continue;
            long long cx, cy, cz; grid.cellOf(net_in.adjustedX[i], net_in.adjustedY[i], net_in.adjustedZ[i], cx, cy, cz);
            grid.cells[VoxelGrid::pack(cx, cy, cz)].push_back(i);
        }
    }
    std::vector<uint8_t> used(nPts0, 0);
    std::vector<uint32_t> nb;
    auto neighbors = [&](uint32_t i) {
        nb.clear();
        long long cx, cy, cz; grid.cellOf(net_in.adjustedX[i], net_in.adjustedY[i], net_in.adjustedZ[i], cx, cy, cz);
        for (int dx=-1; dx<=1; ++dx) for (int dy=-1; dy<=1; ++dy) for (int dz=-1; dz<=1; ++dz) {
            auto it = grid.cells.find(VoxelGrid::pack(cx+dx, cy+dy, cz+dz));
            if (it == grid.cells.end()) continue;
            for (uint32_t j : it->second) {
                if (j == i || used[j]) continue;
                double d[3] = {net_in.adjustedX[j]-net_in.adjustedX[i],
                               net_in.adjustedY[j]-net_in.adjustedY[i],
                               net_in.adjustedZ[j]-net_in.adjustedZ[i]};
                if (d[0]*d[0]+d[1]*d[1]+d[2]*d[2] <= sr2) nb.push_back(j);
            }
        }
    };

    std::vector<std::vector<uint32_t>> chains;  // each = point indices along a filament
    if (haveAdj) {
        for (uint32_t seed = 0; seed < nPts0; ++seed) {
            if (used[seed] || net_in.hasAdjusted[seed] == 0.0) continue;
            std::vector<uint32_t> chain = {seed}; used[seed] = 1;
            uint32_t cur = seed; bool haveDir = false; double dir[3] = {0,0,0};
            for (;;) {
                neighbors(cur);
                if (nb.empty()) break;
                uint32_t best = UINT32_MAX; double bestScore = -1e30;
                for (uint32_t j : nb) {
                    double s[3] = {net_in.adjustedX[j]-net_in.adjustedX[cur],
                                   net_in.adjustedY[j]-net_in.adjustedY[cur],
                                   net_in.adjustedZ[j]-net_in.adjustedZ[cur]};
                    double l = std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
                    if (l <= 0) continue;
                    double score;
                    if (!haveDir) { score = -l; }  // seed step: nearest
                    else {
                        double dot = (s[0]*dir[0]+s[1]*dir[1]+s[2]*dir[2]) / l;
                        if (dot < cosTurn) continue;  // too sharp a turn
                        score = dot - 1e-6 * l;       // best continuation, nearer tiebreak
                    }
                    if (score > bestScore) { bestScore = score; best = j; }
                }
                if (best == UINT32_MAX) break;
                double s[3] = {net_in.adjustedX[best]-net_in.adjustedX[cur],
                               net_in.adjustedY[best]-net_in.adjustedY[cur],
                               net_in.adjustedZ[best]-net_in.adjustedZ[cur]};
                double l = std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
                dir[0]=s[0]/l; dir[1]=s[1]/l; dir[2]=s[2]/l; haveDir = true;
                used[best] = 1; chain.push_back(best); cur = best;
            }
            chains.push_back(std::move(chain));
        }
    }

    // --- 2. Reorder points so each LONG filament is a contiguous block. -----
    // Order = [long-filament points in filament order] then [everything else in
    // original order]. Long filaments get contiguous [rangeStart,rangeCount).
    std::vector<uint32_t> order; order.reserve(nPts0);
    std::vector<uint8_t> placed(nPts0, 0);
    std::vector<std::pair<uint32_t,uint32_t>> lineRanges;  // (rangeStart, rangeCount) in NEW order
    for (auto& c : chains) {
        if (c.size() < opts.minLen) continue;
        uint32_t start = static_cast<uint32_t>(order.size());
        for (uint32_t idx : c) { order.push_back(idx); placed[idx] = 1; }
        lineRanges.push_back({start, static_cast<uint32_t>(c.size())});
    }
    for (uint32_t i = 0; i < nPts0; ++i) if (!placed[i]) order.push_back(i);

    ControlNet net = applyPointPermutation(net_in, order);  // stored (reordered) net

    // --- 3. Simplify each long filament + quantize vertices to (lon,lat). ----
    // GLOBAL int16 quantization: lon∈[-π,π]→int16, lat∈[-π/2,π/2]→int16. Unlike a
    // tile-relative scheme this can never saturate — a line may span the whole
    // globe (12% span >1 tile, some wrap 360°) and still reconstruct exactly.
    // Resolution is ~2πA/65536 ≈ 326 m in lon, ~162 m in lat — ample for an
    // overview and cheap to compress (slowly-varying int16 under shuffle+GZIP).
    const double A = opts.radiusA, C = opts.radiusC;
    const double LON_S = 65535.0 / (2 * M_PI);   // rad → int16 code
    const double LAT_S = 65535.0 / M_PI;
    std::vector<int16_t> qlon, qlat;                 // global quantized vertices
    std::vector<uint32_t> voff = {0};                // vertex CSR
    std::vector<uint32_t> rangeStart, rangeCount;    // point windows (in NEW order)

    std::vector<std::array<double,3>> pl;
    std::vector<char> keep;
    for (auto& lr : lineRanges) {
        uint32_t start = lr.first, cnt = lr.second;
        pl.clear();
        for (uint32_t k = 0; k < cnt; ++k) {
            size_t p = start + k;   // NEW order → contiguous
            pl.push_back({net.adjustedX[p], net.adjustedY[p], net.adjustedZ[p]});
        }
        douglasPeucker(pl, opts.simplifyEps, keep);
        for (size_t i = 0; i < pl.size(); ++i) {
            if (!keep[i]) continue;
            Vec3 p{pl[i][0], pl[i][1], pl[i][2]};
            LatLon ll = utils::ecefToLatLon(p, A, C);
            long qi = std::lround(ll.lon * LON_S); if (qi < -32768) qi = -32768; if (qi > 32767) qi = 32767;
            long qj = std::lround(ll.lat * LAT_S); if (qj < -32768) qj = -32768; if (qj > 32767) qj = 32767;
            qlon.push_back((int16_t)qi); qlat.push_back((int16_t)qj);
        }
        rangeStart.push_back(start);
        rangeCount.push_back(cnt);
        voff.push_back((uint32_t)qlon.size());
    }
    const size_t L = rangeStart.size();

    // --- 4. L0 density map: per-tile point count over the whole cloud. -------
    const int DLON = opts.densTilesLon, DLAT = opts.densTilesLat;
    std::vector<int32_t> dens(DLON * DLAT, 0);
    if (haveAdj) {
        for (size_t p = 0; p < net.numPoints(); ++p) {
            if (net.hasAdjusted[p] == 0.0) continue;
            Vec3 pt{net.adjustedX[p], net.adjustedY[p], net.adjustedZ[p]};
            LatLon ll = utils::ecefToLatLon(pt, A, C);
            int tx = (int)((ll.lon + M_PI) / (2*M_PI) * DLON); if (tx<0) tx=0; if (tx>=DLON) tx=DLON-1;
            int ty = (int)((ll.lat + M_PI/2) / M_PI * DLAT); if (ty<0) ty=0; if (ty>=DLAT) ty=DLAT-1;
            dens[ty * DLON + tx]++;
        }
    }

    // --- 5. Write: base cnet/2 arrays (reordered) + "lines" layer. ----------
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
    putStr(ds, "h.format", "cnet/2");   // overwritten below

#define X(kind, key, member) PUT_##kind(key, member);
    CNET_POINT_COLUMNS(X)
    CNET_MEASURE_COLUMNS(X)
    CNET_CSR_COLUMNS(X)
#undef X

    auto layer = ds.create_layer(kLinesLayer);
    auto putI16 = [&](const char* k, const std::vector<int16_t>& v) {
        NDArray<int16_t> a({v.size()}, (int16_t)0);
        if (!v.empty()) std::copy(v.begin(), v.end(), a.data().begin());
        layer->put(k, std::move(a));
    };
    auto putI32 = [&](const char* k, const std::vector<int32_t>& v) {
        NDArray<int32_t> a({v.size()}, 0);
        if (!v.empty()) std::copy(v.begin(), v.end(), a.data().begin());
        layer->put(k, std::move(a));
    };
    auto putU32 = [&](const char* k, const std::vector<uint32_t>& v) {
        NDArray<uint32_t> a({v.size()}, 0u);
        if (!v.empty()) std::copy(v.begin(), v.end(), a.data().begin());
        layer->put(k, std::move(a));
    };
    putI16("lines.qlon", qlon);
    putI16("lines.qlat", qlat);
    putU32("lines.voff", voff);
    putU32("lines.rangeStart", rangeStart);
    putU32("lines.rangeCount", rangeCount);
    putI32("lines.dens", dens);
    (void)putI32;  // (kept for the density array above)

    // Header stamps (base metadata block): format + method + ellipsoid so the
    // reader can dequantize without out-of-band knowledge. Global int16 encoding,
    // so no tiling params are needed.
    putStr(ds, "h.format", "cnet/3");
    putStr(ds, "h.summaryMethod", "lines");
    putStr(ds, "h.pointsReordered", "1");
    putStr(ds, "h.lineCount", std::to_string(L));
    putStr(ds, "h.linesRadiusA", std::to_string((long long)std::llround(A)));
    putStr(ds, "h.linesRadiusC", std::to_string((long long)std::llround(C)));
    putStr(ds, "h.densTilesLon", std::to_string(DLON));
    putStr(ds, "h.densTilesLat", std::to_string(DLAT));

    dsp->close();
}

// ===========================================================================
// Lines reader (portable — dequantizes to BCBF XYZ)
// ===========================================================================

struct LinesReader::Impl {
    std::shared_ptr<StarDataset> ds;
    size_t nLines = 0;
    std::vector<uint32_t> voff;         // vertex CSR (size L+1)
    std::vector<uint32_t> rangeStart, rangeCount;
    std::vector<float> vxyz;            // dequantized vertices, interleaved xyz
};

LinesReader::LinesReader() : impl_(std::make_unique<Impl>()) {}
LinesReader::~LinesReader() = default;
LinesReader::LinesReader(LinesReader&&) noexcept = default;
LinesReader& LinesReader::operator=(LinesReader&&) noexcept = default;

LinesReader LinesReader::open(const std::string& path) {
    star::setNumThreads(1);
    LinesReader r;
    Impl& im = *r.impl_;
    im.ds = StarDataset::open(path, star::FileMode::READ_ONLY, rangedOpenOptions());
    if (!im.ds->has_layer(kLinesLayer)) return r;   // no lines → count()==0

    // Ellipsoid params from the header (fall back to IAU Mars defaults).
    auto hnum = [&](const char* key, double def) {
        std::string s = getStr(*im.ds, key);
        return s.empty() ? def : std::atof(s.c_str());
    };
    const double A = hnum("h.linesRadiusA", 3396190.0);
    const double C = hnum("h.linesRadiusC", 3376200.0);
    const double LON_S = 65535.0 / (2 * M_PI);   // must match the writer
    const double LAT_S = 65535.0 / M_PI;

    auto layer = im.ds->get_layer(kLinesLayer);
    auto getI16 = [&](const char* k) -> std::vector<int16_t> {
        try { auto a = layer->get<int16_t>(k); return std::vector<int16_t>(a.data().begin(), a.data().end()); }
        catch (...) { return {}; }
    };
    auto getU32 = [&](const char* k) -> std::vector<uint32_t> {
        try { auto a = layer->get<uint32_t>(k); return std::vector<uint32_t>(a.data().begin(), a.data().end()); }
        catch (...) { return {}; }
    };
    std::vector<int16_t> qlon = getI16("lines.qlon"), qlat = getI16("lines.qlat");
    im.voff = getU32("lines.voff");
    im.rangeStart = getU32("lines.rangeStart");
    im.rangeCount = getU32("lines.rangeCount");
    im.nLines = im.rangeStart.size();

    // Dequantize each vertex: global int16 → (lon,lat) → BCBF on the ellipsoid
    // surface (height dropped — the overview is on-surface).
    im.vxyz.resize(qlon.size() * 3);
    for (size_t vi = 0; vi < qlon.size(); ++vi) {
        double lon = qlon[vi] / LON_S;
        double lat = qlat[vi] / LAT_S;
        Vec3 p = utils::latLonToEcef(lat, lon, 0.0, A, C);
        im.vxyz[vi*3+0] = (float)p.x;
        im.vxyz[vi*3+1] = (float)p.y;
        im.vxyz[vi*3+2] = (float)p.z;
    }
    return r;
}

size_t LinesReader::count() const { return impl_->nLines; }
size_t LinesReader::vertexCount() const { return impl_->vxyz.size() / 3; }
void LinesReader::lineRange(size_t i, uint32_t& firstVertex, uint32_t& n) const {
    const Impl& im = *impl_;
    if (i + 1 < im.voff.size()) { firstVertex = im.voff[i]; n = im.voff[i+1] - im.voff[i]; }
    else { firstVertex = 0; n = 0; }
}
void LinesReader::linePointRange(size_t i, uint32_t& start, uint32_t& count) const {
    const Impl& im = *impl_;
    start = i < im.rangeStart.size() ? im.rangeStart[i] : 0;
    count = i < im.rangeCount.size() ? im.rangeCount[i] : 0;
}
const std::vector<float>& LinesReader::verticesXYZ() const { return impl_->vxyz; }

}  // namespace cnet
