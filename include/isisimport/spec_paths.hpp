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

#ifndef MINISET_ISISIMPORT_SPEC_PATHS_HPP
#define MINISET_ISISIMPORT_SPEC_PATHS_HPP

#include <string>

namespace isisimport {

/// Resolve the bundled import spec directory (the one containing dispatch.json
/// and specs/). Resolution order:
///   1. `override_dir` if non-empty
///   2. env MINISET_APPDATA + "/import"
///   3. install-relative <exe dir>/../share/miniset/import
///   4. build-tree fallback (MINISET_APPDATA_BUILD compile definition)
/// Returns the first directory that exists; empty string if none found.
std::string resolve_spec_dir(const std::string& override_dir);

}  // namespace isisimport

#endif  // MINISET_ISISIMPORT_SPEC_PATHS_HPP
