// extract_hero_points — pull the adjusted XYZ control points out of a normalized
// StarDS control net into a small, XYZ-ONLY .stards the docs hero banner streams
// in-browser with the miniset WASM StarDS reader.
//
// This is a SHORT-TERM local build step: it reads a .stards net on disk and
// emits `docs/docs/assets/hero.stards` (git-ignored — we do NOT ship the
// multi-GB source net). Later this step goes away entirely: the browser will
// read the adjusted XYZ points directly from the remote net over /vsicurl/ via
// the same WASM StarDS reader.
//
// By default it extracts ALL points that have an adjusted coordinate (no cap):
// the banner streams them in over time and simply reads more the longer it runs,
// so the complete file is the right source. An optional maxPoints arg caps it
// for quick iteration.
//
// Output: a StarDS dataset with three FLOAT64 data arrays `adjX`, `adjY`, `adjZ`
// (body-centered body-fixed metres), GZIP_SHUFFLE_BLOCK compression (the WASM
// module links Emscripten's zlib port, so it inflates in-browser). Only points
// with hasAdjusted set are kept.
//
// Points are written SORTED BY AZIMUTH (atan2(x, z), the angle around the polar
// Y axis, ascending in [-pi, pi]). This makes any streamed window [start,
// start+k) a contiguous *vertical wedge* of the globe. The hero banner exploits
// that: it ties the read cursor to the globe's rotation so the wedge currently
// rotating toward the viewer loads first, and one full turn loads the whole
// file. (A generic file order would force a full-file decode + global sort at
// runtime — ~4 s — to get the same effect; doing it here makes it free.)
//
// Build (native, with StarDS + zlib to read the compressed source):
//   c++ -std=c++17 -O2 -I../../external -DENABLE_ZLIB extract_hero_points.cpp \
//       -lz -lpthread -o extract_hero_points
// Run (all adjusted points):
//   ./extract_hero_points ~/work/cnets/all_quads.stards ../docs/assets/hero.stards

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stards.h"

using star::CompressionAlgorithm;
using star::FileMode;
using star::NDArray;
using star::Slice;
using star::StarDataset;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <in.stards> <out.stards> [maxPoints=all]\n", argv[0]);
        return 2;
    }
    const std::string inPath = argv[1], outPath = argv[2];

    auto in = StarDataset::open(inPath, FileMode::READ_ONLY);
    // Total points from the index (no block data read). Default: take them all;
    // an optional arg caps it for quick iteration.
    size_t total = in->array_length("p.adjustedX");
    if (argc > 3) total = std::min(total, static_cast<size_t>(std::atoll(argv[3])));

    // get_slice fetches only the covering compressed blocks, so this never
    // materializes more of the (multi-GB) net than the requested window.
    auto ax = in->get_slice<double>("p.adjustedX", {Slice{0, total}});
    auto ay = in->get_slice<double>("p.adjustedY", {Slice{0, total}});
    auto az = in->get_slice<double>("p.adjustedZ", {Slice{0, total}});
    auto ha = in->get_slice<double>("p.hasAdjusted", {Slice{0, total}});
    const size_t n = ax.size();

    // Keep only adjusted points, then order them by azimuth so the file is a
    // sequence of vertical wedges (see the header note). Sort indices by
    // atan2(x, z) ascending, then gather.
    std::vector<size_t> order;
    order.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i < ha.size() && ha.flat(i) == 0.0) continue;  // skip un-adjusted
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return std::atan2(ax.flat(a), az.flat(a)) < std::atan2(ax.flat(b), az.flat(b));
    });
    const size_t kept = order.size();

    std::vector<double> x, y, z;
    x.reserve(kept); y.reserve(kept); z.reserve(kept);
    for (size_t idx : order) {
        x.push_back(ax.flat(idx)); y.push_back(ay.flat(idx)); z.push_back(az.flat(idx));
    }

    star::StarConfig cfg;
    // Byte-shuffle-within-block + GZIP: compresses the float64 coords well and
    // stays sliceable; the WASM module links zlib so it inflates in-browser.
    cfg.compression = CompressionAlgorithm::GZIP_SHUFFLE_BLOCK;
    auto out = StarDataset::create(outPath, cfg);
    out->put("adjX", NDArray<double>(std::move(x), {kept}));
    out->put("adjY", NDArray<double>(std::move(y), {kept}));
    out->put("adjZ", NDArray<double>(std::move(z), {kept}));
    out->close();

    std::fprintf(stderr, "wrote %zu adjusted points to %s (GZIP_SHUFFLE_BLOCK)\n", kept, outPath.c_str());
    return 0;
}
