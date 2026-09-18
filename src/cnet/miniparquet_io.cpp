#include "cnet/miniparquet_io.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "miniparquet.hpp"
#include "miniparquet_writer.hpp"

#ifdef MINISET_HAS_GDAL
#include <cpl_vsi.h>
#endif

namespace cnet {

namespace {

#ifdef MINISET_HAS_GDAL
// RangeReader backed by GDAL's VSI layer, so any GDAL-recognized path works:
// local files, /vsimem (tests), /vsicurl (HTTP), /vsis3 (S3). One VSILFILE for
// the reader's lifetime; each read_at is a seek + read (VSI handles ranged HTTP).
class VsiRangeReader : public mparq::RangeReader {
  public:
    explicit VsiRangeReader(const std::string& path) {
        m_fp = VSIFOpenL(path.c_str(), "rb");
    }
    ~VsiRangeReader() override { if (m_fp) VSIFCloseL(m_fp); }
    size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) override {
        out.assign(len, 0);
        if (!m_fp) { out.clear(); return 0; }
        VSIFSeekL(m_fp, static_cast<vsi_l_offset>(offset), SEEK_SET);
        size_t got = VSIFReadL(out.data(), 1, len, m_fp);
        out.resize(got);
        return got;
    }
    size_t size_or_unknown() override {
        if (!m_fp) return 0;
        VSIFSeekL(m_fp, 0, SEEK_END);
        vsi_l_offset end = VSIFTellL(m_fp);
        return static_cast<size_t>(end);
    }
    bool good() const override { return m_fp != nullptr; }

  private:
    VSILFILE* m_fp = nullptr;
};
#endif  // MINISET_HAS_GDAL

// Open a Reader for a path, preferring GDAL VSI when available (handles /vsimem
// and remote schemes); otherwise miniparquet's own local/HTTP backends.
mparq::Reader open_reader(const std::string& path) {
#ifdef MINISET_HAS_GDAL
    return mparq::Reader::fromReader(std::make_unique<VsiRangeReader>(path));
#else
    return mparq::Reader::open(path);
#endif
}

// The full cnet Parquet schema. Header + point columns repeat on every row.
// Names match the schema the writer emits (see encode_net below) exactly, including the
// lowercase / measure_-prefixed measure names.
const char* kHeaderCols[] = {"networkId", "targetName", "created",
                             "lastModified", "description", "userName"};
const char* kStringCols[] = {"id", "chooserName", "datetime",
                             "aprioriSurfPointSourceFile", "aprioriRadiusSourceFile",
                             "serialnumber", "measure_chooserName", "measure_datetime"};
const char* kIntCols[] = {"type", "editLock", "ignore", "jigsawRejected",
                          "referenceIndex", "aprioriSurfPointSource", "aprioriRadiusSource",
                          "measure_type", "measure_editLock", "measure_ignore",
                          "measure_jigsawRejected"};
const char* kDblCols[] = {"aprioriX", "aprioriY", "aprioriZ", "adjustedX", "adjustedY",
                          "adjustedZ", "sample", "line", "sampleResidual", "lineResidual",
                          "diameter", "apriorisample", "aprioriline", "samplesigma", "linesigma"};
const char* kListDblCols[] = {"aprioriCovar", "adjustedCovar", "measure_logValue"};
const char* kListIntCols[] = {"measure_logType"};

std::vector<std::string> all_schema_columns() {
    std::vector<std::string> v;
    for (auto c : kHeaderCols) v.push_back(c);
    for (auto c : kStringCols) v.push_back(c);
    for (auto c : kIntCols) v.push_back(c);
    for (auto c : kDblCols) v.push_back(c);
    for (auto c : kListDblCols) v.push_back(c);
    for (auto c : kListIntCols) v.push_back(c);
    return v;
}

// Assemble a ColumnBatch from decoded miniparquet columns. `cols` holds whatever
// columns were requested; absent columns leave the corresponding ColumnBatch
// vectors empty (control_net_from_columns tolerates short/absent columns).
ColumnBatch to_batch(std::map<std::string, mparq::ColumnData>& cols) {
    ColumnBatch b;

    auto has = [&](const char* n) { return cols.find(n) != cols.end(); };
    auto strc = [&](const char* n) -> std::vector<std::string>& { return cols[n].str; };
    auto intc = [&](const char* n) -> std::vector<int32_t>& { return cols[n].i32; };
    auto dblc = [&](const char* n) -> std::vector<double>& { return cols[n].f64; };
    auto pres = [&](const char* n) -> std::vector<uint8_t>& { return cols[n].present; };

    // Header: row 0 is authoritative (repeated on every row).
    if (has("networkId") && !strc("networkId").empty()) b.networkId = strc("networkId")[0];
    if (has("targetName") && !strc("targetName").empty()) b.targetName = strc("targetName")[0];
    if (has("created") && !strc("created").empty()) b.created = strc("created")[0];
    if (has("lastModified") && !strc("lastModified").empty()) b.lastModified = strc("lastModified")[0];
    if (has("description") && !strc("description").empty()) b.description = strc("description")[0];
    if (has("userName") && !strc("userName").empty()) b.userName = strc("userName")[0];

    // Point string columns.
    if (has("id")) b.id = std::move(strc("id"));
    if (has("chooserName")) b.chooserName = std::move(strc("chooserName"));
    if (has("datetime")) b.datetime = std::move(strc("datetime"));
    if (has("aprioriSurfPointSourceFile")) b.aprioriSurfPointSourceFile = std::move(strc("aprioriSurfPointSourceFile"));
    if (has("aprioriRadiusSourceFile")) b.aprioriRadiusSourceFile = std::move(strc("aprioriRadiusSourceFile"));

    // Point int columns (+ has* presence for the optional ones).
    if (has("type")) b.type = std::move(intc("type"));
    if (has("editLock")) { b.editLock = std::move(intc("editLock")); b.hasEditLock.assign(pres("editLock").begin(), pres("editLock").end()); }
    if (has("ignore")) { b.ignore = std::move(intc("ignore")); b.hasIgnore.assign(pres("ignore").begin(), pres("ignore").end()); }
    if (has("jigsawRejected")) { b.jigsawRejected = std::move(intc("jigsawRejected")); b.hasJigsawRejected.assign(pres("jigsawRejected").begin(), pres("jigsawRejected").end()); }
    if (has("referenceIndex")) { b.referenceIndex = std::move(intc("referenceIndex")); b.hasReferenceIndex.assign(pres("referenceIndex").begin(), pres("referenceIndex").end()); }
    if (has("aprioriSurfPointSource")) b.aprioriSurfPointSource = std::move(intc("aprioriSurfPointSource"));
    if (has("aprioriRadiusSource")) b.aprioriRadiusSource = std::move(intc("aprioriRadiusSource"));

    // Point double columns. aprioriX/Y/Z share one presence flag (hasApriori),
    // adjustedX/Y/Z share hasAdjusted — derive from the X column's presence.
    if (has("aprioriX")) { b.aprioriX = std::move(dblc("aprioriX"));
        b.hasApriori.assign(pres("aprioriX").begin(), pres("aprioriX").end()); }
    if (has("aprioriY")) b.aprioriY = std::move(dblc("aprioriY"));
    if (has("aprioriZ")) b.aprioriZ = std::move(dblc("aprioriZ"));
    if (has("adjustedX")) { b.adjustedX = std::move(dblc("adjustedX"));
        b.hasAdjusted.assign(pres("adjustedX").begin(), pres("adjustedX").end()); }
    if (has("adjustedY")) b.adjustedY = std::move(dblc("adjustedY"));
    if (has("adjustedZ")) b.adjustedZ = std::move(dblc("adjustedZ"));

    // Covariance list columns: flattened values + per-row length.
    if (has("aprioriCovar")) { b.aprioriCovar = std::move(dblc("aprioriCovar")); b.aprioriCovarLen = std::move(cols["aprioriCovar"].listLen); }
    if (has("adjustedCovar")) { b.adjustedCovar = std::move(dblc("adjustedCovar")); b.adjustedCovarLen = std::move(cols["adjustedCovar"].listLen); }

    // Measure string columns.
    if (has("serialnumber")) b.serialnumber = std::move(strc("serialnumber"));
    if (has("measure_chooserName")) b.measureChooserName = std::move(strc("measure_chooserName"));
    if (has("measure_datetime")) b.measureDatetime = std::move(strc("measure_datetime"));

    // Measure int columns (+ presence).
    if (has("measure_type")) b.measureType = std::move(intc("measure_type"));
    if (has("measure_editLock")) { b.measureEditLock = std::move(intc("measure_editLock")); b.hasMeasureEditLock.assign(pres("measure_editLock").begin(), pres("measure_editLock").end()); }
    if (has("measure_ignore")) { b.measureIgnore = std::move(intc("measure_ignore")); b.hasMeasureIgnore.assign(pres("measure_ignore").begin(), pres("measure_ignore").end()); }
    if (has("measure_jigsawRejected")) { b.measureJigsawRejected = std::move(intc("measure_jigsawRejected")); b.hasMeasureJigsawRejected.assign(pres("measure_jigsawRejected").begin(), pres("measure_jigsawRejected").end()); }

    // Measure double columns (+ presence).
    auto dblWithPresence = [&](const char* n, std::vector<double>& val, std::vector<double>& hasv) {
        if (!has(n)) return;
        val = std::move(dblc(n));
        hasv.assign(pres(n).begin(), pres(n).end());
    };
    dblWithPresence("sample", b.sample, b.hasSample);
    dblWithPresence("line", b.line, b.hasLine);
    dblWithPresence("sampleResidual", b.sampleResidual, b.hasSampleResidual);
    dblWithPresence("lineResidual", b.lineResidual, b.hasLineResidual);
    dblWithPresence("diameter", b.diameter, b.hasDiameter);
    dblWithPresence("apriorisample", b.aprioriSample, b.hasAprioriSample);
    dblWithPresence("aprioriline", b.aprioriLine, b.hasAprioriLine);
    dblWithPresence("samplesigma", b.sampleSigma, b.hasSampleSigma);
    dblWithPresence("linesigma", b.lineSigma, b.hasLineSigma);

    // Measure log list columns.
    if (has("measure_logType")) { b.measureLogType = std::move(intc("measure_logType")); b.measureLogLen = std::move(cols["measure_logType"].listLen); }
    if (has("measure_logValue")) b.measureLogValue = std::move(dblc("measure_logValue"));

    return b;
}

// Append the kept rows (keep[i] true) of one row group's column map `src` onto
// accumulator `dst`. Scalar column value vectors are row-aligned (a placeholder
// entry per row); list columns carry per-row listLen + flattened values, so we
// copy each kept row's slice. dst starts empty (per-column created on first RG).
void append_filtered(std::map<std::string, mparq::ColumnData>& dst,
                     const std::map<std::string, mparq::ColumnData>& src,
                     const std::vector<char>& keep) {
    for (const auto& kv : src) {
        const std::string& name = kv.first;
        const mparq::ColumnData& s = kv.second;
        mparq::ColumnData& d = dst[name];
        d.type = s.type;
        d.is_list = s.is_list;
        if (!s.is_list) {
            // Row-aligned scalar values + presence.
            for (size_t i = 0; i < keep.size(); ++i) {
                if (!keep[i]) continue;
                d.present.push_back(i < s.present.size() ? s.present[i] : 1);
                if (s.type == mparq::ColumnData::Int32) d.i32.push_back(i < s.i32.size() ? s.i32[i] : 0);
                else if (s.type == mparq::ColumnData::Double) d.f64.push_back(i < s.f64.size() ? s.f64[i] : 0.0);
                else d.str.push_back(i < s.str.size() ? s.str[i] : std::string());
            }
        } else {
            // List: walk rows, tracking a cursor into the flattened values.
            size_t cursor = 0;
            for (size_t i = 0; i < s.listLen.size(); ++i) {
                int32_t n = s.listLen[i];
                if (i < keep.size() && keep[i]) {
                    d.listLen.push_back(n);
                    d.present.push_back(i < s.present.size() ? s.present[i] : 1);
                    for (int32_t k = 0; k < n; ++k) {
                        if (s.type == mparq::ColumnData::Int32) d.i32.push_back(cursor + k < s.i32.size() ? s.i32[cursor + k] : 0);
                        else d.f64.push_back(cursor + k < s.f64.size() ? s.f64[cursor + k] : 0.0);
                    }
                }
                cursor += n;
            }
        }
    }
}

}  // namespace

ControlNet read_where_miniparquet(const std::string& path, const std::string& column,
                                  double lo, double hi) {
    mparq::Reader r = open_reader(path);
    if (!r.hasColumn(column))
        throw std::runtime_error("read_where: unknown column '" + column + "'");

    std::vector<std::string> all = all_schema_columns();
    // Prune row groups by the query column's statistics (skips whole groups).
    std::vector<size_t> candidates = r.pruneRowGroupsDouble(column, lo, hi);

    std::map<std::string, mparq::ColumnData> acc;
    for (size_t g : candidates) {
        std::map<std::string, mparq::ColumnData> rg = r.readRowGroup(g, all);
        const mparq::ColumnData& q = rg[column];
        size_t rows = q.is_list ? q.listLen.size() : q.present.size();
        // Exact filter: keep a row iff the column is present and within [lo, hi].
        std::vector<char> keep(rows, 0);
        for (size_t i = 0; i < rows; ++i) {
            bool present = i < q.present.size() ? q.present[i] != 0 : true;
            if (!present) continue;
            double v = 0;
            if (q.type == mparq::ColumnData::Double) v = i < q.f64.size() ? q.f64[i] : 0.0;
            else if (q.type == mparq::ColumnData::Int32) v = i < q.i32.size() ? (double)q.i32[i] : 0.0;
            else continue;  // string column range not supported here
            if (v >= lo && v <= hi) keep[i] = 1;
        }
        append_filtered(acc, rg, keep);
    }

    ColumnBatch b = to_batch(acc);
    return control_net_from_columns(b);
}

ControlNet read_control_net_miniparquet(const std::string& path) {
    mparq::Reader r = open_reader(path);
    auto cols = r.readColumns(all_schema_columns());
    ColumnBatch b = to_batch(cols);
    return control_net_from_columns(b);
}

ControlNet read_control_net_miniparquet_bytes(const std::vector<uint8_t>& bytes) {
    mparq::Reader r = mparq::Reader::openBytes(bytes);
    auto cols = r.readColumns(all_schema_columns());
    ColumnBatch b = to_batch(cols);
    return control_net_from_columns(b);
}

ColumnBatch read_columns_miniparquet(const std::string& path,
                                     const std::vector<std::string>& columns,
                                     size_t rowStart, size_t rowCount) {
    mparq::Reader r = open_reader(path);
    // `id` is always needed to group rows into points; force it in.
    std::vector<std::string> req = columns.empty() ? all_schema_columns() : columns;
    if (!columns.empty()) {
        bool haveId = false;
        for (const auto& c : req) if (c == "id") { haveId = true; break; }
        if (!haveId) req.push_back("id");
    }
    size_t n = (rowCount == 0) ? r.numRows() : rowCount;
    auto cols = r.readColumns(req, rowStart, n);
    return to_batch(cols);
}

bool ParquetShape::hasColumn(const std::string& name) const {
    for (const auto& c : columns) if (c == name) return true;
    return false;
}

ParquetShape read_shape_miniparquet(const std::string& path) {
    mparq::Reader r = open_reader(path);
    ParquetShape shape;
    shape.rowCount = r.numRows();

    // Record which physical columns exist (for hasColumn / projection checks).
    for (const auto& c : all_schema_columns())
        if (r.hasColumn(c)) shape.columns.push_back(c);

    // Header from row 0's repeated columns (single narrow read).
    auto hdrCols = r.readColumns({"networkId", "targetName", "created",
                                  "lastModified", "description", "userName"}, 0, 1);
    auto first = [&](const char* n) -> std::string {
        auto it = hdrCols.find(n);
        return (it != hdrCols.end() && !it->second.str.empty()) ? it->second.str[0] : std::string();
    };
    shape.header.networkId = first("networkId");
    shape.header.targetName = first("targetName");
    shape.header.created = first("created");
    shape.header.lastModified = first("lastModified");
    shape.header.description = first("description");
    shape.header.userName = first("userName");

    // Point index: read ONLY the `id` column, one ROW GROUP at a time, so peak
    // memory is a single group's ids (not the whole column) — this scales to
    // GB-scale files. Group contiguous equal ids into points across group edges.
    bool have = false;
    std::string current;
    uint32_t globalRow = 0;
    for (size_t g = 0; g < r.numRowGroups(); ++g) {
        auto rg = r.readRowGroup(g, {"id"});
        const std::vector<std::string>& ids = rg["id"].str;
        for (size_t i = 0; i < ids.size(); ++i, ++globalRow) {
            if (!have || ids[i] != current) {
                have = true;
                current = ids[i];
                shape.ids.push_back(ids[i]);
                shape.pointFirstRow.push_back(globalRow);
            }
        }
    }
    shape.pointFirstRow.push_back(static_cast<uint32_t>(shape.rowCount));
    return shape;
}

// ===========================================================================
// Writer bridge: ControlNet -> denormalized WriteColumns -> Parquet bytes.
// One row per measure; header + point columns repeated on each of a point's
// rows; a point with zero measures emits a single row with an empty
// serialnumber (the reader's zero-measure sentinel). Presence and list layout
// mirror the (now-removed) GDAL writer exactly, so output round-trips and is
// readable by Arrow/GDAL.
// ===========================================================================
namespace {

using mparq::PhysType;
using mparq::WriteColumn;

// Accumulates one column's rows: values pushed only when present, presence
// tracked per row. Keeps value/presence vectors aligned with the writer's model.
struct ColBuilder {
    WriteColumn col;
    explicit ColBuilder(const char* name, PhysType t, bool list = false) {
        col.name = name; col.type = t; col.is_list = list;
    }
    void pushStr(const std::string& v, bool present) {
        col.present.push_back(present ? 1 : 0);
        if (present) col.str.push_back(v);
    }
    void pushStrAlways(const std::string& v) { col.present.push_back(1); col.str.push_back(v); }
    void pushInt(int32_t v, bool present) {
        col.present.push_back(present ? 1 : 0);
        if (present) col.i32.push_back(v);
    }
    void pushIntAlways(int32_t v) { col.present.push_back(1); col.i32.push_back(v); }
    void pushDbl(double v, bool present) {
        col.present.push_back(present ? 1 : 0);
        if (present) col.f64.push_back(v);
    }
};

// The 44-column schema, in canonical order (shared by every row group).
std::vector<mparq::ColumnSchema> cnet_schema() {
    return {
        {"networkId", PhysType::ByteArray, false}, {"targetName", PhysType::ByteArray, false},
        {"created", PhysType::ByteArray, false}, {"lastModified", PhysType::ByteArray, false},
        {"description", PhysType::ByteArray, false}, {"userName", PhysType::ByteArray, false},
        {"id", PhysType::ByteArray, false}, {"type", PhysType::Int32, false},
        {"chooserName", PhysType::ByteArray, false}, {"datetime", PhysType::ByteArray, false},
        {"editLock", PhysType::Int32, false}, {"ignore", PhysType::Int32, false},
        {"jigsawRejected", PhysType::Int32, false}, {"referenceIndex", PhysType::Int32, false},
        {"aprioriSurfPointSource", PhysType::Int32, false}, {"aprioriSurfPointSourceFile", PhysType::ByteArray, false},
        {"aprioriRadiusSource", PhysType::Int32, false}, {"aprioriRadiusSourceFile", PhysType::ByteArray, false},
        {"aprioriX", PhysType::Double, false}, {"aprioriY", PhysType::Double, false}, {"aprioriZ", PhysType::Double, false},
        {"aprioriCovar", PhysType::Double, true},
        {"adjustedX", PhysType::Double, false}, {"adjustedY", PhysType::Double, false}, {"adjustedZ", PhysType::Double, false},
        {"adjustedCovar", PhysType::Double, true},
        {"serialnumber", PhysType::ByteArray, false}, {"measure_type", PhysType::Int32, false},
        {"sample", PhysType::Double, false}, {"line", PhysType::Double, false},
        {"sampleResidual", PhysType::Double, false}, {"lineResidual", PhysType::Double, false},
        {"measure_chooserName", PhysType::ByteArray, false}, {"measure_datetime", PhysType::ByteArray, false},
        {"measure_editLock", PhysType::Int32, false}, {"measure_ignore", PhysType::Int32, false},
        {"measure_jigsawRejected", PhysType::Int32, false}, {"diameter", PhysType::Double, false},
        {"apriorisample", PhysType::Double, false}, {"aprioriline", PhysType::Double, false},
        {"samplesigma", PhysType::Double, false}, {"linesigma", PhysType::Double, false},
        {"measure_logType", PhysType::Int32, true}, {"measure_logValue", PhysType::Double, true},
    };
}

// Fill one row group's WriteColumns for points [pStart, pEnd). Points are never
// split across row groups, so covariance/log CSR semantics stay intact. Returns
// the row count (measures + zero-measure sentinel rows) in this batch.
size_t fill_batch(const ControlNet& net, size_t pStart, size_t pEnd,
                  std::vector<WriteColumn>& outCols) {
    ColBuilder h_networkId("networkId", PhysType::ByteArray);
    ColBuilder h_targetName("targetName", PhysType::ByteArray);
    ColBuilder h_created("created", PhysType::ByteArray);
    ColBuilder h_lastModified("lastModified", PhysType::ByteArray);
    ColBuilder h_description("description", PhysType::ByteArray);
    ColBuilder h_userName("userName", PhysType::ByteArray);
    // Point columns.
    ColBuilder p_id("id", PhysType::ByteArray);
    ColBuilder p_type("type", PhysType::Int32);
    ColBuilder p_chooserName("chooserName", PhysType::ByteArray);
    ColBuilder p_datetime("datetime", PhysType::ByteArray);
    ColBuilder p_editLock("editLock", PhysType::Int32);
    ColBuilder p_ignore("ignore", PhysType::Int32);
    ColBuilder p_jigsawRejected("jigsawRejected", PhysType::Int32);
    ColBuilder p_referenceIndex("referenceIndex", PhysType::Int32);
    ColBuilder p_aprSurfSrc("aprioriSurfPointSource", PhysType::Int32);
    ColBuilder p_aprSurfFile("aprioriSurfPointSourceFile", PhysType::ByteArray);
    ColBuilder p_aprRadSrc("aprioriRadiusSource", PhysType::Int32);
    ColBuilder p_aprRadFile("aprioriRadiusSourceFile", PhysType::ByteArray);
    ColBuilder p_aprX("aprioriX", PhysType::Double);
    ColBuilder p_aprY("aprioriY", PhysType::Double);
    ColBuilder p_aprZ("aprioriZ", PhysType::Double);
    ColBuilder p_aprCovar("aprioriCovar", PhysType::Double, /*list*/ true);
    ColBuilder p_adjX("adjustedX", PhysType::Double);
    ColBuilder p_adjY("adjustedY", PhysType::Double);
    ColBuilder p_adjZ("adjustedZ", PhysType::Double);
    ColBuilder p_adjCovar("adjustedCovar", PhysType::Double, /*list*/ true);
    // Measure columns.
    ColBuilder m_serial("serialnumber", PhysType::ByteArray);
    ColBuilder m_type("measure_type", PhysType::Int32);
    ColBuilder m_sample("sample", PhysType::Double);
    ColBuilder m_line("line", PhysType::Double);
    ColBuilder m_sampleResid("sampleResidual", PhysType::Double);
    ColBuilder m_lineResid("lineResidual", PhysType::Double);
    ColBuilder m_chooser("measure_chooserName", PhysType::ByteArray);
    ColBuilder m_datetime("measure_datetime", PhysType::ByteArray);
    ColBuilder m_editLock("measure_editLock", PhysType::Int32);
    ColBuilder m_ignore("measure_ignore", PhysType::Int32);
    ColBuilder m_jigsaw("measure_jigsawRejected", PhysType::Int32);
    ColBuilder m_diameter("diameter", PhysType::Double);
    ColBuilder m_aprSample("apriorisample", PhysType::Double);
    ColBuilder m_aprLine("aprioriline", PhysType::Double);
    ColBuilder m_sampleSigma("samplesigma", PhysType::Double);
    ColBuilder m_lineSigma("linesigma", PhysType::Double);
    ColBuilder m_logType("measure_logType", PhysType::Int32, /*list*/ true);
    ColBuilder m_logValue("measure_logValue", PhysType::Double, /*list*/ true);

    size_t numRows = 0;
    auto emitPointCols = [&](size_t p) {
        h_networkId.pushStr(net.header.networkId, !net.header.networkId.empty());
        h_targetName.pushStr(net.header.targetName, !net.header.targetName.empty());
        h_created.pushStr(net.header.created, !net.header.created.empty());
        h_lastModified.pushStr(net.header.lastModified, !net.header.lastModified.empty());
        h_description.pushStr(net.header.description, !net.header.description.empty());
        h_userName.pushStr(net.header.userName, !net.header.userName.empty());

        p_id.pushStr(net.pointId[p], !net.pointId[p].empty());
        p_type.pushIntAlways(static_cast<int32_t>(net.pointType[p]));
        p_chooserName.pushStr(net.chooserName[p], !net.chooserName[p].empty());
        p_datetime.pushStr(net.datetime[p], !net.datetime[p].empty());
        p_editLock.pushInt(net.editLock[p] ? 1 : 0, net.hasEditLock[p] != 0);
        p_ignore.pushInt(net.ignore[p] ? 1 : 0, net.hasIgnore[p] != 0);
        p_jigsawRejected.pushInt(net.jigsawRejected[p] ? 1 : 0, net.hasJigsawRejected[p] != 0);
        p_referenceIndex.pushInt(net.referenceIndex[p], net.hasReferenceIndex[p] != 0);
        p_aprSurfSrc.pushIntAlways(static_cast<int32_t>(net.aprioriSurfPointSource[p]));
        p_aprSurfFile.pushStr(net.aprioriSurfPointSourceFile[p], !net.aprioriSurfPointSourceFile[p].empty());
        p_aprRadSrc.pushIntAlways(static_cast<int32_t>(net.aprioriRadiusSource[p]));
        p_aprRadFile.pushStr(net.aprioriRadiusSourceFile[p], !net.aprioriRadiusSourceFile[p].empty());
        bool ha = net.hasApriori[p] != 0;
        p_aprX.pushDbl(net.aprioriX[p], ha); p_aprY.pushDbl(net.aprioriY[p], ha); p_aprZ.pushDbl(net.aprioriZ[p], ha);
        bool hj = net.hasAdjusted[p] != 0;
        p_adjX.pushDbl(net.adjustedX[p], hj); p_adjY.pushDbl(net.adjustedY[p], hj); p_adjZ.pushDbl(net.adjustedZ[p], hj);
        // Covariance lists: only on the point's first row; empty on continuations.
        // Emitted per-row below in the row loop.
    };

    // For list columns, listLen is per row. Covariances appear only on the
    // point's first measure row (or its single sentinel row).
    for (size_t p = pStart; p < pEnd; ++p) {
        uint32_t mstart = net.measureStart[p], mcount = net.measureCount[p];
        uint32_t ab = net.aprioriCovarOffset[p], ae = net.aprioriCovarOffset[p + 1];
        uint32_t db = net.adjustedCovarOffset[p], de = net.adjustedCovarOffset[p + 1];

        auto emitCovarForRow = [&](bool firstRow) {
            if (firstRow) {
                p_aprCovar.col.listLen.push_back(static_cast<int32_t>(ae - ab));
                for (uint32_t k = ab; k < ae; ++k) p_aprCovar.col.f64.push_back(net.aprioriCovar[k]);
                p_adjCovar.col.listLen.push_back(static_cast<int32_t>(de - db));
                for (uint32_t k = db; k < de; ++k) p_adjCovar.col.f64.push_back(net.adjustedCovar[k]);
            } else {
                p_aprCovar.col.listLen.push_back(0);
                p_adjCovar.col.listLen.push_back(0);
            }
            // List columns are always "present" (an empty list, not null).
            p_aprCovar.col.present.push_back(1);
            p_adjCovar.col.present.push_back(1);
        };
        auto emitMeasure = [&](uint32_t m) {
            m_serial.pushStr(net.serialNumber[m], !net.serialNumber[m].empty());
            m_type.pushIntAlways(static_cast<int32_t>(net.measureType[m]));
            m_sample.pushDbl(net.sample[m], net.hasSample[m] != 0);
            m_line.pushDbl(net.line[m], net.hasLine[m] != 0);
            m_sampleResid.pushDbl(net.sampleResidual[m], net.hasSampleResidual[m] != 0);
            m_lineResid.pushDbl(net.lineResidual[m], net.hasLineResidual[m] != 0);
            m_chooser.pushStr(net.measureChooserName[m], !net.measureChooserName[m].empty());
            m_datetime.pushStr(net.measureDatetime[m], !net.measureDatetime[m].empty());
            m_editLock.pushInt(net.measureEditLock[m] ? 1 : 0, net.hasMeasureEditLock[m] != 0);
            m_ignore.pushInt(net.measureIgnore[m] ? 1 : 0, net.hasMeasureIgnore[m] != 0);
            m_jigsaw.pushInt(net.measureJigsawRejected[m] ? 1 : 0, net.hasMeasureJigsawRejected[m] != 0);
            m_diameter.pushDbl(net.diameter[m], net.hasDiameter[m] != 0);
            m_aprSample.pushDbl(net.aprioriSample[m], net.hasAprioriSample[m] != 0);
            m_aprLine.pushDbl(net.aprioriLine[m], net.hasAprioriLine[m] != 0);
            m_sampleSigma.pushDbl(net.sampleSigma[m], net.hasSampleSigma[m] != 0);
            m_lineSigma.pushDbl(net.lineSigma[m], net.hasLineSigma[m] != 0);
            uint32_t lb = net.measureLogOffset[m], le = net.measureLogOffset[m + 1];
            m_logType.col.listLen.push_back(static_cast<int32_t>(le - lb));
            m_logValue.col.listLen.push_back(static_cast<int32_t>(le - lb));
            m_logType.col.present.push_back(1);
            m_logValue.col.present.push_back(1);
            for (uint32_t k = lb; k < le; ++k) {
                m_logType.col.i32.push_back(net.measureLogType[k]);
                m_logValue.col.f64.push_back(net.measureLogValue[k]);
            }
        };
        auto emitEmptyMeasure = [&]() {
            // Zero-measure sentinel row: empty serialnumber (null), zero-value
            // measure fields all absent, empty log lists.
            m_serial.pushStr(std::string(), false);
            m_type.pushInt(0, false);
            m_sample.pushDbl(0, false); m_line.pushDbl(0, false);
            m_sampleResid.pushDbl(0, false); m_lineResid.pushDbl(0, false);
            m_chooser.pushStr(std::string(), false); m_datetime.pushStr(std::string(), false);
            m_editLock.pushInt(0, false); m_ignore.pushInt(0, false); m_jigsaw.pushInt(0, false);
            m_diameter.pushDbl(0, false); m_aprSample.pushDbl(0, false); m_aprLine.pushDbl(0, false);
            m_sampleSigma.pushDbl(0, false); m_lineSigma.pushDbl(0, false);
            m_logType.col.listLen.push_back(0); m_logValue.col.listLen.push_back(0);
            m_logType.col.present.push_back(1); m_logValue.col.present.push_back(1);
        };

        if (mcount == 0) {
            emitPointCols(p);
            emitCovarForRow(true);
            emitEmptyMeasure();
            ++numRows;
        } else {
            for (uint32_t m = 0; m < mcount; ++m) {
                emitPointCols(p);
                emitCovarForRow(m == 0);
                emitMeasure(mstart + m);
                ++numRows;
            }
        }
    }

    outCols = {
        std::move(h_networkId.col), std::move(h_targetName.col), std::move(h_created.col),
        std::move(h_lastModified.col), std::move(h_description.col), std::move(h_userName.col),
        std::move(p_id.col), std::move(p_type.col), std::move(p_chooserName.col), std::move(p_datetime.col),
        std::move(p_editLock.col), std::move(p_ignore.col), std::move(p_jigsawRejected.col),
        std::move(p_referenceIndex.col), std::move(p_aprSurfSrc.col), std::move(p_aprSurfFile.col),
        std::move(p_aprRadSrc.col), std::move(p_aprRadFile.col), std::move(p_aprX.col), std::move(p_aprY.col),
        std::move(p_aprZ.col), std::move(p_aprCovar.col), std::move(p_adjX.col), std::move(p_adjY.col),
        std::move(p_adjZ.col), std::move(p_adjCovar.col),
        std::move(m_serial.col), std::move(m_type.col), std::move(m_sample.col), std::move(m_line.col),
        std::move(m_sampleResid.col), std::move(m_lineResid.col), std::move(m_chooser.col), std::move(m_datetime.col),
        std::move(m_editLock.col), std::move(m_ignore.col), std::move(m_jigsaw.col), std::move(m_diameter.col),
        std::move(m_aprSample.col), std::move(m_aprLine.col), std::move(m_sampleSigma.col), std::move(m_lineSigma.col),
        std::move(m_logType.col), std::move(m_logValue.col),
    };
    return numRows;
}

// Choose a point batch size so each row group holds roughly this many rows
// (points never split). Streams row groups so peak RAM is one batch, not the
// net. Overridable via MINISET_CNET_ROWGROUP_ROWS (mainly for tests, to force
// many small groups and exercise statistics-based pruning).
size_t row_group_target_rows() {
    if (const char* e = std::getenv("MINISET_CNET_ROWGROUP_ROWS")) {
        long v = std::atol(e);
        if (v > 0) return static_cast<size_t>(v);
    }
    return 200000;
}

// Translate the public compression enum to writer options.
mparq::WriterOptions writer_opts(ParquetCompression codec) {
    mparq::WriterOptions o;
    o.codec = (codec == ParquetCompression::Gzip) ? mparq::Codec::Gzip : mparq::Codec::Uncompressed;
    return o;
}

void stream_net(const ControlNet& net, mparq::ByteSink& sink, ParquetCompression codec) {
    mparq::StreamWriter w(sink, cnet_schema(), writer_opts(codec));
    const size_t target = row_group_target_rows();
    size_t p = 0, nPts = net.numPoints();
    while (p < nPts) {
        // Grow the batch point-by-point until it reaches the target row count.
        size_t pEnd = p, rows = 0;
        while (pEnd < nPts && rows < target) {
            uint32_t mc = net.measureCount[pEnd];
            rows += (mc == 0 ? 1 : mc);
            ++pEnd;
        }
        if (pEnd == p) pEnd = p + 1;  // always make progress
        std::vector<WriteColumn> cols;
        size_t batchRows = fill_batch(net, p, pEnd, cols);
        w.writeRowGroup(cols, batchRows);
        p = pEnd;
    }
    w.finish();
}

#ifdef MINISET_HAS_GDAL
// ByteSink writing through GDAL VSI (so /vsimem and remote-write schemes work).
class VsiByteSink : public mparq::ByteSink {
  public:
    explicit VsiByteSink(const std::string& path) {
        m_fp = VSIFOpenL(path.c_str(), "wb");
        if (m_fp == nullptr) throw std::runtime_error("Cannot create Parquet file: " + path);
    }
    ~VsiByteSink() override { if (m_fp) VSIFCloseL(m_fp); }
    void write(const char* data, size_t len) override {
        if (len) VSIFWriteL(data, 1, len, m_fp);
        m_pos += len;
    }
    size_t tell() const override { return m_pos; }
    using mparq::ByteSink::write;

  private:
    VSILFILE* m_fp = nullptr;
    size_t m_pos = 0;
};
#endif

// Open a ByteSink for a path (VSI when GDAL present, else a file stream).
std::unique_ptr<mparq::ByteSink> open_sink(const std::string& path) {
#ifdef MINISET_HAS_GDAL
    return std::make_unique<VsiByteSink>(path);
#else
    return std::make_unique<mparq::FileSink>(path);
#endif
}

}  // namespace

std::vector<uint8_t> write_control_net_miniparquet_bytes(const ControlNet& net,
                                                         ParquetCompression codec) {
    mparq::VectorSink sink;
    stream_net(net, sink, codec);
    return sink.take();
}

void write_control_net_miniparquet(const ControlNet& net, const std::string& path,
                                   ParquetCompression codec) {
    auto sink = open_sink(path);   // streams row groups straight to disk (bounded RAM)
    stream_net(net, *sink, codec);
}

// ---- Incremental streaming writer -----------------------------------------
struct ControlNetParquetWriter::Impl {
    std::unique_ptr<mparq::ByteSink> sink;
    std::unique_ptr<mparq::StreamWriter> writer;
    bool finished = false;
    Impl(const std::string& path, ParquetCompression codec)
        : sink(open_sink(path)),
          writer(std::make_unique<mparq::StreamWriter>(*sink, cnet_schema(), writer_opts(codec))) {}
};

ControlNetParquetWriter::ControlNetParquetWriter(const std::string& path, ParquetCompression codec)
    : impl_(std::make_unique<Impl>(path, codec)) {}
ControlNetParquetWriter::~ControlNetParquetWriter() {
    // Best-effort finalize if the caller forgot (keeps the file valid).
    if (impl_ && impl_->writer && !impl_->finished) {
        try { impl_->writer->finish(); } catch (...) {}
    }
}
ControlNetParquetWriter::ControlNetParquetWriter(ControlNetParquetWriter&&) noexcept = default;
ControlNetParquetWriter& ControlNetParquetWriter::operator=(ControlNetParquetWriter&&) noexcept = default;

void ControlNetParquetWriter::writeBatch(const ControlNet& batch) {
    if (batch.numPoints() == 0) return;
    std::vector<WriteColumn> cols;
    size_t rows = fill_batch(batch, 0, batch.numPoints(), cols);
    impl_->writer->writeRowGroup(cols, rows);
}

void ControlNetParquetWriter::finish() {
    if (!impl_->finished) { impl_->writer->finish(); impl_->finished = true; }
}

}  // namespace cnet
