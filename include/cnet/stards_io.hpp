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

#ifndef MINISET_CNET_STARDS_IO_HPP
#define MINISET_CNET_STARDS_IO_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cnet/control_net.hpp"

namespace cnet {

/// Read a control network from a StarDS (.stards) file. Each ControlNet column
/// is stored as a named StarDS array (struct-of-arrays), so this maps directly
/// back into the in-memory model. `path` may be local or (if StarDS is built
/// with HTTP/S3 support) a remote URL. Native build only.
ControlNet read_control_net_stards(const std::string& path);

/// Write a control network to a StarDS (.stards) file: one named array per SoA
/// column, the network header into StarDS metadata. Large columns are block
/// compressed, enabling later block-level random-access reads.
void write_control_net_stards(const ControlNet& net, const std::string& path);

/// Lazy, block-level partial reader over a StarDS control network.
///
/// `open()` reads only shape metadata — the header plus the small per-point
/// index columns (id / measureStart / measureCount) — leaving the GB-scale
/// per-measure columns on disk. `readPoints()` then pulls just the requested
/// points: numeric columns (samples, residuals, sigmas, covariances, logs) are
/// fetched with StarDS `get_slice`, which reads only the covering compressed
/// blocks (coalesced ranged reads), so slicing a huge file touches only those
/// blocks. String columns (ids, serial numbers) are read once and cached — the
/// vendored StarDS cannot block-slice variable-width strings — so repeated
/// slice reads stay cheap for the numeric bulk. Native build only.
class StardsControlNetReader {
  public:
    StardsControlNetReader();
    ~StardsControlNetReader();
    StardsControlNetReader(StardsControlNetReader&&) noexcept;
    StardsControlNetReader& operator=(StardsControlNetReader&&) noexcept;
    StardsControlNetReader(const StardsControlNetReader&) = delete;
    StardsControlNetReader& operator=(const StardsControlNetReader&) = delete;

    /// Open a .stards control network and read its shape (no measure data).
    static StardsControlNetReader open(const std::string& path);

    // ---- Shape (no measure data loaded) -----------------------------------
    size_t numPoints() const;
    size_t numMeasures() const;
    const NetworkHeader& header() const;
    const std::string& pointId(size_t i) const;
    /// Measure range [firstMeasure, firstMeasure+count) owned by point i.
    void measureRange(size_t i, uint32_t& first, uint32_t& count) const;

    // ---- Lazy loads (return a small ControlNet holding just the slice) -----
    /// Whole points [start, start+count); numeric columns sliced via get_slice.
    ControlNet readPoints(size_t start, size_t count) const;
    /// Snap the raw measure window [rowStart, rowStart+rowCount) to the whole
    /// points that own it and return them (StarDS stores points normalized, so
    /// a raw measure window can't carry point columns; snapping keeps it whole).
    ControlNet readMeasures(size_t rowStart, size_t rowCount) const;
    /// The entire network (full-column reads).
    ControlNet readAll() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Streaming reader for the adjusted XYZ points of a StarDS net, for the docs
/// hero-banner point-cloud globe. Kept here (not in wasm_bindings.cpp) so that
/// stards.h — which under emscripten defines a file-scope EM_ASYNC_JS fetch glue
/// that must appear in exactly ONE translation unit — is included only by
/// stards_io.cpp; the WASM bindings call this plain-C++ API instead.
///
/// `open()` opens the net BY PATH: a remote URL over "/vsicurl/https://…" (or
/// "s3://…", "/vsis3/bucket/key") is read WITHOUT downloading the whole file —
/// only the header/index is fetched up front (one ranged GET), and each
/// readXYZ() pulls only the covering compressed blocks via get_slice. Accepts a
/// compact XYZ-only net (keys adjX/adjY/adjZ) or a full normalized cnet (keys
/// p.adjustedX/Y/Z). Coordinates are body-centered body-fixed metres.
class HeroPointsReader {
  public:
    HeroPointsReader();
    ~HeroPointsReader();
    HeroPointsReader(HeroPointsReader&&) noexcept;
    HeroPointsReader& operator=(HeroPointsReader&&) noexcept;
    HeroPointsReader(const HeroPointsReader&) = delete;
    HeroPointsReader& operator=(const HeroPointsReader&) = delete;

    /// Open a .stards net by path/URL; reads only the header + index (no blocks).
    static HeroPointsReader open(const std::string& path);

    /// Total adjusted points available (from the index; no block data read).
    size_t count() const;

    /// Read window [start, start+n) of adjusted XYZ into `outXYZ` as interleaved
    /// float32 (x,y,z per point), clamped to [0, count()). Returns the number of
    /// points written; sets `outMaxRadius` to the max |xyz| in the window (for
    /// camera framing). Only the covering compressed blocks are fetched.
    size_t readXYZ(size_t start, size_t n,
                   std::vector<float>& outXYZ, double& outMaxRadius) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnet

#endif  // MINISET_CNET_STARDS_IO_HPP
