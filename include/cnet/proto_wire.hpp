#ifndef MINISET_CNET_PROTO_WIRE_HPP
#define MINISET_CNET_PROTO_WIRE_HPP

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Minimal protobuf wire-format reader/writer — no libprotobuf dependency.
// Supports the subset needed by ISIS control-network messages: varint (int32/
// bool/enum), 64-bit (double), and length-delimited (string / packed / nested).
// Reference: https://protobuf.dev/programming-guides/encoding/

namespace cnet {
namespace wire {

enum WireType : uint32_t {
    kVarint = 0,
    kFixed64 = 1,
    kLenDelim = 2,
    kFixed32 = 5,
};

/// Reads protobuf fields from a byte range. Not owning; caller keeps data alive.
class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : cur_(data), end_(data + size) {}
    Reader(const char* data, size_t size)
        : Reader(reinterpret_cast<const uint8_t*>(data), size) {}

    bool done() const { return cur_ >= end_; }

    /// Read the next field's tag; returns false at end. Sets field number + wire type.
    bool readTag(uint32_t& fieldNum, uint32_t& wireType) {
        if (done()) return false;
        uint64_t tag = readVarint();
        fieldNum = static_cast<uint32_t>(tag >> 3);
        wireType = static_cast<uint32_t>(tag & 0x7);
        return true;
    }

    uint64_t readVarint() {
        uint64_t result = 0;
        int shift = 0;
        while (cur_ < end_) {
            uint8_t b = *cur_++;
            result |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return result;
            shift += 7;
            if (shift >= 64) break;
        }
        throw std::runtime_error("protobuf varint truncated");
    }

    int32_t readInt32() { return static_cast<int32_t>(readVarint()); }
    bool readBool() { return readVarint() != 0; }

    double readDouble() {
        if (cur_ + 8 > end_) throw std::runtime_error("protobuf fixed64 truncated");
        uint64_t bits;
        std::memcpy(&bits, cur_, 8);  // protobuf fixed64 is little-endian
        cur_ += 8;
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }

    /// Read a length-delimited byte range (string / bytes / nested message).
    void readBytes(const uint8_t*& out, size_t& len) {
        uint64_t n = readVarint();
        if (cur_ + n > end_) throw std::runtime_error("protobuf length-delimited truncated");
        out = cur_;
        len = static_cast<size_t>(n);
        cur_ += n;
    }

    std::string readString() {
        const uint8_t* p;
        size_t n;
        readBytes(p, n);
        return std::string(reinterpret_cast<const char*>(p), n);
    }

    /// Return a sub-reader over the next length-delimited region (nested message).
    Reader readSubMessage() {
        const uint8_t* p;
        size_t n;
        readBytes(p, n);
        return Reader(p, n);
    }

    /// Skip a field of the given wire type whose value we don't consume.
    void skip(uint32_t wireType) {
        switch (wireType) {
            case kVarint: readVarint(); break;
            case kFixed64: advance(8); break;
            case kFixed32: advance(4); break;
            case kLenDelim: {
                uint64_t n = readVarint();
                advance(static_cast<size_t>(n));
                break;
            }
            default: throw std::runtime_error("protobuf unknown wire type");
        }
    }

  private:
    void advance(size_t n) {
        if (cur_ + n > end_) throw std::runtime_error("protobuf skip past end");
        cur_ += n;
    }
    const uint8_t* cur_;
    const uint8_t* end_;
};

/// Appends protobuf fields to a growable byte buffer.
class Writer {
  public:
    explicit Writer(std::vector<uint8_t>& buf) : buf_(buf) {}

    void writeVarintField(uint32_t fieldNum, uint64_t value) {
        writeTag(fieldNum, kVarint);
        writeVarint(value);
    }
    void writeInt32(uint32_t fieldNum, int32_t v) {
        writeVarintField(fieldNum, static_cast<uint64_t>(static_cast<int64_t>(v)));
    }
    void writeBool(uint32_t fieldNum, bool v) { writeVarintField(fieldNum, v ? 1 : 0); }
    void writeEnum(uint32_t fieldNum, int32_t v) { writeVarintField(fieldNum, static_cast<uint64_t>(v)); }

    void writeDouble(uint32_t fieldNum, double v) {
        writeTag(fieldNum, kFixed64);
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }

    void writeString(uint32_t fieldNum, const std::string& s) {
        writeTag(fieldNum, kLenDelim);
        writeVarint(s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }

    /// Write a nested/length-delimited field from a pre-serialized byte buffer.
    void writeBytes(uint32_t fieldNum, const std::vector<uint8_t>& bytes) {
        writeTag(fieldNum, kLenDelim);
        writeVarint(bytes.size());
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    /// Write a `repeated double [packed=true]` field.
    void writePackedDoubles(uint32_t fieldNum, const double* vals, size_t n) {
        writeTag(fieldNum, kLenDelim);
        writeVarint(n * 8);
        for (size_t i = 0; i < n; ++i) {
            uint64_t bits;
            std::memcpy(&bits, &vals[i], 8);
            for (int b = 0; b < 8; ++b) buf_.push_back(static_cast<uint8_t>(bits >> (8 * b)));
        }
    }

  private:
    void writeTag(uint32_t fieldNum, WireType wt) {
        writeVarint((static_cast<uint64_t>(fieldNum) << 3) | wt);
    }
    void writeVarint(uint64_t value) {
        while (value >= 0x80) {
            buf_.push_back(static_cast<uint8_t>(value) | 0x80);
            value >>= 7;
        }
        buf_.push_back(static_cast<uint8_t>(value));
    }
    std::vector<uint8_t>& buf_;
};

}  // namespace wire
}  // namespace cnet

#endif  // MINISET_CNET_PROTO_WIRE_HPP
