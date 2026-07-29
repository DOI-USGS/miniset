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
