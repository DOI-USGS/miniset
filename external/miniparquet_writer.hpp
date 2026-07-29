// miniparquet_writer.hpp — a header-only, dependency-light C++17 Parquet *writer*,
// the inverse of miniparquet.hpp. Together they let miniset read and write the
// control-network Parquet schema with no GDAL/Arrow dependency.
//
// Scope (matches the reader — tailored to miniset's control networks):
//   * Physical types INT32, DOUBLE, BYTE_ARRAY(utf8); one level of LIST nesting.
//   * Optional (nullable) columns via definition levels; LIST columns via
//     repetition levels. All columns are OPTIONAL (matching the Arrow writer).
//   * Encoding: PLAIN values, RLE/bit-packed-hybrid def/rep levels.
//   * Compression: UNCOMPRESSED, or GZIP if MINIPARQUET_ENABLE_ZLIB.
//
// Memory & scale:
//   * STREAMING: data is written to a ByteSink as it is produced (a file stream,
//     so the whole file is NOT held in RAM), one ROW GROUP at a time. The caller
//     feeds bounded row-group batches, so peak RAM is one row group, not the file.
//   * Each column chunk is split into BOUNDED DATA PAGES (page byte size stays
//     well under the Parquet int32 page-size limit — the old single-page writer
//     silently corrupted files whose column exceeded ~2 GB).
//   * Per-column-chunk STATISTICS (min/max/null_count) are written into each
//     row group, so a reader can prune whole row groups for range queries
//     (e.g. "sample BETWEEN a AND b") without touching data pages.
//
// The emitted file is a standard Parquet v1 file (PAR1 ... thrift FileMetaData
// ... PAR1) that Arrow/parquet-cpp, DuckDB, pandas, etc. read normally.

#ifndef MINIPARQUET_WRITER_HPP
#define MINIPARQUET_WRITER_HPP

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniparquet.hpp"  // enums (PhysType/Encoding/Codec/Repetition), tc::*

#ifdef MINIPARQUET_ENABLE_ZLIB
#include <zlib.h>
#endif

namespace mparq {

// ===========================================================================
// A column's data for ONE row group. `present` (per top-level row) drives
// definition levels; list columns supply `listLen` (per-row element count) and
// flattened element values.
// ===========================================================================
struct WriteColumn {
    std::string name;                 // top-level column name
    PhysType type = PhysType::Int32;
    bool is_list = false;             // wrap as LIST<optional element>
    std::vector<int32_t> i32;
    std::vector<double> f64;
    std::vector<std::string> str;
    std::vector<uint8_t> present;     // flat cols: presence per row (empty => all present)
    std::vector<int32_t> listLen;     // list cols: element count per row
};

// Column identity for the schema (set once, before any row group).
struct ColumnSchema {
    std::string name;
    PhysType type = PhysType::Int32;
    bool is_list = false;
};

// ===========================================================================
// ByteSink — where the writer streams bytes. Two impls: a growing in-memory
// vector (WASM / tests / small files) and a file stream (bounded RAM).
// ===========================================================================
class ByteSink {
  public:
    virtual ~ByteSink() = default;
    virtual void write(const char* data, size_t len) = 0;
    virtual size_t tell() const = 0;
    void write(const std::string& s) { write(s.data(), s.size()); }
};

class VectorSink : public ByteSink {
  public:
    void write(const char* data, size_t len) override {
        m_buf.insert(m_buf.end(), data, data + len);
    }
    size_t tell() const override { return m_buf.size(); }
    std::vector<uint8_t> take() { return std::move(m_buf); }

  private:
    std::vector<uint8_t> m_buf;
};

class FileSink : public ByteSink {
  public:
    explicit FileSink(const std::string& path)
        : m_f(path, std::ios::binary | std::ios::trunc) {
        if (!m_f) throw std::runtime_error("miniparquet_writer: cannot open " + path);
    }
    void write(const char* data, size_t len) override {
        m_f.write(data, static_cast<std::streamsize>(len));
        m_pos += len;
    }
    size_t tell() const override { return m_pos; }

  private:
    std::ofstream m_f;
    size_t m_pos = 0;
};

// ===========================================================================
// Thrift compact-protocol writer (mirror of miniparquet.hpp's ThriftReader).
// ===========================================================================
class ThriftWriter {
  public:
    explicit ThriftWriter(std::string& out) : m_out(out) {}

    void field_begin(int type, int fid) {
        int delta = fid - m_last_fid;
        if (delta > 0 && delta <= 15) {
            m_out.push_back(static_cast<char>((delta << 4) | (type & 0x0F)));
        } else {
            m_out.push_back(static_cast<char>(type & 0x0F));
            zigzag(fid);
        }
        m_last_fid = fid;
    }
    void field_stop() { m_out.push_back(0); }

    void varint(uint64_t v) {
        while (v >= 0x80) { m_out.push_back(static_cast<char>((v & 0x7F) | 0x80)); v >>= 7; }
        m_out.push_back(static_cast<char>(v));
    }
    void zigzag(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ (v >> 63)); }
    void raw(const std::string& s) { m_out.append(s); }

    void i32_field(int fid, int32_t v) { field_begin(tc::I32, fid); zigzag(v); }
    void i64_field(int fid, int64_t v) { field_begin(tc::I64, fid); zigzag(v); }
    void bool_field(int fid, bool v) { field_begin(v ? tc::BOOL_T : tc::BOOL_F, fid); }
    void binary_field(int fid, const std::string& s) {
        field_begin(tc::BINARY, fid);
        varint(s.size());
        m_out.append(s);
    }
    void list_begin(int fid, int elemType, size_t size) {
        field_begin(tc::LIST, fid);
        if (size < 15) {
            m_out.push_back(static_cast<char>((static_cast<int>(size) << 4) | (elemType & 0x0F)));
        } else {
            m_out.push_back(static_cast<char>(0xF0 | (elemType & 0x0F)));
            varint(size);
        }
    }
    int enter_struct() { int saved = m_last_fid; m_last_fid = 0; return saved; }
    void leave_struct(int saved) { field_stop(); m_last_fid = saved; }
    int elem_struct_begin() { int saved = m_last_fid; m_last_fid = 0; return saved; }
    void elem_struct_end(int saved) { field_stop(); m_last_fid = saved; }

  private:
    std::string& m_out;
    int m_last_fid = 0;
};

// ===========================================================================
// Options.
// ===========================================================================
struct WriterOptions {
    Codec codec = Codec::Uncompressed;
    // Data-page bound: values are flushed to a new page every `maxPageRows` top-
    // level rows, and eagerly if the page body would exceed `maxPageBytes`. This
    // keeps every page well under the Parquet int32 page-size limit and gives
    // page-level read granularity.
    size_t maxPageRows = 100000;
    size_t maxPageBytes = 64u * 1024u * 1024u;  // 64 MiB
};

// ===========================================================================
// StreamWriter — write a Parquet file one row group at a time to a ByteSink.
// ===========================================================================
class StreamWriter {
  public:
    StreamWriter(ByteSink& sink, std::vector<ColumnSchema> schema, WriterOptions opts = {})
        : m_sink(sink), m_schema(std::move(schema)), m_opts(opts) {
#ifndef MINIPARQUET_ENABLE_ZLIB
        if (m_opts.codec == Codec::Gzip)
            throw std::runtime_error("miniparquet_writer: GZIP requires MINIPARQUET_ENABLE_ZLIB");
#endif
        if (m_opts.codec != Codec::Uncompressed && m_opts.codec != Codec::Gzip)
            throw std::runtime_error("miniparquet_writer: only UNCOMPRESSED/GZIP supported");
        m_sink.write("PAR1", 4);
    }

    // Append one row group. `cols` must match the schema by position; each column
    // carries exactly `numRows` top-level rows. Encodes bounded pages and streams
    // them to the sink immediately, recording (small) per-chunk metadata.
    void writeRowGroup(const std::vector<WriteColumn>& cols, size_t numRows) {
        if (numRows == 0) return;
        if (cols.size() != m_schema.size())
            throw std::runtime_error("miniparquet_writer: row-group column count != schema");
        RGMeta rg;
        rg.num_rows = static_cast<int64_t>(numRows);
        rg.chunks.resize(cols.size());
        for (size_t c = 0; c < cols.size(); ++c) {
            encode_column_chunk(cols[c], numRows, rg.chunks[c]);
        }
        m_totalRows += static_cast<int64_t>(numRows);
        m_rowGroups.push_back(std::move(rg));
    }

    // Write the footer and finalize. After this the sink holds a complete file.
    void finish() {
        std::string footer;
        ThriftWriter t(footer);
        t.i32_field(1, 1);                          // version
        write_schema(t);                            // field 2: schema
        t.i64_field(3, m_totalRows);                // num_rows
        write_row_groups(t);                        // field 4: row_groups
        t.binary_field(6, "miniparquet");           // created_by
        t.field_stop();

        uint32_t footLen = static_cast<uint32_t>(footer.size());
        m_sink.write(footer);
        char lenLE[4]; std::memcpy(lenLE, &footLen, 4);
        m_sink.write(lenLE, 4);
        m_sink.write("PAR1", 4);
    }

  private:
    ByteSink& m_sink;
    std::vector<ColumnSchema> m_schema;
    WriterOptions m_opts;
    int64_t m_totalRows = 0;

    struct ChunkMeta {
        int64_t data_page_offset = 0;
        int64_t total_compressed = 0;
        int64_t total_uncompressed = 0;
        int64_t num_values = 0;
        bool has_stats = false;
        int64_t null_count = 0;
        std::string min_value, max_value;  // PLAIN-encoded (numeric) / raw (bytes)
    };
    struct RGMeta {
        std::vector<ChunkMeta> chunks;
        int64_t num_rows = 0;
    };
    std::vector<RGMeta> m_rowGroups;

    static int max_def(const ColumnSchema& c) { return c.is_list ? 3 : 1; }
    static int max_rep(const ColumnSchema& c) { return c.is_list ? 1 : 0; }
    static int max_def(const WriteColumn& c) { return c.is_list ? 3 : 1; }
    static int max_rep(const WriteColumn& c) { return c.is_list ? 1 : 0; }
    static int bit_width(int maxLevel) { int bw = 0; while ((1 << bw) <= maxLevel) ++bw; return bw; }

    // Running min/max/null tracker for a column chunk's statistics.
    struct Stats {
        PhysType type;
        bool seen = false;
        int64_t nulls = 0;
        int32_t iMin = 0, iMax = 0;
        double dMin = 0, dMax = 0;
        std::string sMin, sMax;
        explicit Stats(PhysType t) : type(t) {}
        void addInt(int32_t v) {
            if (!seen || v < iMin) iMin = v;
            if (!seen || v > iMax) iMax = v;
            seen = true;
        }
        void addDbl(double v) {
            if (!seen || v < dMin) dMin = v;
            if (!seen || v > dMax) dMax = v;
            seen = true;
        }
        void addStr(const std::string& v) {
            if (!seen || v < sMin) sMin = v;
            if (!seen || v > sMax) sMax = v;
            seen = true;
        }
        std::string minBytes() const { return encode(type, iMin, dMin, sMin); }
        std::string maxBytes() const { return encode(type, iMax, dMax, sMax); }
        static std::string encode(PhysType t, int32_t iv, double dv, const std::string& sv) {
            if (t == PhysType::Int32) { std::string b(4, '\0'); std::memcpy(&b[0], &iv, 4); return b; }
            if (t == PhysType::Double) { std::string b(8, '\0'); std::memcpy(&b[0], &dv, 8); return b; }
            return sv;  // BYTE_ARRAY: raw bytes
        }
    };

    // Encode a column chunk (bounded pages) to the sink, filling `out` metadata.
    void encode_column_chunk(const WriteColumn& col, size_t numRows, ChunkMeta& out) {
        out.data_page_offset = static_cast<int64_t>(m_sink.tell());
        Stats stats(col.type);
        // Value cursors advance continuously across pages.
        size_t vi32 = 0, vf64 = 0, vstr = 0;

        size_t row = 0;
        while (row < numRows) {
            size_t pageRowBegin = row;
            std::vector<int32_t> rep, def;
            std::string values;
            size_t pageRows = 0;
            // Accumulate rows into this page until a bound trips.
            while (row < numRows && pageRows < m_opts.maxPageRows &&
                   values.size() < m_opts.maxPageBytes) {
                emit_row(col, row, rep, def, values, vi32, vf64, vstr, stats);
                ++row; ++pageRows;
            }
            (void)pageRowBegin;
            int64_t numValues = static_cast<int64_t>(col.is_list ? def.size() : def.size());
            flush_page(col, rep, def, values, numValues, out);
        }

        out.num_values = out.num_values;  // accumulated in flush_page
        // Statistics only for non-list (scalar) columns — those are the query
        // targets; list (covariance/log) columns get none.
        if (!col.is_list && stats.seen) {
            out.has_stats = true;
            out.null_count = stats.nulls;
            out.min_value = stats.minBytes();
            out.max_value = stats.maxBytes();
        } else if (!col.is_list) {
            // All-null scalar column: still record null_count.
            out.has_stats = true;
            out.null_count = stats.nulls;
        }
    }

    // Emit one top-level row's levels + present values into the page buffers.
    void emit_row(const WriteColumn& col, size_t r,
                  std::vector<int32_t>& rep, std::vector<int32_t>& def, std::string& values,
                  size_t& vi32, size_t& vf64, size_t& vstr, Stats& stats) {
        auto emit_value = [&]() {
            switch (col.type) {
                case PhysType::Int32: {
                    int32_t v = (vi32 < col.i32.size()) ? col.i32[vi32++] : 0;
                    char b[4]; std::memcpy(b, &v, 4); values.append(b, 4);
                    if (!col.is_list) stats.addInt(v);
                    break;
                }
                case PhysType::Double: {
                    double v = (vf64 < col.f64.size()) ? col.f64[vf64++] : 0.0;
                    char b[8]; std::memcpy(b, &v, 8); values.append(b, 8);
                    if (!col.is_list) stats.addDbl(v);
                    break;
                }
                case PhysType::ByteArray: {
                    const std::string& s = (vstr < col.str.size()) ? col.str[vstr++] : k_empty;
                    uint32_t len = static_cast<uint32_t>(s.size());
                    char b[4]; std::memcpy(b, &len, 4); values.append(b, 4);
                    values.append(s);
                    if (!col.is_list) stats.addStr(s);
                    break;
                }
                default: throw std::runtime_error("miniparquet_writer: unsupported physical type");
            }
        };
        if (!col.is_list) {
            bool present = col.present.empty() ? true : (r < col.present.size() && col.present[r]);
            def.push_back(present ? 1 : 0);
            if (present) emit_value(); else stats.nulls += 1;
        } else {
            int32_t n = (r < col.listLen.size()) ? col.listLen[r] : 0;
            if (n <= 0) {
                def.push_back(1);   // empty (non-null) list
                rep.push_back(0);
            } else {
                for (int32_t k = 0; k < n; ++k) {
                    def.push_back(3);
                    rep.push_back(k == 0 ? 0 : 1);
                    emit_value();
                }
            }
        }
    }

    // Encode + compress one DATA_PAGE v1 and stream it; accumulate chunk sizes.
    void flush_page(const WriteColumn& col, const std::vector<int32_t>& rep,
                    const std::vector<int32_t>& def, const std::string& values,
                    int64_t numValues, ChunkMeta& out) {
        std::string body;
        if (max_rep(col) > 0) append_rle_level_section(body, rep, bit_width(max_rep(col)));
        if (max_def(col) > 0) append_rle_level_section(body, def, bit_width(max_def(col)));
        body.append(values);

        if (body.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("miniparquet_writer: page body exceeds int32 (raise page bounds?)");

        std::string compressed = compress_body(body);
        if (compressed.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("miniparquet_writer: compressed page exceeds int32");

        std::string hdr;
        ThriftWriter t(hdr);
        t.i32_field(1, static_cast<int32_t>(PageType::DataPage));
        t.i32_field(2, static_cast<int32_t>(body.size()));         // uncompressed_page_size
        t.i32_field(3, static_cast<int32_t>(compressed.size()));   // compressed_page_size
        t.field_begin(tc::STRUCT, 5);                              // DataPageHeader
        int saved = t.enter_struct();
        t.i32_field(1, static_cast<int32_t>(numValues));
        t.i32_field(2, static_cast<int32_t>(Encoding::Plain));
        t.i32_field(3, static_cast<int32_t>(Encoding::Rle));       // def level encoding
        t.i32_field(4, static_cast<int32_t>(Encoding::Rle));       // rep level encoding
        t.leave_struct(saved);
        t.field_stop();

        m_sink.write(hdr);
        m_sink.write(compressed);
        out.total_uncompressed += static_cast<int64_t>(hdr.size() + body.size());
        out.total_compressed += static_cast<int64_t>(hdr.size() + compressed.size());
        out.num_values += numValues;
    }

    void append_rle_level_section(std::string& out, const std::vector<int32_t>& levels, int bitW) {
        if (bitW == 0) { uint32_t z = 0; char b[4]; std::memcpy(b, &z, 4); out.append(b, 4); return; }
        std::string enc = encode_bitpacked_hybrid(levels, bitW);
        uint32_t len = static_cast<uint32_t>(enc.size());
        char b[4]; std::memcpy(b, &len, 4); out.append(b, 4);
        out.append(enc);
    }

    std::string encode_bitpacked_hybrid(const std::vector<int32_t>& values, int bitW) {
        std::string out;
        size_t n = values.size();
        size_t groups = (n + 7) / 8;
        uint64_t header = (static_cast<uint64_t>(groups) << 1) | 1;
        while (header >= 0x80) { out.push_back(static_cast<char>((header & 0x7F) | 0x80)); header >>= 7; }
        out.push_back(static_cast<char>(header));
        uint64_t bitbuf = 0; int bits = 0;
        size_t total = groups * 8;
        for (size_t i = 0; i < total; ++i) {
            uint32_t v = (i < n) ? static_cast<uint32_t>(values[i]) : 0u;
            bitbuf |= static_cast<uint64_t>(v & ((bitW == 32) ? 0xFFFFFFFFu : ((1u << bitW) - 1))) << bits;
            bits += bitW;
            while (bits >= 8) { out.push_back(static_cast<char>(bitbuf & 0xFF)); bitbuf >>= 8; bits -= 8; }
        }
        if (bits > 0) out.push_back(static_cast<char>(bitbuf & 0xFF));
        return out;
    }

    std::string compress_body(const std::string& body) {
        if (m_opts.codec == Codec::Uncompressed) return body;
#ifdef MINIPARQUET_ENABLE_ZLIB
        if (m_opts.codec == Codec::Gzip) {
            // compressBound() sizes the raw deflate stream; the gzip wrapper adds
            // a 10-byte header + 8-byte trailer (+ up to a few bytes), which for
            // tiny inputs can exceed that bound and make deflate() stop at Z_OK
            // (out of output) instead of Z_STREAM_END. Add slack for the wrapper.
            uLongf bound = compressBound(static_cast<uLong>(body.size())) + 64;
            std::string out; out.resize(bound);
            z_stream zs; std::memset(&zs, 0, sizeof(zs));
            if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                throw std::runtime_error("miniparquet_writer: deflateInit2 failed");
            zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(body.data()));
            zs.avail_in = static_cast<uInt>(body.size());
            zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
            zs.avail_out = static_cast<uInt>(out.size());
            int rc = deflate(&zs, Z_FINISH);
            deflateEnd(&zs);
            if (rc != Z_STREAM_END) throw std::runtime_error("miniparquet_writer: gzip deflate failed");
            out.resize(zs.total_out);
            return out;
        }
#endif
        throw std::runtime_error("miniparquet_writer: unsupported codec");
    }

    // --- schema (from m_schema) ---
    enum { CT_NONE = -1, CT_UTF8 = 0, CT_LIST = 3 };
    void write_schema(ThriftWriter& t) {
        size_t count = 1;
        for (const auto& c : m_schema) count += c.is_list ? 3 : 1;
        t.list_begin(2, tc::STRUCT, count);
        write_schema_element(t, false, PhysType::Int32, Repetition::Required,
                             "schema", static_cast<int>(m_schema.size()), CT_NONE);
        for (const auto& c : m_schema) {
            int leafCT = (c.type == PhysType::ByteArray) ? CT_UTF8 : CT_NONE;
            if (!c.is_list) {
                write_schema_element(t, true, c.type, Repetition::Optional, c.name, 0, leafCT);
            } else {
                write_schema_element(t, false, PhysType::Int32, Repetition::Optional, c.name, 1, CT_LIST);
                write_schema_element(t, false, PhysType::Int32, Repetition::Repeated, "list", 1, CT_NONE);
                write_schema_element(t, true, c.type, Repetition::Optional, "element", 0, leafCT);
            }
        }
    }
    void write_schema_element(ThriftWriter& t, bool hasType, PhysType type,
                              Repetition rep, const std::string& name, int numChildren,
                              int convertedType) {
        int saved = t.elem_struct_begin();
        if (hasType) t.i32_field(1, static_cast<int32_t>(type));
        t.i32_field(3, static_cast<int32_t>(rep));
        t.binary_field(4, name);
        if (numChildren > 0) t.i32_field(5, numChildren);
        if (convertedType != CT_NONE) t.i32_field(6, convertedType);
        t.elem_struct_end(saved);
    }

    // --- row groups ---
    void write_row_groups(ThriftWriter& t) {
        t.list_begin(4, tc::STRUCT, m_rowGroups.size());
        for (const auto& rg : m_rowGroups) {
            int savedRG = t.elem_struct_begin();
            t.list_begin(1, tc::STRUCT, m_schema.size());  // columns
            int64_t totalBytes = 0;
            for (const auto& ci : rg.chunks) totalBytes += ci.total_compressed;
            for (size_t i = 0; i < m_schema.size(); ++i) {
                const ChunkMeta& ci = rg.chunks[i];
                int savedCC = t.elem_struct_begin();
                t.i64_field(2, ci.data_page_offset);       // file_offset
                t.field_begin(tc::STRUCT, 3);              // meta_data
                int savedMD = t.enter_struct();
                t.i32_field(1, static_cast<int32_t>(m_schema[i].type));
                t.list_begin(2, tc::I32, 2);               // encodings [PLAIN, RLE]
                t.zigzag(static_cast<int32_t>(Encoding::Plain));
                t.zigzag(static_cast<int32_t>(Encoding::Rle));
                write_path(t, m_schema[i]);                // path_in_schema
                t.i32_field(4, static_cast<int32_t>(m_opts.codec));
                t.i64_field(5, ci.num_values);
                t.i64_field(6, ci.total_uncompressed);
                t.i64_field(7, ci.total_compressed);
                t.i64_field(9, ci.data_page_offset);       // data_page_offset
                if (ci.has_stats) write_statistics(t, ci); // field 12
                t.leave_struct(savedMD);
                t.elem_struct_end(savedCC);
            }
            t.i64_field(2, totalBytes);                    // total_byte_size
            t.i64_field(3, rg.num_rows);                   // num_rows
            t.elem_struct_end(savedRG);
        }
    }

    // Statistics struct: null_count(3), max_value(5), min_value(6).
    void write_statistics(ThriftWriter& t, const ChunkMeta& ci) {
        t.field_begin(tc::STRUCT, 12);
        int saved = t.enter_struct();
        t.i64_field(3, ci.null_count);
        if (!ci.max_value.empty() || !ci.min_value.empty()) {
            t.binary_field(5, ci.max_value);
            t.binary_field(6, ci.min_value);
        }
        t.leave_struct(saved);
    }

    void write_path(ThriftWriter& t, const ColumnSchema& col) {
        if (!col.is_list) {
            t.list_begin(3, tc::BINARY, 1);
            t.varint(col.name.size()); t.raw(col.name);
        } else {
            t.list_begin(3, tc::BINARY, 3);
            t.varint(col.name.size()); t.raw(col.name);
            t.varint(4); t.raw("list");
            t.varint(7); t.raw("element");
        }
    }

    static const std::string k_empty;
};

inline const std::string StreamWriter::k_empty = std::string();

// ===========================================================================
// Convenience wrapper: write all columns as a single row group to bytes / file.
// (Bounded pages still apply. For multi-GB data, use StreamWriter directly and
// feed bounded row groups.)
// ===========================================================================
class Writer {
  public:
    explicit Writer(Codec codec = Codec::Uncompressed) { m_opts.codec = codec; }

    std::vector<uint8_t> writeBytes(const std::vector<WriteColumn>& columns, size_t numRows) {
        VectorSink sink;
        StreamWriter w(sink, schemaOf(columns), m_opts);
        w.writeRowGroup(columns, numRows);
        w.finish();
        return sink.take();
    }
    void writeFile(const std::vector<WriteColumn>& columns, size_t numRows, const std::string& path) {
        FileSink sink(path);
        StreamWriter w(sink, schemaOf(columns), m_opts);
        w.writeRowGroup(columns, numRows);
        w.finish();
    }

  private:
    WriterOptions m_opts;
    static std::vector<ColumnSchema> schemaOf(const std::vector<WriteColumn>& cols) {
        std::vector<ColumnSchema> s;
        s.reserve(cols.size());
        for (const auto& c : cols) s.push_back({c.name, c.type, c.is_list});
        return s;
    }
};

}  // namespace mparq

#endif  // MINIPARQUET_WRITER_HPP
