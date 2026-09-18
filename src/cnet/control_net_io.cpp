#include "cnet/control_net_io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "cnet/net_protobuf.hpp"

#ifdef MINISET_HAS_GDAL
#include <cpl_vsi.h>
#endif
#ifdef MINISET_CNET_HAS_MPARQUET
#include "cnet/miniparquet_io.hpp"
#endif
#ifdef MINISET_CNET_HAS_STARDS
#include "cnet/stards_io.hpp"
#endif

namespace cnet {

namespace {

std::string strip_query(const std::string& path) {
    std::size_t q = path.find('?');
    return q == std::string::npos ? path : path.substr(0, q);
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
                      [](char a, char b) { return std::tolower((unsigned char)a) ==
                                                  std::tolower((unsigned char)b); });
}

/// Read an entire file into memory. Uses GDAL VSI (so /vsimem, /vsicurl, /vsis3
/// work) when built with GDAL; falls back to std::ifstream otherwise.
std::vector<uint8_t> read_all_bytes(const std::string& path) {
#ifdef MINISET_HAS_GDAL
    VSILFILE* fp = VSIFOpenL(path.c_str(), "rb");
    if (fp == nullptr) throw std::runtime_error("Cannot open control net file: " + path);
    VSIFSeekL(fp, 0, SEEK_END);
    vsi_l_offset size = VSIFTellL(fp);
    VSIFSeekL(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    // Loop: a single VSIFReadL of a multi-GB buffer can short-read, so read in
    // chunks until EOF. (Files >4 GB otherwise came back truncated.)
    size_t total = 0;
    const size_t kChunk = 256u * 1024u * 1024u;  // 256 MiB
    while (total < buf.size()) {
        size_t want = std::min(kChunk, buf.size() - total);
        size_t got = VSIFReadL(buf.data() + total, 1, want, fp);
        if (got == 0) break;
        total += got;
    }
    VSIFCloseL(fp);
    buf.resize(total);
    return buf;
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open control net file: " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
#endif
}

void write_all_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
#ifdef MINISET_HAS_GDAL
    VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
    if (fp == nullptr) throw std::runtime_error("Cannot create control net file: " + path);
    VSIFWriteL(bytes.data(), 1, bytes.size(), fp);
    VSIFCloseL(fp);
#else
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot create control net file: " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#endif
}

}  // namespace

NetFormat infer_net_format(const std::string& path) {
    std::string p = strip_query(path);
    if (ends_with_ci(p, ".parquet")) return NetFormat::Parquet;
    if (ends_with_ci(p, ".stards")) return NetFormat::StarDS;
    return NetFormat::Protobuf;
}

ControlNet read_control_net(const std::string& path, NetFormat format) {
    if (format == NetFormat::Auto) format = infer_net_format(path);
    if (format == NetFormat::Parquet) {
        // Header-only reader (no GDAL/Arrow), in both native and WASM.
#if defined(MINISET_CNET_HAS_MPARQUET)
        return read_control_net_miniparquet(path);
#else
        throw std::runtime_error(
            "Parquet control networks require a build with the miniparquet reader.");
#endif
    }
    if (format == NetFormat::StarDS) {
#ifdef MINISET_CNET_HAS_STARDS
        return read_control_net_stards(path);
#else
        throw std::runtime_error(
            "StarDS control networks require the native build (StarDS is not built for WASM).");
#endif
    }
    return read_net_protobuf(read_all_bytes(path));
}

void write_control_net(const ControlNet& net, const std::string& path, NetFormat format) {
    if (format == NetFormat::Auto) format = infer_net_format(path);
    if (format == NetFormat::Parquet) {
        // Header-only writer (no GDAL/Arrow), in both native and WASM. Default to
        // GZIP page compression — control-net columns compress well and the
        // reader (miniparquet, Arrow, GDAL) all handle it, so this shrinks output
        // substantially at negligible cost.
#if defined(MINISET_CNET_HAS_MPARQUET)
#if defined(MINIPARQUET_ENABLE_ZLIB)
        write_control_net_miniparquet(net, path, ParquetCompression::Gzip);
#else
        write_control_net_miniparquet(net, path, ParquetCompression::None);
#endif
        return;
#else
        throw std::runtime_error(
            "Parquet control networks require a build with the miniparquet writer.");
#endif
    }
    if (format == NetFormat::StarDS) {
#ifdef MINISET_CNET_HAS_STARDS
        write_control_net_stards(net, path);
        return;
#else
        throw std::runtime_error(
            "StarDS control networks require the native build (StarDS is not built for WASM).");
#endif
    }
    write_all_bytes(path, write_net_protobuf(net));
}

}  // namespace cnet
