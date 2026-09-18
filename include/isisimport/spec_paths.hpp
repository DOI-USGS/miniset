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
