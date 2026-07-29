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

#include "cnet/control_net_reader.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "cnet/column_source.hpp"
#include "cnet/control_net_io.hpp"
#include "cnet/miniparquet_io.hpp"

namespace cnet {

// ---------------------------------------------------------------------------
// Lazy control-network reader, backed by the header-only Parquet reader
// (miniparquet) — no GDAL/Arrow, portable to WASM. For a Parquet path, open()
// reads only shape (footer + the `id` column); readPoints/readMeasures pull just
// the requested row window (row-group-granular) and assemble via the shared
// ColumnBatch path. Protobuf `.net` is not seekable: it is parsed fully at open
// and sliced from RAM (supportsLazy() == false).
struct ControlNetReader::Impl {
    bool lazy = false;                 // Parquet (seekable) vs .net (not)
    std::string path;

    // Parquet-backed shape (from miniparquet).
    NetworkHeader header;
    size_t rowCount = 0;
    std::vector<std::string> ids;          // one per point
    std::vector<uint32_t> pointFirstRow;   // size numPoints + 1 (CSR over rows)
    std::vector<std::string> columns;      // physical columns present

    // .net fallback: parsed once, sliced from memory.
    ControlNet fullNet;
    bool fullLoaded = false;
};

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}
bool endsWithParquet(const std::string& path) {
    std::string p = path.substr(0, path.find('?'));
    std::string suf = ".parquet";
    return p.size() >= suf.size() && lower(p.substr(p.size() - suf.size())) == suf;
}

// Convert a ColumnBatch slice into a ControlNet. The batch already re-bases
// nothing (it's a raw row window); control_net_from_columns groups by id.
ControlNet assemble(const ColumnBatch& b, const NetworkHeader& header) {
    ControlNet net = control_net_from_columns(b);
    net.header = header;  // header columns may be projected out of a slice
    return net;
}

// Seed the CSR offset arrays of an empty net (matches control_net_from_columns).
void initEmpty(ControlNet& net) {
    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);
}

}  // namespace

ControlNetReader::ControlNetReader() : impl_(std::make_unique<Impl>()) {}
ControlNetReader::~ControlNetReader() = default;
ControlNetReader::ControlNetReader(ControlNetReader&&) noexcept = default;
ControlNetReader& ControlNetReader::operator=(ControlNetReader&&) noexcept = default;

ControlNetReader ControlNetReader::open(const std::string& path) {
    ControlNetReader r;
    Impl& im = *r.impl_;
    im.path = path;

    if (!endsWithParquet(path)) {
        // Legacy protobuf .net: not seekable, parse fully now and slice from RAM.
        im.lazy = false;
        im.fullNet = read_control_net(path, NetFormat::Protobuf);
        im.fullLoaded = true;
        im.header = im.fullNet.header;
        im.rowCount = im.fullNet.numMeasures();
        im.ids = im.fullNet.pointId;
        im.pointFirstRow.assign(im.fullNet.measureStart.begin(), im.fullNet.measureStart.end());
        im.pointFirstRow.push_back(static_cast<uint32_t>(im.rowCount));
        return r;
    }

    // Parquet: cheap shape read (footer + id column only) via miniparquet.
    ParquetShape shape = read_shape_miniparquet(path);
    im.lazy = true;
    im.header = shape.header;
    im.rowCount = shape.rowCount;
    im.ids = std::move(shape.ids);
    im.pointFirstRow = std::move(shape.pointFirstRow);
    im.columns = std::move(shape.columns);
    return r;
}

bool ControlNetReader::supportsLazy() const { return impl_->lazy; }
size_t ControlNetReader::numPoints() const { return impl_->ids.size(); }
size_t ControlNetReader::numRows() const { return impl_->rowCount; }
const NetworkHeader& ControlNetReader::header() const { return impl_->header; }

bool ControlNetReader::hasColumn(const std::string& name) const {
    if (!impl_->lazy) return false;
    for (const auto& c : impl_->columns) if (c == name) return true;
    return false;
}

const std::string& ControlNetReader::pointId(size_t i) const { return impl_->ids.at(i); }

void ControlNetReader::measureRange(size_t i, uint32_t& firstRow, uint32_t& count) const {
    firstRow = impl_->pointFirstRow.at(i);
    count = impl_->pointFirstRow.at(i + 1) - firstRow;
}

// Read [rowStart, rowStart+rowCount) rows into a fresh ControlNet.
ControlNet ControlNetReader::readMeasures(size_t rowStart, size_t rowCount,
                                          const ReadOptions& opts) const {
    Impl& im = *impl_;
    if (!im.lazy) {
        // Not seekable: reuse the fully-parsed net (already in RAM).
        return im.fullNet;
    }
    ControlNet net;
    net.header = im.header;
    if (rowStart >= im.rowCount || rowCount == 0) { initEmpty(net); return net; }

    // Column projection: miniparquet reads only the requested columns. Note the
    // attribute-filter (opts.filter) form isn't supported by miniparquet (no
    // predicate pushdown); it is ignored here — callers that need row filtering
    // should post-filter the returned slice.
    ColumnBatch b = read_columns_miniparquet(im.path, opts.columns, rowStart, rowCount);
    return assemble(b, im.header);
}

ControlNet ControlNetReader::readPoints(size_t start, size_t count,
                                        const ReadOptions& opts) const {
    const Impl& im = *impl_;
    if (start >= numPoints() || count == 0) {
        ControlNet net; net.header = im.header; initEmpty(net); return net;
    }
    size_t endPt = std::min(start + count, numPoints());
    uint32_t rowStart = im.pointFirstRow[start];
    uint32_t rowEnd = im.pointFirstRow[endPt];  // exclusive; snaps to whole points
    return readMeasures(rowStart, rowEnd - rowStart, opts);
}

ControlNet ControlNetReader::readAll(const ReadOptions& opts) const {
    if (!impl_->lazy) return impl_->fullNet;
    return readMeasures(0, impl_->rowCount, opts);
}

ControlNet ControlNetReader::readWhere(const std::string& column, double lo, double hi,
                                       const ReadOptions& /*opts*/) const {
    const Impl& im = *impl_;
    if (!im.lazy)
        throw std::runtime_error("readWhere: range queries require Parquet (not .net)");
    ControlNet net = read_where_miniparquet(im.path, column, lo, hi);
    net.header = im.header;
    return net;
}

}  // namespace cnet
