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

// cnet_convert: convert control networks between the protobuf .net, Parquet, and
// StarDS formats. Format is inferred from each path's extension. .net and
// .parquet may be /vsi paths; .stards is a local-filesystem columnar store.
//
// Memory: a Parquet->Parquet or .net->Parquet conversion STREAMS point batches
// through a bounded reader->writer pipeline, so a multi-GB control net converts
// without materializing the whole network in RAM. Other combinations fall back
// to a whole-net load.
//
// Query: --where COL:LO..HI selects only measures whose numeric column COL is in
// [LO,HI], using Parquet row-group statistics to skip non-matching groups.

#include <cstdlib>
#include <iostream>
#include <string>

#include "../external/argparse.hpp"
#include "cnet/control_net_io.hpp"
#include "cnet/control_net_reader.hpp"
#include "cnet/miniparquet_io.hpp"
#include "cnet/net_protobuf.hpp"
#include "cnet/stards_io.hpp"

namespace {

bool ends_with(const std::string& s, const std::string& suf) {
    std::string p = s.substr(0, s.find('?'));
    if (p.size() < suf.size()) return false;
    std::string tail = p.substr(p.size() - suf.size());
    for (auto& c : tail) c = static_cast<char>(std::tolower((unsigned char)c));
    return tail == suf;
}

// Parse "col:lo..hi" into its parts. Returns false if not present/parseable.
bool parse_where(const std::string& w, std::string& col, double& lo, double& hi) {
    auto colon = w.find(':');
    auto dots = w.find("..");
    if (colon == std::string::npos || dots == std::string::npos || dots < colon) return false;
    col = w.substr(0, colon);
    try {
        lo = std::stod(w.substr(colon + 1, dots - colon - 1));
        hi = std::stod(w.substr(dots + 2));
    } catch (...) { return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    argparse::ArgumentParser program("cnet_convert", "1.0");
    program.add_description(
        "Convert an ISIS control network between protobuf (.net), Parquet "
        "(.parquet), and StarDS (.stards). Format is inferred from the file "
        "extensions. Parquet/.net conversions stream in bounded memory, and "
        "Parquet output is GZIP-compressed by default (--compress none to opt out).");
    program.add_argument("from").help("Input control network (.net, .parquet, or .stards)");
    program.add_argument("to").help("Output control network (.net, .parquet, or .stards)");
    program.add_argument("--where")
        .default_value(std::string())
        .help("Range filter COL:LO..HI, e.g. sample:0..1024 (Parquet source only)");
    program.add_argument("--compress")
        .default_value(std::string("gzip"))
        .help("Parquet page compression for .parquet output: gzip (default) or none");
    program.add_argument("--summarize")
        .default_value(std::string())
        .help("For .stards output: also write a Gaussian-splat LOD summary layer "
              "(cnet/3) of the given number of splats, e.g. --summarize 4000");
    program.add_argument("--summarize-method")
        .default_value(std::string("tracks"))
        .help("Summary type for .stards output. Gaussian-splat methods (need "
              "--summarize K): 'tracks' (default) reorders by image-overlap track "
              "before fitting; 'segment' fits in file order. Polyline method: "
              "'lines' traces geometric filaments and stores them as compact "
              "on-ellipsoid (lon,lat) polylines (no K needed) — the faithful "
              "track model.");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n" << program;
        return 1;
    }

    const std::string from = program.get<std::string>("from");
    const std::string to = program.get<std::string>("to");
    const std::string where = program.get<std::string>("--where");
    const std::string compressArg = program.get<std::string>("--compress");
    const std::string summarizeArg = program.get<std::string>("--summarize");
    const std::string summarizeMethod = program.get<std::string>("--summarize-method");
    cnet::ParquetCompression pcodec = (compressArg == "none")
        ? cnet::ParquetCompression::None : cnet::ParquetCompression::Gzip;

    size_t summarizeK = 0;
    if (!summarizeArg.empty()) {
        try {
            long long v = std::stoll(summarizeArg);
            if (v <= 0) throw std::runtime_error("must be positive");
            summarizeK = static_cast<size_t>(v);
        } catch (const std::exception&) {
            std::cerr << "bad --summarize (expected a positive splat count): " << summarizeArg << "\n";
            return 1;
        }
    }
    if (summarizeMethod != "tracks" && summarizeMethod != "segment" &&
        summarizeMethod != "lines") {
        std::cerr << "bad --summarize-method (expected 'tracks', 'segment', or 'lines'): "
                  << summarizeMethod << "\n";
        return 1;
    }
    const bool summarizeByTracks = (summarizeMethod == "tracks");
    const bool summarizeLines = (summarizeMethod == "lines");

    try {
        const bool fromParquet = ends_with(from, ".parquet");
        const bool toParquet = ends_with(to, ".parquet");
        const bool toStards = ends_with(to, ".stards");

        // --- Polyline LOD path: trace geometric filaments and store them as
        // compact on-ellipsoid (lon,lat) polylines. No K needed (line count is
        // data-driven). Whole-net load; .stards output only.
        if (summarizeLines) {
            if (!toStards)
                throw std::runtime_error("--summarize-method lines requires a .stards output");
            cnet::ControlNet net = cnet::read_control_net(from);
            cnet::write_control_net_stards_lines(net, to);   // defaults tuned on a real net
            std::cout << "Converted " << from << " -> " << to << " (cnet/3, polylines)\n"
                      << "  points: " << net.numPoints() << "  measures: " << net.numMeasures()
                      << "  method: lines\n";
            return 0;
        }

        // --- Summarize path: write a cnet/3 .stards with a Gaussian-splat LOD
        // layer. Requires a whole-net load (the fit needs all adjusted points);
        // --where/streaming don't apply. Only valid for .stards output.
        if (summarizeK > 0) {
            if (!toStards)
                throw std::runtime_error("--summarize requires a .stards output");
            cnet::ControlNet net = cnet::read_control_net(from);
            cnet::write_control_net_stards_summarized(net, to, summarizeK, summarizeByTracks);
            std::cout << "Converted " << from << " -> " << to << " (cnet/3, summarized)\n"
                      << "  points: " << net.numPoints() << "  measures: " << net.numMeasures()
                      << "  splats: " << summarizeK << "  method: " << summarizeMethod << "\n";
            return 0;
        }

        // --- Query path: range filter (requires a Parquet source) -----------
        if (!where.empty()) {
            std::string col; double lo, hi;
            if (!parse_where(where, col, lo, hi))
                throw std::runtime_error("bad --where (expected COL:LO..HI): " + where);
            if (!fromParquet)
                throw std::runtime_error("--where range queries require a .parquet source");
            cnet::ControlNetReader r = cnet::ControlNetReader::open(from);
            cnet::ControlNet sel = r.readWhere(col, lo, hi);
            if (toParquet) cnet::write_control_net_miniparquet(sel, to, pcodec);
            else cnet::write_control_net(sel, to);
            std::cout << "Queried " << from << " where " << col << " in [" << lo << ", " << hi
                      << "] -> " << to << "\n"
                      << "  points: " << sel.numPoints() << "  measures: " << sel.numMeasures() << "\n";
            return 0;
        }

        // --- Streaming path: Parquet source -> stream point batches ---------
        // Bounded memory: read one window of points at a time and write it out.
        if (fromParquet && toParquet) {
            cnet::ControlNetReader r = cnet::ControlNetReader::open(from);
            cnet::ControlNetParquetWriter w(to, pcodec);
            const size_t nPts = r.numPoints();
            const size_t kBatch = 50000;  // points per batch (bounded RAM)
            size_t totalMeas = 0;
            for (size_t p = 0; p < nPts; p += kBatch) {
                size_t count = std::min(kBatch, nPts - p);
                cnet::ControlNet batch = r.readPoints(p, count);
                w.writeBatch(batch);
                totalMeas += batch.numMeasures();
            }
            w.finish();
            std::cout << "Converted (streamed) " << from << " -> " << to << "\n"
                      << "  points: " << nPts << "  measures: " << totalMeas << "\n";
            return 0;
        }

        // --- Streaming path: .net source -> Parquet -------------------------
        // Stream point batches out of the protobuf .net (mmap; the reader decodes
        // one bounded batch of whole points at a time) straight into the Parquet
        // writer. Peak memory is one batch, so multi-GB .net files convert without
        // materializing the whole network.
        {
            const bool fromNet = ends_with(from, ".net") || ends_with(from, ".cnet");
            if (fromNet && toParquet) {
                cnet::ControlNetParquetWriter w(to, pcodec);
                size_t totalPts = 0, totalMeas = 0;
                cnet::stream_net_protobuf(from, 50000, [&](cnet::ControlNet& batch) {
                    totalPts += batch.numPoints();
                    totalMeas += batch.numMeasures();
                    w.writeBatch(batch);
                });
                w.finish();
                std::cout << "Converted (streamed) " << from << " -> " << to << "\n"
                          << "  points: " << totalPts << "  measures: " << totalMeas << "\n";
                return 0;
            }
        }

        // --- Fallback: whole-net load (non-chunked format combinations) -----
        cnet::ControlNet net = cnet::read_control_net(from);
        cnet::write_control_net(net, to);
        std::cout << "Converted " << from << " -> " << to << "\n"
                  << "  points: " << net.numPoints()
                  << "  measures: " << net.numMeasures() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "cnet_convert failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
