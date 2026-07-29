// miniparquet.hpp — a header-only, dependency-light C++17 Parquet *reader*.
//
// Scope (deliberately narrow — tailored to miniset's control-network files):
//   * Physical types: INT32, DOUBLE, BYTE_ARRAY (utf8). No INT96/FLOAT/BOOL/FLBA.
//   * One level of LIST nesting (the standard 3-node LIST -> list -> element
//     group), used for the covariance / log columns. No deeper nesting, maps, or
//     structs.
//   * Encodings: PLAIN, RLE / BIT_PACKED hybrid (levels), PLAIN_DICTIONARY /
//     RLE_DICTIONARY (dictionary-indexed data pages).
//   * Compression: UNCOMPRESSED, SNAPPY (vendored decoder), GZIP (zlib, if
//     MINIPARQUET_ENABLE_ZLIB). ZSTD/LZ4/BROTLI -> clear "unsupported" throw.
//   * DATA_PAGE (v1) and DATA_PAGE_V2. Thrift *compact* footer only.
// Anything outside this set throws std::runtime_error rather than returning
// silently-wrong data.
//
// Remote reads: a small RangeReader abstraction (mirroring external/stards.h)
// backs local files, in-memory bytes, and — behind MINIPARQUET_ENABLE_CURL /
// MINIPARQUET_ENABLE_S3 — HTTP/S3 range requests, so a .parquet can be read
// lazily like a cloud-optimized file (footer first, then only the requested
// column chunks).

#ifndef MINIPARQUET_HPP
#define MINIPARQUET_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "snappy/snappy_decompress.hpp"

#ifdef MINIPARQUET_ENABLE_ZLIB
#include <zlib.h>
#endif

#ifdef MINIPARQUET_ENABLE_CURL
#include <curl/curl.h>
#endif

// On WASM, remote range reads go through Emscripten's Fetch API (there is no
// libcurl in the browser build). Requires linking with -sFETCH=1.
#if defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>
#endif

namespace mparq {

// ===========================================================================
// RangeReader — byte-range access over local / memory / HTTP / S3 sources.
// (Contract mirrors external/stards.h so the two share a mental model.)
// ===========================================================================
class RangeReader {
  public:
    virtual ~RangeReader() = default;
    /// Read `len` bytes at `offset` into `out` (resized to bytes read). Returns
    /// the number of bytes read (may be short at EOF).
    virtual size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) = 0;
    /// Total size if known, else SIZE_MAX.
    virtual size_t size_or_unknown() = 0;
    virtual bool good() const = 0;
};

class LocalRangeReader : public RangeReader {
  public:
    explicit LocalRangeReader(const std::string& path)
        : m_stream(path, std::ios::binary) {}
    size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) override {
        out.assign(len, 0);
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        size_t got = static_cast<size_t>(m_stream.gcount());
        out.resize(got);
        return got;
    }
    size_t size_or_unknown() override {
        auto cur = m_stream.tellg();
        m_stream.seekg(0, std::ios::end);
        auto end = m_stream.tellg();
        m_stream.seekg(cur);
        return end < 0 ? SIZE_MAX : static_cast<size_t>(end);
    }
    bool good() const override { return m_stream.good() || m_stream.eof(); }

  private:
    mutable std::ifstream m_stream;
};

class MemoryRangeReader : public RangeReader {
  public:
    explicit MemoryRangeReader(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}
    size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) override {
        if (offset >= m_bytes.size()) { out.clear(); return 0; }
        size_t end = std::min(offset + len, m_bytes.size());
        out.assign(m_bytes.begin() + offset, m_bytes.begin() + end);
        return out.size();
    }
    size_t size_or_unknown() override { return m_bytes.size(); }
    bool good() const override { return true; }

  private:
    std::vector<uint8_t> m_bytes;
};

#ifdef MINIPARQUET_ENABLE_CURL
// One persistent easy handle per file; each read_at is a single ranged GET.
// Size is learned opportunistically from Content-Range (no HEAD).
class HttpRangeReader : public RangeReader {
  public:
    explicit HttpRangeReader(const std::string& url) : m_url(url) {
        m_curl = curl_easy_init();
    }
    ~HttpRangeReader() override { if (m_curl) curl_easy_cleanup(m_curl); }

    size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) override {
        out.clear();
        if (!m_curl || len == 0) return 0;
        m_sink = &out;
        std::string range = std::to_string(offset) + "-" + std::to_string(offset + len - 1);
        curl_easy_setopt(m_curl, CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &HttpRangeReader::write_cb);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &HttpRangeReader::header_cb);
        curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
        m_ok = (curl_easy_perform(m_curl) == CURLE_OK);
        m_sink = nullptr;
        return out.size();
    }
    size_t size_or_unknown() override { return m_total; }
    bool good() const override { return m_curl != nullptr && m_ok; }

  private:
    static size_t write_cb(char* p, size_t sz, size_t nm, void* self) {
        auto* r = static_cast<HttpRangeReader*>(self);
        size_t n = sz * nm;
        if (r->m_sink) r->m_sink->insert(r->m_sink->end(),
                                         reinterpret_cast<uint8_t*>(p),
                                         reinterpret_cast<uint8_t*>(p) + n);
        return n;
    }
    static size_t header_cb(char* p, size_t sz, size_t nm, void* self) {
        auto* r = static_cast<HttpRangeReader*>(self);
        size_t n = sz * nm;
        std::string h(p, n);
        auto slash = h.find('/');
        if (h.rfind("Content-Range:", 0) == 0 && slash != std::string::npos) {
            try { r->m_total = std::stoull(h.substr(slash + 1)); } catch (...) {}
        }
        return n;
    }
    std::string m_url;
    CURL* m_curl = nullptr;
    std::vector<uint8_t>* m_sink = nullptr;
    size_t m_total = SIZE_MAX;
    bool m_ok = true;
};
#endif  // MINIPARQUET_ENABLE_CURL

#if defined(__EMSCRIPTEN__)
// Remote range reads in the browser/Node WASM build via Emscripten's Fetch API.
// Each read_at issues one synchronous ranged GET (Range: bytes=a-b), matching the
// StarDS remote-read model (one request per coalesced span, no HEAD — the total
// size is learned opportunistically from the Content-Range of any response).
// Requires linking the module with -sFETCH=1. Synchronous fetch works on a Web
// Worker or in Node; on the main browser thread, build with -sASYNCIFY or drive
// reads from a worker.
class EmscriptenFetchRangeReader : public RangeReader {
  public:
    explicit EmscriptenFetchRangeReader(const std::string& url) : m_url(url) {}

    size_t read_at(size_t offset, size_t len, std::vector<uint8_t>& out) override {
        out.clear();
        if (len == 0) return 0;

        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init(&attr);
        std::strcpy(attr.requestMethod, "GET");
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

        std::string range = "bytes=" + std::to_string(offset) + "-" +
                            std::to_string(offset + len - 1);
        const char* headers[] = {"Range", range.c_str(), nullptr};
        attr.requestHeaders = headers;

        emscripten_fetch_t* f = emscripten_fetch(&attr, m_url.c_str());
        if (f == nullptr) { m_ok = false; return 0; }
        // 206 Partial Content (range honored) or 200 (whole file returned).
        bool ok = (f->status == 206 || f->status == 200) && f->numBytes > 0;
        if (ok) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(f->data);
            out.assign(p, p + f->numBytes);
            // Learn total size from Content-Range: "bytes a-b/TOTAL" if present.
            size_t hlen = emscripten_fetch_get_response_headers_length(f);
            if (hlen > 0) {
                std::string hbuf(hlen + 1, '\0');
                emscripten_fetch_get_response_headers(f, &hbuf[0], hlen + 1);
                auto pos = hbuf.find("/");
                auto cr = hbuf.find("content-range");
                if (cr == std::string::npos) cr = hbuf.find("Content-Range");
                if (cr != std::string::npos) {
                    auto slash = hbuf.find('/', cr);
                    if (slash != std::string::npos) {
                        try { m_total = std::stoull(hbuf.substr(slash + 1)); } catch (...) {}
                    }
                } else if (f->status == 200) {
                    m_total = f->numBytes;  // whole file returned
                }
            }
        } else {
            m_ok = false;
        }
        emscripten_fetch_close(f);
        return out.size();
    }
    size_t size_or_unknown() override { return m_total; }
    bool good() const override { return m_ok; }

  private:
    std::string m_url;
    size_t m_total = SIZE_MAX;
    bool m_ok = true;
};
#endif  // __EMSCRIPTEN__

// Build a remote reader for an http(s) URL, using whichever backend the build
// provides: Emscripten Fetch in WASM, libcurl natively (if enabled).
inline std::unique_ptr<RangeReader> make_http_reader(const std::string& url) {
#if defined(__EMSCRIPTEN__)
    return std::make_unique<EmscriptenFetchRangeReader>(url);
#elif defined(MINIPARQUET_ENABLE_CURL)
    return std::make_unique<HttpRangeReader>(url);
#else
    (void)url;
    throw std::runtime_error(
        "miniparquet: HTTP range reads need Emscripten Fetch (WASM) or "
        "MINIPARQUET_ENABLE_CURL (native).");
#endif
}

/// Choose a backend from a path. `/vsicurl/<url>` and `http(s)://…` -> remote
/// (Emscripten Fetch on WASM, libcurl natively); otherwise a local file.
inline std::unique_ptr<RangeReader> make_range_reader(const std::string& path) {
    const std::string vc = "/vsicurl/";
    if (path.rfind(vc, 0) == 0) return make_http_reader(path.substr(vc.size()));
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        return make_http_reader(path);
    return std::make_unique<LocalRangeReader>(path);
}

// ===========================================================================
// Parquet enums (subset we handle) + thrift-compact reader.
// ===========================================================================
enum class PhysType { Boolean = 0, Int32 = 1, Int64 = 2, Int96 = 3,
                      Float = 4, Double = 5, ByteArray = 6, Flba = 7 };
enum class Encoding { Plain = 0, PlainDictionary = 2, Rle = 3, BitPacked = 4,
                      DeltaBinaryPacked = 5, DeltaLengthByteArray = 6,
                      DeltaByteArray = 7, RleDictionary = 8, ByteStreamSplit = 9 };
enum class Codec { Uncompressed = 0, Snappy = 1, Gzip = 2, Lzo = 3, Brotli = 4,
                   Lz4 = 5, Zstd = 6, Lz4Raw = 7 };
enum class PageType { DataPage = 0, IndexPage = 1, DictionaryPage = 2, DataPageV2 = 3 };
enum class Repetition { Required = 0, Optional = 1, Repeated = 2 };

// Thrift compact-protocol element types.
namespace tc {
enum : int { STOP = 0, BOOL_T = 1, BOOL_F = 2, BYTE = 3, I16 = 4, I32 = 5,
             I64 = 6, DOUBLE = 7, BINARY = 8, LIST = 9, SET = 10, MAP = 11, STRUCT = 12 };
}

/// Minimal Thrift compact-protocol reader over an in-memory buffer.
class ThriftReader {
  public:
    ThriftReader(const uint8_t* data, size_t size) : m_p(data), m_n(size) {}

    size_t pos() const { return m_i; }
    void seek(size_t i) { m_i = i; }

    uint8_t byte() {
        if (m_i >= m_n) throw std::runtime_error("miniparquet: thrift EOF");
        return m_p[m_i++];
    }
    uint64_t varint() {
        uint64_t r = 0; int shift = 0;
        while (true) {
            uint8_t b = byte();
            r |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 63) throw std::runtime_error("miniparquet: varint too long");
        }
        return r;
    }
    int64_t zigzag() {
        uint64_t n = varint();
        return static_cast<int64_t>(n >> 1) ^ -static_cast<int64_t>(n & 1);
    }
    double f64() {
        if (m_i + 8 > m_n) throw std::runtime_error("miniparquet: thrift EOF (double)");
        double v; std::memcpy(&v, m_p + m_i, 8); m_i += 8; return v;  // LE host
    }
    std::string binary() {
        uint64_t len = varint();
        if (m_i + len > m_n) throw std::runtime_error("miniparquet: thrift EOF (binary)");
        std::string s(reinterpret_cast<const char*>(m_p + m_i), len);
        m_i += len;
        return s;
    }

    /// Read a struct field header. Returns false at STOP. On true, sets `type`
    /// (compact element type) and `fid` (field id, delta-decoded).
    bool field(int& type, int& fid) {
        uint8_t h = byte();
        if (h == tc::STOP) return false;
        int delta = (h >> 4) & 0x0F;
        type = h & 0x0F;
        if (delta == 0) fid = static_cast<int>(zigzag());
        else fid = m_last_fid + delta;
        m_last_fid = fid;
        return true;
    }

    /// Skip a value of the given compact type (used for fields we don't need).
    void skip(int type) {
        switch (type) {
            case tc::BOOL_T: case tc::BOOL_F: break;
            case tc::BYTE: byte(); break;
            case tc::I16: case tc::I32: case tc::I64: zigzag(); break;
            case tc::DOUBLE: f64(); break;
            case tc::BINARY: binary(); break;
            case tc::LIST: case tc::SET: {
                int et, sz; list_header(et, sz);
                for (int i = 0; i < sz; ++i) skip(et);
                break;
            }
            case tc::MAP: {
                uint64_t sz = varint();
                if (sz != 0) {
                    uint8_t kv = byte();
                    int kt = (kv >> 4) & 0x0F, vt = kv & 0x0F;
                    for (uint64_t i = 0; i < sz; ++i) { skip(kt); skip(vt); }
                }
                break;
            }
            case tc::STRUCT: {
                int t, f;
                int saved = m_last_fid; m_last_fid = 0;
                while (field(t, f)) skip(t);
                m_last_fid = saved;
                break;
            }
            default: throw std::runtime_error("miniparquet: bad thrift type in skip");
        }
    }

    /// Read a list/set header, returning element type and count.
    void list_header(int& elem_type, int& size) {
        uint8_t h = byte();
        int sz = (h >> 4) & 0x0F;
        elem_type = h & 0x0F;
        size = (sz == 0x0F) ? static_cast<int>(varint()) : sz;
    }

    // Nested structs need their own field-id delta context; callers save/restore.
    int last_fid() const { return m_last_fid; }
    void set_last_fid(int v) { m_last_fid = v; }

  private:
    const uint8_t* m_p;
    size_t m_n;
    size_t m_i = 0;
    int m_last_fid = 0;
};

// RAII helper: enter a nested struct (reset field-id delta), restore on exit.
struct ThriftStructScope {
    ThriftReader& t;
    int saved;
    explicit ThriftStructScope(ThriftReader& tr) : t(tr), saved(tr.last_fid()) { t.set_last_fid(0); }
    ~ThriftStructScope() { t.set_last_fid(saved); }
};

// ===========================================================================
// Parsed footer metadata (the subset we use).
// ===========================================================================
struct SchemaElement {
    PhysType type = PhysType::Int32;
    bool has_type = false;              // false for group nodes (LIST containers)
    Repetition repetition = Repetition::Required;
    std::string name;
    int num_children = 0;
};

struct ColumnChunkMeta {
    std::vector<std::string> path;      // dotted schema path
    PhysType type = PhysType::Int32;
    Codec codec = Codec::Uncompressed;
    int64_t num_values = 0;
    int64_t data_page_offset = 0;
    int64_t dictionary_page_offset = 0; // 0 if absent
    int64_t total_compressed_size = 0;
    bool has_dictionary = false;
    // Statistics (present when has_stats): min/max are PLAIN-encoded raw bytes
    // (4B int32, 8B double, or the raw utf8 for BYTE_ARRAY).
    bool has_stats = false;
    bool has_minmax = false;
    int64_t null_count = 0;
    std::string min_value, max_value;
};

struct RowGroupMeta {
    std::vector<ColumnChunkMeta> columns;
    int64_t num_rows = 0;
};

struct FileMeta {
    int version = 0;
    std::vector<SchemaElement> schema;  // flattened, incl. group nodes
    int64_t num_rows = 0;
    std::vector<RowGroupMeta> row_groups;
};

// ===========================================================================
// Decoded column output.
// ===========================================================================
struct ColumnData {
    enum Type { Int32, Double, ByteArray } type = Int32;
    std::vector<int32_t> i32;
    std::vector<double> f64;
    std::vector<std::string> str;
    // Presence per top-level row (1 = value present, 0 = null). For list
    // columns, `present` is per-row and `listLen` gives the element count.
    std::vector<uint8_t> present;
    std::vector<int32_t> listLen;       // populated only for list columns
    bool is_list = false;
};

// ===========================================================================
// Reader
// ===========================================================================
class Reader {
  public:
    static Reader open(const std::string& path) {
        Reader r;
        r.m_reader = make_range_reader(path);
        r.load_footer();
        return r;
    }
    static Reader openBytes(std::vector<uint8_t> bytes) {
        Reader r;
        r.m_reader = std::make_unique<MemoryRangeReader>(std::move(bytes));
        r.load_footer();
        return r;
    }
    /// Open over a caller-supplied RangeReader. Lets an embedder plug in its own
    /// byte source (e.g. GDAL VSI, which handles /vsimem, /vsicurl, /vsis3).
    static Reader fromReader(std::unique_ptr<RangeReader> reader) {
        Reader r;
        r.m_reader = std::move(reader);
        r.load_footer();
        return r;
    }

    size_t numRows() const { return static_cast<size_t>(m_meta.num_rows); }
    size_t numRowGroups() const { return m_meta.row_groups.size(); }
    bool hasColumn(const std::string& name) const { return leaf_index(name) >= 0; }

    /// Read named columns for all rows.
    std::map<std::string, ColumnData> readColumns(const std::vector<std::string>& names) {
        return readColumns(names, 0, numRows());
    }

    /// Read named columns for the row range [rowStart, rowStart+rowCount),
    /// fetching only the covering row groups' chunks for those columns.
    std::map<std::string, ColumnData> readColumns(const std::vector<std::string>& names,
                                                  size_t rowStart, size_t rowCount) {
        std::map<std::string, ColumnData> out;
        if (rowCount == 0 || rowStart >= numRows()) {
            for (const auto& n : names) out[n] = ColumnData{};
            return out;
        }
        size_t rowEnd = std::min(rowStart + rowCount, numRows());

        // Determine which row groups overlap [rowStart, rowEnd).
        std::vector<std::pair<size_t, std::pair<size_t, size_t>>> groups;  // (rgIdx,(localStart,localEnd))
        size_t cum = 0;
        for (size_t g = 0; g < m_meta.row_groups.size(); ++g) {
            size_t gRows = static_cast<size_t>(m_meta.row_groups[g].num_rows);
            size_t gStart = cum, gEnd = cum + gRows;
            if (gEnd > rowStart && gStart < rowEnd) {
                size_t ls = (rowStart > gStart) ? rowStart - gStart : 0;
                size_t le = (rowEnd < gEnd) ? rowEnd - gStart : gRows;
                groups.push_back({g, {ls, le}});
            }
            cum = gEnd;
        }

        for (const auto& name : names) {
            int leaf = leaf_index(name);
            if (leaf < 0) { out[name] = ColumnData{}; continue; }
            ColumnData col;
            col.is_list = m_leaf_is_list[leaf];
            col.type = to_coltype(m_meta.schema[m_leaf_schema[leaf]].type);
            for (const auto& gr : groups) {
                decode_chunk(gr.first, name, gr.second.first, gr.second.second, col);
            }
            out[name] = std::move(col);
        }
        return out;
    }

    const FileMeta& meta() const { return m_meta; }

    // ---- Row-group access + statistics (for range queries) -----------------
    /// First global row index of row group g.
    size_t rowGroupFirstRow(size_t g) const {
        size_t cum = 0;
        for (size_t i = 0; i < g && i < m_meta.row_groups.size(); ++i)
            cum += static_cast<size_t>(m_meta.row_groups[i].num_rows);
        return cum;
    }
    size_t rowGroupRows(size_t g) const {
        return static_cast<size_t>(m_meta.row_groups.at(g).num_rows);
    }

    /// True if column `name`'s chunk in row group `g` carries min/max stats.
    bool rowGroupHasStats(size_t g, const std::string& name) const {
        const ColumnChunkMeta* cc = chunkOf(g, name);
        return cc && cc->has_stats && cc->has_minmax;
    }
    /// Decoded double min/max for a scalar column in a row group (valid only when
    /// the column is Int32/Double and rowGroupHasStats is true).
    bool rowGroupRangeDouble(size_t g, const std::string& name, double& lo, double& hi) const {
        const ColumnChunkMeta* cc = chunkOf(g, name);
        if (!cc || !cc->has_minmax) return false;
        if (cc->type == PhysType::Double) {
            if (cc->min_value.size() < 8 || cc->max_value.size() < 8) return false;
            std::memcpy(&lo, cc->min_value.data(), 8);
            std::memcpy(&hi, cc->max_value.data(), 8);
            return true;
        }
        if (cc->type == PhysType::Int32) {
            if (cc->min_value.size() < 4 || cc->max_value.size() < 4) return false;
            int32_t a, b; std::memcpy(&a, cc->min_value.data(), 4); std::memcpy(&b, cc->max_value.data(), 4);
            lo = a; hi = b; return true;
        }
        return false;
    }
    /// String min/max for a BYTE_ARRAY column in a row group.
    bool rowGroupRangeString(size_t g, const std::string& name, std::string& lo, std::string& hi) const {
        const ColumnChunkMeta* cc = chunkOf(g, name);
        if (!cc || !cc->has_minmax || cc->type != PhysType::ByteArray) return false;
        lo = cc->min_value; hi = cc->max_value; return true;
    }

    /// Row groups whose `name` range intersects [lo, hi] (numeric). Row groups
    /// lacking stats are conservatively kept (cannot prune). This is the cheap
    /// index-based prune — no data pages are read.
    std::vector<size_t> pruneRowGroupsDouble(const std::string& name, double lo, double hi) const {
        std::vector<size_t> keep;
        for (size_t g = 0; g < m_meta.row_groups.size(); ++g) {
            double rlo, rhi;
            if (rowGroupRangeDouble(g, name, rlo, rhi)) {
                if (rhi < lo || rlo > hi) continue;  // disjoint → prune
            }
            keep.push_back(g);
        }
        return keep;
    }

    /// Read named columns for exactly one row group (bounded memory: one RG).
    std::map<std::string, ColumnData> readRowGroup(size_t g, const std::vector<std::string>& names) {
        std::map<std::string, ColumnData> out;
        size_t rows = rowGroupRows(g);
        for (const auto& name : names) {
            int leaf = leaf_index(name);
            if (leaf < 0) { out[name] = ColumnData{}; continue; }
            ColumnData col;
            col.is_list = m_leaf_is_list[leaf];
            col.type = to_coltype(m_meta.schema[m_leaf_schema[leaf]].type);
            decode_chunk(g, name, 0, rows, col);
            out[name] = std::move(col);
        }
        return out;
    }

  private:
    const ColumnChunkMeta* chunkOf(size_t g, const std::string& name) const {
        if (g >= m_meta.row_groups.size()) return nullptr;
        for (const auto& c : m_meta.row_groups[g].columns)
            if (!c.path.empty() && c.path.front() == name) return &c;
        return nullptr;
    }

    std::unique_ptr<RangeReader> m_reader;
    FileMeta m_meta;
    // Leaf columns = schema elements with a physical type (skip group nodes).
    std::vector<int> m_leaf_schema;         // leaf -> schema element index
    std::vector<std::string> m_leaf_name;   // leaf -> leaf name (last path part)
    std::vector<uint8_t> m_leaf_is_list;    // leaf -> is inside a LIST group
    std::vector<int> m_leaf_max_def;        // leaf -> max definition level
    std::vector<int> m_leaf_max_rep;        // leaf -> max repetition level

    // --- footer ------------------------------------------------------------
    void load_footer() {
        size_t total = m_reader->size_or_unknown();
        // Read the last chunk (enough to hold the footer for our files); if the
        // footer length says it's bigger, do one more ranged read.
        const size_t kTail = 64 * 1024;
        size_t tail = (total == SIZE_MAX) ? kTail : std::min(kTail, total);
        size_t tailOff = (total == SIZE_MAX) ? 0 : total - tail;
        std::vector<uint8_t> buf;
        m_reader->read_at(tailOff, tail, buf);
        if (buf.size() < 8) throw std::runtime_error("miniparquet: file too small");
        if (std::memcmp(buf.data() + buf.size() - 4, "PAR1", 4) != 0)
            throw std::runtime_error("miniparquet: missing trailing PAR1 magic");
        uint32_t footLen;
        std::memcpy(&footLen, buf.data() + buf.size() - 8, 4);  // LE host

        const uint8_t* footPtr;
        std::vector<uint8_t> footBuf;
        if (footLen + 8 <= buf.size()) {
            footPtr = buf.data() + buf.size() - 8 - footLen;
        } else {
            // Footer wasn't fully in the tail; fetch it precisely.
            if (total == SIZE_MAX) throw std::runtime_error("miniparquet: footer beyond tail on unsized source");
            size_t fo = total - 8 - footLen;
            m_reader->read_at(fo, footLen, footBuf);
            footPtr = footBuf.data();
            if (footBuf.size() < footLen) throw std::runtime_error("miniparquet: short footer read");
        }
        parse_file_meta(footPtr, footLen);
        build_leaf_index();
    }

    void parse_file_meta(const uint8_t* p, size_t n) {
        ThriftReader t(p, n);
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 1: m_meta.version = static_cast<int>(t.zigzag()); break;      // version
                case 2: {                                                          // schema list
                    int et, sz; t.list_header(et, sz);
                    for (int i = 0; i < sz; ++i) m_meta.schema.push_back(parse_schema_element(t));
                    break;
                }
                case 3: m_meta.num_rows = t.zigzag(); break;                        // num_rows
                case 4: {                                                          // row_groups list
                    int et, sz; t.list_header(et, sz);
                    for (int i = 0; i < sz; ++i) m_meta.row_groups.push_back(parse_row_group(t));
                    break;
                }
                default: t.skip(type); break;
            }
        }
    }

    SchemaElement parse_schema_element(ThriftReader& t) {
        ThriftStructScope scope(t);
        SchemaElement e;
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 1: e.type = static_cast<PhysType>(t.zigzag()); e.has_type = true; break;
                case 3: e.repetition = static_cast<Repetition>(t.zigzag()); break;
                case 4: e.name = t.binary(); break;
                case 5: e.num_children = static_cast<int>(t.zigzag()); break;
                default: t.skip(type); break;  // type_length, converted_type, logicalType...
            }
        }
        return e;
    }

    RowGroupMeta parse_row_group(ThriftReader& t) {
        ThriftStructScope scope(t);
        RowGroupMeta rg;
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 1: {  // columns list<ColumnChunk>
                    int et, sz; t.list_header(et, sz);
                    for (int i = 0; i < sz; ++i) rg.columns.push_back(parse_column_chunk(t));
                    break;
                }
                case 3: rg.num_rows = t.zigzag(); break;
                default: t.skip(type); break;
            }
        }
        return rg;
    }

    ColumnChunkMeta parse_column_chunk(ThriftReader& t) {
        ThriftStructScope scope(t);
        ColumnChunkMeta cc;
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 3: parse_column_meta(t, cc); break;  // meta_data (ColumnMetaData)
                default: t.skip(type); break;             // file_path, file_offset...
            }
        }
        return cc;
    }

    void parse_column_meta(ThriftReader& t, ColumnChunkMeta& cc) {
        ThriftStructScope scope(t);
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 1: cc.type = static_cast<PhysType>(t.zigzag()); break;
                case 2: { int et, sz; t.list_header(et, sz); for (int i = 0; i < sz; ++i) t.skip(et); break; }  // encodings
                case 3: { int et, sz; t.list_header(et, sz); for (int i = 0; i < sz; ++i) cc.path.push_back(t.binary()); break; }
                case 4: cc.codec = static_cast<Codec>(t.zigzag()); break;
                case 5: cc.num_values = t.zigzag(); break;
                case 9: cc.data_page_offset = t.zigzag(); break;
                case 11: cc.dictionary_page_offset = t.zigzag(); cc.has_dictionary = true; break;
                case 7: cc.total_compressed_size = t.zigzag(); break;
                case 12: parse_statistics(t, cc); break;  // Statistics
                default: t.skip(type); break;
            }
        }
    }

    // Statistics: null_count(3), distinct_count(4), max_value(5), min_value(6),
    // plus deprecated max(1)/min(2). We read the modern max_value/min_value and
    // fall back to the deprecated ones. All are PLAIN-encoded raw bytes.
    void parse_statistics(ThriftReader& t, ColumnChunkMeta& cc) {
        ThriftStructScope scope(t);
        cc.has_stats = true;
        std::string depMin, depMax, minVal, maxVal;
        bool haveMin = false, haveMax = false, haveDepMin = false, haveDepMax = false;
        int type, fid;
        while (t.field(type, fid)) {
            switch (fid) {
                case 1: depMax = t.binary(); haveDepMax = true; break;   // deprecated max
                case 2: depMin = t.binary(); haveDepMin = true; break;   // deprecated min
                case 3: cc.null_count = t.zigzag(); break;               // null_count
                case 5: maxVal = t.binary(); haveMax = true; break;      // max_value
                case 6: minVal = t.binary(); haveMin = true; break;      // min_value
                default: t.skip(type); break;
            }
        }
        if (haveMin && haveMax) { cc.min_value = minVal; cc.max_value = maxVal; cc.has_minmax = true; }
        else if (haveDepMin && haveDepMax) { cc.min_value = depMin; cc.max_value = depMax; cc.has_minmax = true; }
    }

    // Compute leaf columns + their max def/rep levels from the flattened schema.
    void build_leaf_index() {
        // schema[0] is the root group. Walk in pre-order tracking nesting via the
        // num_children counts; a node with num_children==0 && has_type is a leaf.
        // Definition level += 1 for each OPTIONAL/REPEATED ancestor; repetition
        // level += 1 for each REPEATED ancestor. A leaf with rep>0 is inside a
        // LIST. This yields the standard levels for our one-level-LIST files.
        // `topName` carries the name of the top-level (directly-under-root)
        // ancestor, which is the user-facing column name AND the column chunk's
        // path.front(). A leaf inside a LIST is schema-named "element"; we key it
        // by topName (e.g. "aprioriCovar") so callers address it naturally.
        struct Frame { int remaining; int def; int rep; std::string topName; };
        std::vector<Frame> stack;
        int rootChildren = m_meta.schema.empty() ? 0 : m_meta.schema[0].num_children;
        stack.push_back({rootChildren, 0, 0, std::string()});
        for (size_t i = 1; i < m_meta.schema.size() && stack.size() >= 1; ++i) {
            const SchemaElement& e = m_meta.schema[i];
            int def = stack.back().def;
            int rep = stack.back().rep;
            // The top-level ancestor is this node's name when the parent is root.
            std::string topName = (stack.size() == 1) ? e.name : stack.back().topName;
            if (e.repetition == Repetition::Optional) def += 1;
            else if (e.repetition == Repetition::Repeated) { def += 1; rep += 1; }
            bool isLeaf = (e.num_children == 0) && e.has_type;
            // This node occupies one child slot of the current parent.
            stack.back().remaining -= 1;
            if (isLeaf) {
                m_leaf_schema.push_back(static_cast<int>(i));
                m_leaf_name.push_back(topName);
                m_leaf_is_list.push_back(rep > 0 ? 1 : 0);
                m_leaf_max_def.push_back(def);
                m_leaf_max_rep.push_back(rep);
            } else {
                // Group node: descend into its children with the accumulated levels.
                stack.push_back({e.num_children, def, rep, topName});
            }
            // Pop any parents whose children are all consumed.
            while (stack.size() > 1 && stack.back().remaining <= 0) stack.pop_back();
        }
    }

    int leaf_index(const std::string& name) const {
        for (size_t i = 0; i < m_leaf_name.size(); ++i)
            if (m_leaf_name[i] == name) return static_cast<int>(i);
        return -1;
    }

    static ColumnData::Type to_coltype(PhysType t) {
        switch (t) {
            case PhysType::Int32: return ColumnData::Int32;
            case PhysType::Double: return ColumnData::Double;
            case PhysType::ByteArray: return ColumnData::ByteArray;
            default: throw std::runtime_error("miniparquet: unsupported physical type");
        }
    }

    // --- chunk decode ------------------------------------------------------
    void decode_chunk(size_t rgIdx, const std::string& leafName,
                      size_t localStart, size_t localEnd, ColumnData& out) {
        int leaf = leaf_index(leafName);
        const RowGroupMeta& rg = m_meta.row_groups[rgIdx];
        // Find the column chunk whose path leaf matches.
        const ColumnChunkMeta* cc = nullptr;
        for (const auto& c : rg.columns) {
            if (!c.path.empty() && c.path.front() == leafName) { cc = &c; break; }
        }
        if (cc == nullptr) throw std::runtime_error("miniparquet: column not in row group: " + leafName);

        // Byte range for the chunk: from the earliest of dict/data page offset.
        int64_t start = cc->has_dictionary && cc->dictionary_page_offset > 0
                            ? cc->dictionary_page_offset : cc->data_page_offset;
        std::vector<uint8_t> raw;
        m_reader->read_at(static_cast<size_t>(start),
                          static_cast<size_t>(cc->total_compressed_size), raw);

        ChunkDecoder dec(*cc, m_leaf_max_def[leaf], m_leaf_max_rep[leaf]);
        dec.decode(raw, static_cast<size_t>(rg.num_rows), localStart, localEnd, out);
    }

    // ------------------------------------------------------------------
    // Per-chunk page decoder.
    // ------------------------------------------------------------------
    class ChunkDecoder {
      public:
        ChunkDecoder(const ColumnChunkMeta& cc, int maxDef, int maxRep)
            : m_cc(cc), m_maxDef(maxDef), m_maxRep(maxRep) {}

        // Decode all pages, emitting only rows in [localStart, localEnd) into out.
        void decode(const std::vector<uint8_t>& raw, size_t /*rgRows*/,
                    size_t localStart, size_t localEnd, ColumnData& out) {
            ThriftReader t(raw.data(), raw.size());
            size_t rowsSeen = 0;  // top-level rows produced so far in this chunk
            while (t.pos() < raw.size()) {
                PageHeader ph = read_page_header(t);
                size_t bodyStart = t.pos();
                const uint8_t* body = raw.data() + bodyStart;
                if (bodyStart + ph.compressed_size > raw.size())
                    throw std::runtime_error("miniparquet: page body exceeds chunk");

                std::string decompressed;
                decompress(m_cc.codec, body, ph.compressed_size, ph.uncompressed_size, decompressed);

                if (ph.type == PageType::DictionaryPage) {
                    load_dictionary(reinterpret_cast<const uint8_t*>(decompressed.data()),
                                    decompressed.size(), ph.dict_num_values);
                } else if (ph.type == PageType::DataPage) {
                    decode_data_page_v1(reinterpret_cast<const uint8_t*>(decompressed.data()),
                                        decompressed.size(), ph, rowsSeen, localStart, localEnd, out);
                } else if (ph.type == PageType::DataPageV2) {
                    decode_data_page_v2(body, ph, raw, bodyStart, rowsSeen, localStart, localEnd, out);
                }
                // else index page: ignore.
                t.seek(bodyStart + ph.compressed_size);
            }
        }

      private:
        struct PageHeader {
            PageType type = PageType::DataPage;
            int32_t uncompressed_size = 0;
            int32_t compressed_size = 0;
            // data page v1/v2
            int32_t num_values = 0;
            Encoding encoding = Encoding::Plain;
            Encoding def_encoding = Encoding::Rle;
            Encoding rep_encoding = Encoding::Rle;
            // dictionary page
            int32_t dict_num_values = 0;
            // v2 extras
            int32_t num_nulls = 0;
            int32_t num_rows = 0;
            int32_t def_byte_len = 0;
            int32_t rep_byte_len = 0;
            bool v2_compressed = true;
        };

        PageHeader read_page_header(ThriftReader& t) {
            PageHeader ph;
            // Each PageHeader is a top-level struct in the chunk stream; its
            // field-ids are delta-encoded from 0, so reset the delta context.
            ThriftStructScope scope(t);
            int type, fid;
            while (t.field(type, fid)) {
                switch (fid) {
                    case 1: ph.type = static_cast<PageType>(t.zigzag()); break;
                    case 2: ph.uncompressed_size = static_cast<int32_t>(t.zigzag()); break;
                    case 3: ph.compressed_size = static_cast<int32_t>(t.zigzag()); break;
                    case 5: read_data_page_header(t, ph); break;      // DataPageHeader (v1)
                    case 7: read_dict_page_header(t, ph); break;      // DictionaryPageHeader
                    case 8: read_data_page_header_v2(t, ph); break;   // DataPageHeaderV2
                    default: t.skip(type); break;
                }
            }
            return ph;
        }
        void read_data_page_header(ThriftReader& t, PageHeader& ph) {
            ThriftStructScope scope(t);
            int type, fid;
            while (t.field(type, fid)) {
                switch (fid) {
                    case 1: ph.num_values = static_cast<int32_t>(t.zigzag()); break;
                    case 2: ph.encoding = static_cast<Encoding>(t.zigzag()); break;
                    case 3: ph.def_encoding = static_cast<Encoding>(t.zigzag()); break;
                    case 4: ph.rep_encoding = static_cast<Encoding>(t.zigzag()); break;
                    default: t.skip(type); break;
                }
            }
        }
        void read_dict_page_header(ThriftReader& t, PageHeader& ph) {
            ThriftStructScope scope(t);
            int type, fid;
            while (t.field(type, fid)) {
                switch (fid) {
                    case 1: ph.dict_num_values = static_cast<int32_t>(t.zigzag()); break;
                    case 2: /* encoding */ t.skip(type); break;
                    default: t.skip(type); break;
                }
            }
        }
        void read_data_page_header_v2(ThriftReader& t, PageHeader& ph) {
            ThriftStructScope scope(t);
            ph.v2_compressed = true;
            int type, fid;
            while (t.field(type, fid)) {
                switch (fid) {
                    case 1: ph.num_values = static_cast<int32_t>(t.zigzag()); break;
                    case 2: ph.num_nulls = static_cast<int32_t>(t.zigzag()); break;
                    case 3: ph.num_rows = static_cast<int32_t>(t.zigzag()); break;
                    case 4: ph.encoding = static_cast<Encoding>(t.zigzag()); break;
                    case 5: ph.def_byte_len = static_cast<int32_t>(t.zigzag()); break;
                    case 6: ph.rep_byte_len = static_cast<int32_t>(t.zigzag()); break;
                    // is_compressed: a compact-protocol bool carries its value in
                    // the field type nibble (BOOL_T/BOOL_F), no data byte follows.
                    case 7: ph.v2_compressed = (type == tc::BOOL_T); break;
                    default: t.skip(type); break;
                }
            }
        }

        // --- decompression ---
        static void decompress(Codec codec, const uint8_t* in, size_t inLen,
                               size_t outLen, std::string& out) {
            if (codec == Codec::Uncompressed) {
                out.assign(reinterpret_cast<const char*>(in), inLen);
                return;
            }
            if (codec == Codec::Snappy) {
                if (!snappy_lite::raw_uncompress(in, inLen, &out))
                    throw std::runtime_error("miniparquet: snappy decompress failed");
                return;
            }
            if (codec == Codec::Gzip) {
#ifdef MINIPARQUET_ENABLE_ZLIB
                out.resize(outLen);
                z_stream zs; std::memset(&zs, 0, sizeof(zs));
                // Parquet GZIP uses the gzip wrapper: windowBits 15 + 16.
                if (inflateInit2(&zs, 15 + 16) != Z_OK)
                    throw std::runtime_error("miniparquet: inflateInit2 failed");
                zs.next_in = const_cast<Bytef*>(in);
                zs.avail_in = static_cast<uInt>(inLen);
                zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
                zs.avail_out = static_cast<uInt>(outLen);
                int rc = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                if (rc != Z_STREAM_END) throw std::runtime_error("miniparquet: gzip inflate failed");
                out.resize(zs.total_out);
                return;
#else
                throw std::runtime_error("miniparquet: GZIP not enabled (define MINIPARQUET_ENABLE_ZLIB)");
#endif
            }
            throw std::runtime_error("miniparquet: unsupported compression codec (only UNCOMPRESSED/SNAPPY/GZIP)");
        }

        // --- dictionary ---
        void load_dictionary(const uint8_t* p, size_t n, int32_t numValues) {
            m_dictInt.clear(); m_dictDbl.clear(); m_dictStr.clear();
            if (m_cc.type == PhysType::Int32) {
                m_dictInt.resize(numValues);
                if (n < static_cast<size_t>(numValues) * 4) throw std::runtime_error("miniparquet: short int32 dict");
                for (int i = 0; i < numValues; ++i) { int32_t v; std::memcpy(&v, p + i * 4, 4); m_dictInt[i] = v; }
            } else if (m_cc.type == PhysType::Double) {
                m_dictDbl.resize(numValues);
                if (n < static_cast<size_t>(numValues) * 8) throw std::runtime_error("miniparquet: short double dict");
                for (int i = 0; i < numValues; ++i) { double v; std::memcpy(&v, p + i * 8, 8); m_dictDbl[i] = v; }
            } else {  // ByteArray: [len(LE u32)][bytes]...
                m_dictStr.reserve(numValues);
                size_t off = 0;
                for (int i = 0; i < numValues; ++i) {
                    if (off + 4 > n) throw std::runtime_error("miniparquet: short BA dict");
                    uint32_t len; std::memcpy(&len, p + off, 4); off += 4;
                    if (off + len > n) throw std::runtime_error("miniparquet: short BA dict body");
                    m_dictStr.emplace_back(reinterpret_cast<const char*>(p + off), len);
                    off += len;
                }
            }
            m_haveDict = true;
        }

        // --- data page v1 ---
        void decode_data_page_v1(const uint8_t* p, size_t n, const PageHeader& ph,
                                 size_t& rowsSeen, size_t localStart, size_t localEnd,
                                 ColumnData& out) {
            size_t off = 0;
            std::vector<int32_t> repLevels, defLevels;
            // In v1 the rep and def levels are length-prefixed (RLE) inside the body.
            if (m_maxRep > 0)
                repLevels = read_rle_levels_v1(p, n, off, ph.num_values, bit_width(m_maxRep));
            if (m_maxDef > 0)
                defLevels = read_rle_levels_v1(p, n, off, ph.num_values, bit_width(m_maxDef));
            emit_values(p + off, n - off, ph, repLevels, defLevels,
                        rowsSeen, localStart, localEnd, out);
        }

        // --- data page v2 ---
        void decode_data_page_v2(const uint8_t* body, const PageHeader& ph,
                                 const std::vector<uint8_t>& /*raw*/, size_t /*bodyStart*/,
                                 size_t& rowsSeen, size_t localStart, size_t localEnd,
                                 ColumnData& out) {
            // In v2, rep+def levels are always uncompressed and precede the
            // (possibly compressed) values. Layout: [rep bytes][def bytes][values].
            const uint8_t* p = body;
            std::vector<int32_t> repLevels, defLevels;
            size_t off = 0;
            if (ph.rep_byte_len > 0)
                repLevels = read_bitpacked_hybrid(p + off, ph.rep_byte_len, ph.num_values, bit_width(m_maxRep));
            off += ph.rep_byte_len;
            if (ph.def_byte_len > 0)
                defLevels = read_bitpacked_hybrid(p + off, ph.def_byte_len, ph.num_values, bit_width(m_maxDef));
            off += ph.def_byte_len;
            // Values region. In v2, only the values are (optionally) compressed;
            // the level sections are always stored raw and excluded from the
            // codec. compressed_size counts levels+values; uncompressed_size too.
            size_t valCompLen = static_cast<size_t>(ph.compressed_size) - off;
            std::string vals;
            if (ph.v2_compressed && m_cc.codec != Codec::Uncompressed) {
                size_t valUncompLen = static_cast<size_t>(ph.uncompressed_size) - off;
                decompress(m_cc.codec, p + off, valCompLen, valUncompLen, vals);
            } else {
                vals.assign(reinterpret_cast<const char*>(p + off), valCompLen);
            }
            emit_values_split(reinterpret_cast<const uint8_t*>(vals.data()), vals.size(),
                              ph, repLevels, defLevels, rowsSeen, localStart, localEnd, out);
        }

        // Shared value emission for v1 (levels + values already positioned).
        void emit_values(const uint8_t* valp, size_t valn, const PageHeader& ph,
                          const std::vector<int32_t>& rep, const std::vector<int32_t>& def,
                          size_t& rowsSeen, size_t localStart, size_t localEnd, ColumnData& out) {
            emit_values_split(valp, valn, ph, rep, def, rowsSeen, localStart, localEnd, out);
        }

        // Core: walk num_values, using rep/def levels to group into top-level
        // rows and decide null/present, pulling actual values (PLAIN or dict).
        void emit_values_split(const uint8_t* valp, size_t valn, const PageHeader& ph,
                               const std::vector<int32_t>& rep, const std::vector<int32_t>& def,
                               size_t& rowsSeen, size_t localStart, size_t localEnd, ColumnData& out) {
            // Count of non-null values in this page (def == maxDef).
            // Pull all present values up front from the value section.
            ValueCursor vc(*this, valp, valn, ph.encoding, count_present(def, ph.num_values));

            if (m_maxRep == 0) {
                // Flat column: each value index is one top-level row.
                for (int i = 0; i < ph.num_values; ++i) {
                    bool present = m_maxDef == 0 ? true : (def[i] == m_maxDef);
                    size_t row = rowsSeen;
                    ++rowsSeen;
                    bool want = (row >= localStart && row < localEnd);
                    if (!want) { if (present) vc.advance(); continue; }
                    if (present) { push_value(vc, out); out.present.push_back(1); }
                    else { push_null(out); out.present.push_back(0); }
                }
            } else {
                // List column: a new top-level row starts when rep level == 0.
                // Accumulate element counts + values per row.
                int i = 0;
                while (i < ph.num_values) {
                    // start of a row (rep==0). gather until next rep==0.
                    size_t row = rowsSeen;
                    ++rowsSeen;
                    bool want = (row >= localStart && row < localEnd);
                    int32_t count = 0;
                    bool rowPresent = false;
                    do {
                        bool present = (def[i] == m_maxDef);  // element present
                        // def == maxDef-? : distinguish empty list vs null list vs present element.
                        // For LIST optional<element optional>: maxDef covers element present.
                        if (present) {
                            rowPresent = true;
                            if (want) { push_value(vc, out); }
                            else vc.advance();
                            ++count;
                        } else {
                            // def < maxDef: either null list or empty list; no value.
                            // Only counts as an element if def indicates a present-but-null element.
                        }
                        ++i;
                    } while (i < ph.num_values && rep[i] != 0);
                    if (want) {
                        out.listLen.push_back(count);
                        out.present.push_back(rowPresent ? 1 : 0);
                    }
                }
            }
        }

        // Cursor over present values in a page, PLAIN or dictionary-indexed.
        struct ValueCursor {
            ChunkDecoder& dec;
            const uint8_t* p;
            size_t n;
            Encoding enc;
            int total;
            // PLAIN cursors
            size_t off = 0;
            // dictionary index cursor
            std::vector<int32_t> dictIdx;
            size_t idxPos = 0;

            ValueCursor(ChunkDecoder& d, const uint8_t* pp, size_t nn, Encoding e, int totalPresent)
                : dec(d), p(pp), n(nn), enc(e), total(totalPresent) {
                if (enc == Encoding::RleDictionary || enc == Encoding::PlainDictionary) {
                    // First byte = bit width, then RLE/bitpacked-hybrid of `total` indices.
                    if (n == 0) return;
                    int bw = p[0];
                    dictIdx = dec.read_bitpacked_hybrid(p + 1, n - 1, total, bw);
                }
            }
            void advance() {
                if (enc == Encoding::Plain) {
                    if (dec.m_cc.type == PhysType::Int32) off += 4;
                    else if (dec.m_cc.type == PhysType::Double) off += 8;
                    else {
                        if (off + 4 > n) throw std::runtime_error("miniparquet: PLAIN string overrun");
                        uint32_t l; std::memcpy(&l, p + off, 4); off += 4 + l;
                    }
                } else { ++idxPos; }
            }
        };

        void push_value(ValueCursor& vc, ColumnData& out) {
            if (vc.enc == Encoding::Plain) {
                if (m_cc.type == PhysType::Int32) {
                    if (vc.off + 4 > vc.n) throw std::runtime_error("miniparquet: PLAIN int32 overrun");
                    int32_t v; std::memcpy(&v, vc.p + vc.off, 4); vc.off += 4; out.i32.push_back(v);
                } else if (m_cc.type == PhysType::Double) {
                    if (vc.off + 8 > vc.n) throw std::runtime_error("miniparquet: PLAIN double overrun");
                    double v; std::memcpy(&v, vc.p + vc.off, 8); vc.off += 8; out.f64.push_back(v);
                } else {
                    if (vc.off + 4 > vc.n) throw std::runtime_error("miniparquet: PLAIN string len overrun");
                    uint32_t l; std::memcpy(&l, vc.p + vc.off, 4); vc.off += 4;
                    if (vc.off + l > vc.n) throw std::runtime_error("miniparquet: PLAIN string body overrun");
                    out.str.emplace_back(reinterpret_cast<const char*>(vc.p + vc.off), l); vc.off += l;
                }
            } else {  // dictionary
                if (vc.idxPos >= vc.dictIdx.size()) throw std::runtime_error("miniparquet: dictionary index overrun");
                int32_t idx = vc.dictIdx[vc.idxPos++];
                if (m_cc.type == PhysType::Int32) out.i32.push_back(m_dictInt.at(idx));
                else if (m_cc.type == PhysType::Double) out.f64.push_back(m_dictDbl.at(idx));
                else out.str.push_back(m_dictStr.at(idx));
            }
        }
        void push_null(ColumnData& out) {
            if (m_cc.type == PhysType::Int32) out.i32.push_back(0);
            else if (m_cc.type == PhysType::Double) out.f64.push_back(0.0);
            else out.str.emplace_back();
        }

        // Number of values physically present in this page = count of entries at
        // the (schema-derived) max definition level. Using m_maxDef, not an
        // observed max, is essential: an all-null page still has 0 present values.
        int count_present(const std::vector<int32_t>& def, int numValues) const {
            if (m_maxDef == 0) return numValues;  // required column -> all present
            int c = 0; for (int i = 0; i < numValues; ++i) if (def[i] == m_maxDef) ++c; return c;
        }

        static int bit_width(int maxLevel) {
            int bw = 0; while ((1 << bw) <= maxLevel) ++bw; return bw;
        }

        // Read the v1 level section: 4-byte LE length prefix, then a
        // bit-packed/RLE hybrid of `count` values at `bitW` bits.
        std::vector<int32_t> read_rle_levels_v1(const uint8_t* p, size_t n, size_t& off,
                                                int count, int bitW) {
            if (bitW == 0) return std::vector<int32_t>(count, 0);
            if (off + 4 > n) throw std::runtime_error("miniparquet: short level prefix");
            uint32_t len; std::memcpy(&len, p + off, 4); off += 4;
            if (off + len > n) throw std::runtime_error("miniparquet: short level data");
            std::vector<int32_t> lv = read_bitpacked_hybrid(p + off, len, count, bitW);
            off += len;
            return lv;
        }

        // RLE / bit-packed hybrid decoder (Parquet's "RLE" hybrid), producing
        // `count` values. Used for def/rep levels and dictionary indices.
        std::vector<int32_t> read_bitpacked_hybrid(const uint8_t* p, size_t n, int count, int bitW) {
            std::vector<int32_t> out;
            out.reserve(count);
            if (bitW == 0) { out.assign(count, 0); return out; }
            size_t pos = 0;
            const int byteW = (bitW + 7) / 8;
            while (static_cast<int>(out.size()) < count && pos < n) {
                uint64_t header = read_uleb(p, n, pos);
                if (header & 1) {
                    // bit-packed run: (header>>1) groups of 8 values.
                    int groups = static_cast<int>(header >> 1);
                    int nvals = groups * 8;
                    int bitsAvail = 0; uint64_t bitbuf = 0;
                    for (int i = 0; i < nvals && static_cast<int>(out.size()) < count; ++i) {
                        while (bitsAvail < bitW) {
                            if (pos >= n) throw std::runtime_error("miniparquet: bitpack underrun");
                            bitbuf |= static_cast<uint64_t>(p[pos++]) << bitsAvail;
                            bitsAvail += 8;
                        }
                        out.push_back(static_cast<int32_t>(bitbuf & ((1u << bitW) - 1)));
                        bitbuf >>= bitW; bitsAvail -= bitW;
                    }
                    // If nvals > count, the extra padded values are discarded above.
                } else {
                    // RLE run: (header>>1) repeats of a value of byteW bytes.
                    int runLen = static_cast<int>(header >> 1);
                    if (pos + byteW > n) throw std::runtime_error("miniparquet: rle underrun");
                    int32_t val = 0;
                    for (int b = 0; b < byteW; ++b) val |= static_cast<int32_t>(p[pos + b]) << (8 * b);
                    pos += byteW;
                    for (int i = 0; i < runLen && static_cast<int>(out.size()) < count; ++i)
                        out.push_back(val);
                }
            }
            if (static_cast<int>(out.size()) < count) out.resize(count, 0);
            return out;
        }

        static uint64_t read_uleb(const uint8_t* p, size_t n, size_t& pos) {
            uint64_t r = 0; int shift = 0;
            while (true) {
                if (pos >= n) throw std::runtime_error("miniparquet: uleb underrun");
                uint8_t b = p[pos++];
                r |= static_cast<uint64_t>(b & 0x7F) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            return r;
        }

        const ColumnChunkMeta& m_cc;
        int m_maxDef, m_maxRep;
        bool m_haveDict = false;
        std::vector<int32_t> m_dictInt;
        std::vector<double> m_dictDbl;
        std::vector<std::string> m_dictStr;
    };
};

}  // namespace mparq

#endif  // MINIPARQUET_HPP
