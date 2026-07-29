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
