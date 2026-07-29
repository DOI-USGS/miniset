// snappy_decompress.hpp — a tiny, self-contained, header-only Snappy *raw block*
// decompressor (C++17). Decompression only (no compressor), which is all a
// Parquet reader needs; Parquet stores column pages as raw Snappy blocks (the
// block format, not the streaming/framed format).
//
// Format reference (raw Snappy block):
//   * Preamble: the uncompressed length, little-endian base-128 varint.
//   * Body: a sequence of elements, each led by a tag byte whose low 2 bits are
//     the element type:
//       00 LITERAL      — length-1 in the tag's high 6 bits; if that value is
//                         >=60 it instead names 1..4 extra little-endian bytes
//                         holding length-1. Then that many literal bytes follow.
//       01 COPY_1B off  — len = 4 + ((tag>>2)&7); offset = ((tag>>5)&7)<<8 | b1.
//       02 COPY_2B off  — len = 1 + (tag>>2); offset = 2 LE bytes.
//       03 COPY_4B off  — len = 1 + (tag>>2); offset = 4 LE bytes.
//     A copy references already-produced output at (out_pos - offset); runs may
//     overlap (offset < length), so bytes are copied one at a time.
//
// Public domain / MIT-style: do as you wish. No warranty.

#ifndef MINIPARQUET_SNAPPY_DECOMPRESS_HPP
#define MINIPARQUET_SNAPPY_DECOMPRESS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace snappy_lite {

/// Read the uncompressed length preamble (base-128 varint). Advances *pos.
/// Returns false on malformed/overlong varint.
inline bool read_uncompressed_length(const uint8_t* in, size_t n, size_t* pos,
                                     uint32_t* result) {
    uint32_t value = 0;
    int shift = 0;
    while (shift < 32) {
        if (*pos >= n) return false;
        uint8_t b = in[*pos];
        ++(*pos);
        value |= static_cast<uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            *result = value;
            return true;
        }
        shift += 7;
    }
    return false;  // varint too long for a uint32 length
}

/// Decompress a raw Snappy block [in, in+n) into `out`. `out` is resized to the
/// decoded length taken from the block preamble. Returns false on any malformed
/// input (bad varint, truncated element, out-of-range copy offset, or a decoded
/// size that disagrees with the preamble).
inline bool raw_uncompress(const uint8_t* in, size_t n, std::string* out) {
    size_t pos = 0;
    uint32_t uncompressed_len = 0;
    if (!read_uncompressed_length(in, n, &pos, &uncompressed_len)) return false;

    out->clear();
    out->reserve(uncompressed_len);

    while (pos < n) {
        uint8_t tag = in[pos++];
        const int type = tag & 0x03;

        if (type == 0x00) {  // LITERAL
            size_t len = static_cast<size_t>(tag >> 2) + 1;
            if (len > 60) {
                // High 6 bits (>=60) encode 1..4 extra length bytes minus 59.
                const size_t extra = len - 60;  // 1..4
                if (pos + extra > n) return false;
                uint32_t l = 0;
                for (size_t i = 0; i < extra; ++i)
                    l |= static_cast<uint32_t>(in[pos + i]) << (8 * i);
                pos += extra;
                len = static_cast<size_t>(l) + 1;
            }
            if (pos + len > n) return false;
            out->append(reinterpret_cast<const char*>(in + pos), len);
            pos += len;
        } else {  // COPY
            size_t len;
            size_t offset;
            if (type == 0x01) {  // 1-byte offset
                if (pos + 1 > n) return false;
                len = 4 + ((tag >> 2) & 0x07);
                offset = (static_cast<size_t>(tag >> 5) << 8) | in[pos];
                pos += 1;
            } else if (type == 0x02) {  // 2-byte offset
                if (pos + 2 > n) return false;
                len = static_cast<size_t>(tag >> 2) + 1;
                offset = static_cast<size_t>(in[pos]) |
                         (static_cast<size_t>(in[pos + 1]) << 8);
                pos += 2;
            } else {  // 0x03, 4-byte offset
                if (pos + 4 > n) return false;
                len = static_cast<size_t>(tag >> 2) + 1;
                offset = static_cast<size_t>(in[pos]) |
                         (static_cast<size_t>(in[pos + 1]) << 8) |
                         (static_cast<size_t>(in[pos + 2]) << 16) |
                         (static_cast<size_t>(in[pos + 3]) << 24);
                pos += 4;
            }
            if (offset == 0 || offset > out->size()) return false;
            // Copy may overlap (offset < len): emit byte-by-byte so the just-
            // written bytes feed the run.
            size_t src = out->size() - offset;
            for (size_t i = 0; i < len; ++i) out->push_back((*out)[src + i]);
        }
    }

    return out->size() == uncompressed_len;
}

/// Convenience overload for the common (const char*, string) call shape.
inline bool raw_uncompress(const char* in, size_t n, std::string* out) {
    return raw_uncompress(reinterpret_cast<const uint8_t*>(in), n, out);
}

}  // namespace snappy_lite

#endif  // MINIPARQUET_SNAPPY_DECOMPRESS_HPP
