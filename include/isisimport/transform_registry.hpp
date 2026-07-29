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

#ifndef MINISET_ISISIMPORT_TRANSFORM_REGISTRY_HPP
#define MINISET_ISISIMPORT_TRANSFORM_REGISTRY_HPP

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace isisimport {

/// Context shared across a single import: the full input label plus a place to
/// record a pixel crop window that a transform may compute (e.g. MRO CTX
/// prefix/suffix trimming). The window, if set, is applied by the output writer.
struct TransformContext {
    const nlohmann::json* input_label = nullptr;

    /// Optional pixel crop window (in source pixels). Applied by output_writer
    /// via a GDAL translate/VRT subset when has_window is true.
    bool has_window = false;
    int win_xoff = 0;
    int win_yoff = 0;
    int win_xsize = 0;
    int win_ysize = 0;
};

/// A named transform. Receives the resolved input value (may be null), the
/// transform's JSON args, and the shared context; returns the JSON to emit.
using TransformFn = std::function<nlohmann::json(
    const nlohmann::json& input_value,
    const nlohmann::json& args,
    TransformContext& ctx)>;

/// Registry mapping transform names (referenced from specs) to C++ functions.
/// Pure instruments need none; the registry is the additive hook for the ~13
/// instruments with computed keywords.
class TransformRegistry {
  public:
    void registerFn(const std::string& name, TransformFn fn);
    const TransformFn* find(const std::string& name) const;

    /// The process-wide registry preloaded with built-in transforms
    /// (currently just "mroctx_crop").
    static const TransformRegistry& builtin();

  private:
    std::unordered_map<std::string, TransformFn> fns_;
};

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_TRANSFORM_REGISTRY_HPP
