#ifndef MINISET_CNET_NET_PROTOBUF_HPP
#define MINISET_CNET_NET_PROTOBUF_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cnet/control_net.hpp"

namespace cnet {

/// Read a control network from an ISIS protobuf `.net` file (version 5 framing),
/// with no libprotobuf dependency. `bytes` is the whole file loaded into memory
/// (read via GDAL VSI so /vsimem, /vsicurl, /vsis3 all work). Throws
/// std::runtime_error on malformed input.
ControlNet read_net_protobuf(const std::vector<uint8_t>& bytes);

/// Stream a `.net` file in bounded memory: parse the header once, then decode
/// points in batches of up to `batchPoints` whole points, invoking `onBatch`
/// with each self-contained batch ControlNet (header + a run of points and their
/// measures, CSR arrays seeded). Peak memory is one batch, not the whole net —
/// so a multi-GB net streams without materializing it. For local files the point
/// block is memory-mapped (the OS bounds resident pages); otherwise the file is
/// read whole (still one materialization, but points are still batched out).
/// Native only (uses mmap for local paths); throws on malformed input.
void stream_net_protobuf(const std::string& path, size_t batchPoints,
                         const std::function<void(ControlNet&)>& onBatch);

/// Serialize a control network to the ISIS protobuf `.net` version 5 byte layout
/// (65536-byte PVL label region + header message + length-prefixed point
/// messages). Byte-compatible with ISIS so files interoperate.
std::vector<uint8_t> write_net_protobuf(const ControlNet& net);

}  // namespace cnet

#endif  // MINISET_CNET_NET_PROTOBUF_HPP
