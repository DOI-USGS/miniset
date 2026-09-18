#ifndef MINISET_CNET_MINIPARQUET_IO_HPP
#define MINISET_CNET_MINIPARQUET_IO_HPP

// Control-network Parquet reading via the vendored header-only reader
// (external/miniparquet.hpp) — no GDAL/Arrow, no hyparquet. Portable across the
// native and WASM builds. Decodes the cnet Parquet schema (one row per measure)
// into a ColumnBatch and assembles it with control_net_from_columns, the same
// format-agnostic path the WASM JS reader used to feed.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cnet/column_source.hpp"
#include "cnet/control_net.hpp"

namespace cnet {

/// Read an entire control-network Parquet file into a ControlNet. `path` may be
/// local or, if miniparquet is built with HTTP support, a remote URL.
ControlNet read_control_net_miniparquet(const std::string& path);

/// Read a control-network Parquet from an in-memory byte buffer (WASM / tests).
ControlNet read_control_net_miniparquet_bytes(const std::vector<uint8_t>& bytes);

/// Page-compression codec for Parquet output. UNCOMPRESSED is the default and
/// universally readable with no deps; GZIP needs zlib (MINIPARQUET_ENABLE_ZLIB)
/// and yields much smaller files (control-net columns compress well). Both remain
/// readable by Arrow/GDAL/DuckDB and by miniparquet's own reader.
enum class ParquetCompression { None, Gzip };

/// Write a control network to a Parquet file using the header-only writer (no
/// GDAL/Arrow). Emits the same denormalized schema the reader expects: one row
/// per measure, header+point columns repeated per row, grouped by `id`.
void write_control_net_miniparquet(const ControlNet& net, const std::string& path,
                                   ParquetCompression codec = ParquetCompression::None);

/// Serialize a control network to Parquet bytes (WASM / tests).
std::vector<uint8_t> write_control_net_miniparquet_bytes(
    const ControlNet& net, ParquetCompression codec = ParquetCompression::None);

/// Incremental Parquet writer: stream a large control network to disk one
/// point-batch at a time, so peak memory is one batch — never the whole net.
/// Each writeBatch() call emits one row group (points are never split). Use with
/// ControlNetReader::readPoints() windows for a bounded reader→writer pipeline.
class ControlNetParquetWriter {
  public:
    explicit ControlNetParquetWriter(const std::string& path,
                                     ParquetCompression codec = ParquetCompression::None);
    ~ControlNetParquetWriter();
    ControlNetParquetWriter(ControlNetParquetWriter&&) noexcept;
    ControlNetParquetWriter& operator=(ControlNetParquetWriter&&) noexcept;
    ControlNetParquetWriter(const ControlNetParquetWriter&) = delete;
    ControlNetParquetWriter& operator=(const ControlNetParquetWriter&) = delete;

    /// Append `batch` (a small ControlNet holding a run of whole points) as one
    /// row group. The batch's header is written into the file's metadata via the
    /// per-row repeated header columns (first batch's header is authoritative).
    void writeBatch(const ControlNet& batch);
    /// Write the footer and close. Must be called exactly once when done.
    void finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Build a denormalized ColumnBatch for the row window [rowStart, rowStart+
/// rowCount) of a Parquet file, reading only `columns` if non-empty (projection).
/// Used by the lazy reader; rowCount == 0 means "all rows from rowStart".
ColumnBatch read_columns_miniparquet(const std::string& path,
                                     const std::vector<std::string>& columns,
                                     size_t rowStart, size_t rowCount);

/// Shape of a Parquet control network, read cheaply (footer + the `id` column
/// only — no measure/covariance data). Feeds the lazy ControlNetReader's
/// up-front index.
struct ParquetShape {
    NetworkHeader header;
    size_t rowCount = 0;               // total measure rows
    std::vector<std::string> ids;      // one per point (contiguous-id runs)
    std::vector<uint32_t> pointFirstRow;  // size numPoints + 1 (CSR over rows)
    bool hasColumn(const std::string& name) const;
    std::vector<std::string> columns;  // physical column names present
};

ParquetShape read_shape_miniparquet(const std::string& path);

/// Range query over a Parquet control net: assemble a ControlNet from the rows
/// whose scalar column `column` is within [lo, hi]. Prunes whole row groups by
/// Parquet statistics (no data pages read for pruned groups), decodes only
/// candidate groups, exact-filters rows, and groups matching measures by `id`.
/// Bounded memory (one row group at a time). `column` must be a scalar column.
ControlNet read_where_miniparquet(const std::string& path, const std::string& column,
                                  double lo, double hi);

}  // namespace cnet

#endif  // MINISET_CNET_MINIPARQUET_IO_HPP
