#ifndef MINISET_CNET_CONTROL_NET_READER_HPP
#define MINISET_CNET_CONTROL_NET_READER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cnet/control_net.hpp"

namespace cnet {

/// Options controlling what a lazy read pulls from disk.
struct ReadOptions {
    /// If non-empty, only these Parquet columns are read (column projection via
    /// GDAL SetIgnoredFields). Heavy columns (covariances, logs) can be skipped.
    std::vector<std::string> columns;
    /// If non-empty, an OGR attribute filter (e.g. "measure_ignore = 0"), pushed
    /// down to Parquet row-group statistics.
    std::string filter;
};

/// Lazy, cloud-optimized reader for control networks.
///
/// `open()` reads only *shape metadata* — the row count (from Parquet metadata,
/// no scan) and a point->firstRow index (built by reading just the `id` column) —
/// then data is loaded on demand via readPoints()/readMeasures(). The Parquet is
/// one row per measure; a point is a contiguous run of rows sharing `id`.
/// Random access uses GDAL's SetNextByIndex, which jumps to the containing row
/// group, so reading a slice of a GB-scale file touches only that row group.
///
/// Backed by the GDAL OGR Parquet driver in both native and WASM builds, so the
/// API is identical. Protobuf `.net` is not seekable (single stream): for `.net`
/// paths, supportsLazy() is false and the slice methods read the whole file.
class ControlNetReader {
  public:
    ControlNetReader();
    ~ControlNetReader();
    ControlNetReader(ControlNetReader&&) noexcept;
    ControlNetReader& operator=(ControlNetReader&&) noexcept;
    ControlNetReader(const ControlNetReader&) = delete;
    ControlNetReader& operator=(const ControlNetReader&) = delete;

    /// Open a control network and read its shape. `path` may be /vsimem etc.
    /// @throws std::runtime_error on open/format error.
    static ControlNetReader open(const std::string& path);

    /// True when the backing format supports true partial reads (Parquet).
    bool supportsLazy() const;

    // ---- Shape (no measure data loaded) -----------------------------------
    size_t numPoints() const;
    /// Total Parquet rows = measures + one sentinel row per zero-measure point.
    /// (Exact measure count requires a data pass; readAll().numMeasures() gives it.)
    size_t numRows() const;
    const NetworkHeader& header() const;
    bool hasColumn(const std::string& name) const;
    const std::string& pointId(size_t i) const;
    /// Row range [firstRow, firstRow+count) of point i's measures.
    void measureRange(size_t i, uint32_t& firstRow, uint32_t& count) const;

    // ---- Lazy loads (return a small ControlNet holding just the slice) -----
    /// Points [start, start+count); always whole points (snaps to id runs).
    ControlNet readPoints(size_t start, size_t count, const ReadOptions& opts = {}) const;
    /// Raw measure-row window [rowStart, rowStart+rowCount). May split points.
    ControlNet readMeasures(size_t rowStart, size_t rowCount, const ReadOptions& opts = {}) const;
    /// The whole network (eager).
    ControlNet readAll(const ReadOptions& opts = {}) const;

    /// Range query: return the measures (as a ControlNet) whose numeric column
    /// `column` (e.g. "sample", "line", "sampleResidual", "aprioriX") falls in
    /// [lo, hi]. Uses Parquet row-group statistics to skip whole row groups that
    /// cannot match (so a GB-scale file touches only candidate groups), then
    /// exact-filters the survivors. Rows are grouped into points by `id`; a
    /// returned point contains only its matching measures. Parquet only — for a
    /// non-lazy `.net`, throws. `column` must be a scalar (non-list) column.
    ControlNet readWhere(const std::string& column, double lo, double hi,
                         const ReadOptions& opts = {}) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnet

#endif  // MINISET_CNET_CONTROL_NET_READER_HPP
