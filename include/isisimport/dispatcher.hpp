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

#ifndef MINISET_ISISIMPORT_DISPATCHER_HPP
#define MINISET_ISISIMPORT_DISPATCHER_HPP

#include <string>

#include <nlohmann/json.hpp>

#include "isisimport/input_label.hpp"

namespace isisimport {

/// Result of dispatch: the chosen spec name and the resolved absolute path.
struct DispatchResult {
    bool ok = false;
    std::string message;      ///< Failure explanation when !ok.
    std::string spec_name;
    std::string spec_path;
};

/// Infer the instrument spec from the input label (default path), like ISIS's
/// fileTemplate.tpl. Reads only `dispatch.json` from `spec_dir`.
DispatchResult dispatch(const nlohmann::json& input_label,
                        ProductType product_type,
                        const std::string& spec_dir);

/// Resolve a manual template override (spec name or path) to a spec path.
/// If `override_name` looks like a path (has a separator or .json), it's used
/// as-is; otherwise it's resolved under `spec_dir`.
DispatchResult resolve_template_override(const std::string& override_name,
                                         ProductType product_type,
                                         const std::string& spec_dir);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_DISPATCHER_HPP
