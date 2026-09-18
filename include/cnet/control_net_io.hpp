#ifndef MINISET_CNET_CONTROL_NET_IO_HPP
#define MINISET_CNET_CONTROL_NET_IO_HPP

#include <string>

#include "cnet/control_net.hpp"

namespace cnet {

/// Control-network on-disk format.
enum class NetFormat { Auto, Protobuf, Parquet, StarDS };

/// Choose the format from a path (after stripping a `?…` VSI query suffix):
/// `.parquet` → Parquet; `.stards` → StarDS; otherwise the protobuf `.net`.
NetFormat infer_net_format(const std::string& path);

/// Read a control network, dispatching on `format` (Auto = infer from path).
/// Protobuf `.net` is read in all builds (pure C++); Parquet requires the native
/// GDAL Parquet driver. Paths may be /vsimem, /vsicurl, /vsis3.
ControlNet read_control_net(const std::string& path, NetFormat format = NetFormat::Auto);

/// Write a control network, dispatching on `format` (Auto = infer from path).
void write_control_net(const ControlNet& net, const std::string& path,
                       NetFormat format = NetFormat::Auto);

}  // namespace cnet

#endif  // MINISET_CNET_CONTROL_NET_IO_HPP
