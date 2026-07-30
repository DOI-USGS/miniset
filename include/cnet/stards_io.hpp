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

/// Gaussian-splat level-of-detail summary of a control net ("cnet/3").
///
/// Each splat approximates the spatial density of a contiguous run of adjusted
/// points by an anisotropic 3D Gaussian: a mean `mu` (BCBF metres) and the upper
/// triangle of its 3x3 covariance `sigma` (6 doubles: xx,xy,xz,yy,yz,zz — the
/// same packing as ControlNet::adjustedCovar). `weight` is the point count, and
/// `rangeStart/rangeCount` point back into the net's point arrays so a client can
/// drill down from a splat to its exact points with one windowed get_slice read.
///
/// The K splats are a partition of the adjusted points: the ranges are contiguous
/// and cover [0, numAdjustedPoints) in point-array order, so drill-down is always
/// a single half-open window (see the plan's "contiguity" invariant).
struct GaussianSummary {
    // Struct-of-arrays, all size K. Split mu/sigma into 1-D component arrays so
    // each stores as a plain 1-D StarDS array (the well-trodden path).
    std::vector<double> muX, muY, muZ;                 // centroid (BCBF metres)
    std::vector<double> s0, s1, s2, s3, s4, s5;        // Σ upper-tri: xx,xy,xz,yy,yz,zz
    std::vector<uint32_t> weight;                      // point count in the splat
    std::vector<uint32_t> rangeStart, rangeCount;      // window into the point arrays

    size_t size() const { return muX.size(); }
};

/// Fit a Gaussian-splat summary over the adjusted points of `net` by segmenting
/// the point array (in its existing order) into `k` contiguous runs of roughly
/// equal size and fitting one Gaussian (mean + covariance) per run. Only points
/// with `hasAdjusted` set contribute; a run's `rangeStart/rangeCount` spans the
/// underlying point indices (including any skipped un-adjusted points) so the
/// ranges remain a contiguous partition for drill-down. O(numPoints), one pass.
/// Exposed (not just internal) so tests can validate the fit directly.
GaussianSummary fit_gaussian_summary(const ControlNet& net, size_t k);

/// Reorder a net's points so that points sharing an image-overlap "track" are
/// contiguous, returning the reordered net. A point's track key is the sorted set
/// of its measures' serial numbers (the images it was observed in), so points
/// along the same image overlap sort together. This makes a later K-segment splat
/// fit align with real overlap ribbons rather than arbitrary file order, and it
/// keeps each splat's points a contiguous window for drill-down.
///
/// The permutation is applied to EVERY per-point column and to the measure block
/// of each point (measures move with their point), and all CSR offset arrays
/// (measureStart/Count, apriori/adjusted covariance, measure logs) are rebuilt.
/// The result is identical to `net` as a SET of whole points — only the order
/// changes. Native only (used by the "tracks" summarize method).
ControlNet reorder_points_by_track(const ControlNet& net);

/// Write `net` to a .stards file AND fit + write a `k`-splat Gaussian summary
/// into the SAME file as a "summary" StarDS layer (keys "summary.*"), stamping
/// the base-layer header h.format="cnet/3", h.summaryCount, h.summaryMethod,
/// h.pointsReordered. Readers that ignore the layer are unaffected.
///
/// If `byTracks` is true, the points are first reordered so image-overlap tracks
/// are contiguous (reorder_points_by_track) before the K-segment fit, so the
/// splats align with real overlap ribbons rather than arbitrary file order
/// (h.summaryMethod="tracks", h.pointsReordered=1). The stored net is then the
/// reordered one — identical as a SET of points, and the splat rangeStart/Count
/// index that stored order for drill-down. If false, points keep their existing
/// order (h.summaryMethod="segment", h.pointsReordered=0).
void write_control_net_stards_summarized(const ControlNet& net, const std::string& path,
                                         size_t k, bool byTracks = false);

/// Tunables for the polyline ("lines") summary. Defaults are the values the
/// prototype validated on a real Mars net (~92% coverage, ~4 MB, 9 m resolution).
struct LineSummaryOptions {
    // Filament tracing (geometric, crosses image boundaries).
    double cellSize = 30000.0;    // voxel grid cell / neighbor search radius (m)
    double maxTurnDeg = 45.0;     // max heading change per growth step
    size_t minLen = 6;            // min points for a chain to count as a "line"
    // Simplification (3D Douglas–Peucker); larger = fewer vertices / smaller model.
    double simplifyEps = 5000.0;  // metres
    // (lon,lat) tiling for line-relative int16 quantization.
    int tilesLon = 36;            // 36 x 18 = 10° tiles
    int tilesLat = 18;
    // Biaxial ellipsoid (metres). Default: IAU Mars.
    double radiusA = 3396190.0;   // equatorial semi-axis
    double radiusC = 3376200.0;   // polar semi-axis
    // Density-map (L0) grid; a coarse per-cell point count for an instant overview.
    int densTilesLon = 72;
    int densTilesLat = 36;
};

/// Write `net` to a .stards file AND a polyline ("lines") LOD summary into the
/// SAME file (a "lines" StarDS layer). Points are reordered so each filament is a
/// contiguous window (drill-down via rangeStart/rangeCount, h.pointsReordered=1);
/// the stored net equals the input as a SET of points. The overview traces
/// geometric filaments in the adjusted point cloud, simplifies each to a
/// polyline, and stores its vertices as line-relative int16 (lon,lat) on the
/// biaxial ellipsoid — a compact, on-surface, faithful track model. Also writes
/// an L0 per-tile density map. Stamps h.format="cnet/3", h.summaryMethod="lines".
/// Native only (tracing/geodetic fit is an offline write step).
void write_control_net_stards_lines(const ControlNet& net, const std::string& path,
                                    const LineSummaryOptions& opts = {});

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

/// Reader for the Gaussian-splat summary layer of a "cnet/3" StarDS net, for the
/// LOD overview. Like HeroPointsReader it lives here (not in wasm_bindings.cpp)
/// so stards.h stays in one translation unit; the WASM bindings call this.
///
/// `open()` opens the net by path/URL (index only — one ranged GET remotely),
/// then reads the small "summary" layer arrays WHOLE (a few small ranged GETs;
/// StarDS layers are not block-sliced here — they're tiny). If the net has no
/// "summary" layer, `count()` is 0 and the client falls back to point streaming.
/// The returned splats' rangeStart/rangeCount index the net's point arrays, so a
/// client drills down with HeroPointsReader::readXYZ(rangeStart, rangeCount) on
/// the same URL.
class SummaryReader {
  public:
    SummaryReader();
    ~SummaryReader();
    SummaryReader(SummaryReader&&) noexcept;
    SummaryReader& operator=(SummaryReader&&) noexcept;
    SummaryReader(const SummaryReader&) = delete;
    SummaryReader& operator=(const SummaryReader&) = delete;

    /// Open a net by path/URL and read its summary layer (if present).
    static SummaryReader open(const std::string& path);

    /// Number of splats (0 if the net has no summary layer).
    size_t count() const;

    /// The full splat set (all arrays, size count()). Read once at open.
    const GaussianSummary& splats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Reader for the polyline ("lines") LOD summary of a "cnet/3" StarDS net. Like
/// SummaryReader it lives here so stards.h stays in one TU; the WASM bindings and
/// the hero banner call it. `open()` reads the small "lines" layer whole (the
/// header stamps carry the tiling + ellipsoid params); `count()` is 0 if the net
/// has no lines layer (client falls back to point streaming).
///
/// The stored vertices are line-relative int16 (lon,lat) on the biaxial
/// ellipsoid; this reader DEQUANTIZES them back to BCBF XYZ (float32) so callers
/// render polylines directly. Each line also exposes rangeStart/rangeCount into
/// the net's point arrays for drill-down to the real points that formed it.
class LinesReader {
  public:
    LinesReader();
    ~LinesReader();
    LinesReader(LinesReader&&) noexcept;
    LinesReader& operator=(LinesReader&&) noexcept;
    LinesReader(const LinesReader&) = delete;
    LinesReader& operator=(const LinesReader&) = delete;

    /// Open a net by path/URL and read its "lines" layer (if present).
    static LinesReader open(const std::string& path);

    /// Number of polylines (0 if the net has no lines layer).
    size_t count() const;
    /// Total vertices across all lines.
    size_t vertexCount() const;

    /// Vertex range [firstVertex, firstVertex+n) owned by line `i` (into the flat
    /// dequantized-vertex arrays returned by verticesXYZ()).
    void lineRange(size_t i, uint32_t& firstVertex, uint32_t& n) const;
    /// Point range [start, start+count) in the net's point arrays that line `i`
    /// was traced from — feed to HeroPointsReader::readXYZ for drill-down.
    void linePointRange(size_t i, uint32_t& start, uint32_t& count) const;

    /// All polyline vertices as interleaved float32 XYZ (BCBF metres), flattened
    /// across lines in line order; use lineRange(i) to index one line. Projected
    /// onto the ellipsoid surface (height dropped — the overview is on-surface).
    const std::vector<float>& verticesXYZ() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnet

#endif  // MINISET_CNET_STARDS_IO_HPP
