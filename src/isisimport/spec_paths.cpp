#include "isisimport/spec_paths.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace isisimport {

namespace {

bool is_dir(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

/// Best-effort absolute path of the running executable.
std::string executable_dir() {
    std::string path;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) == 0) {
        buf.resize(size ? size - 1 : 0);  // drop trailing NUL if counted
        path = buf.c_str();
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        path = buf;
    }
#endif
    if (path.empty()) return std::string();
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace

std::string resolve_spec_dir(const std::string& override_dir) {
    // 1. Explicit override.
    if (!override_dir.empty() && is_dir(override_dir)) return override_dir;

    // 2. Environment (parity with ISIS $ISISROOT/appdata).
    if (const char* env = std::getenv("MINISET_APPDATA")) {
        std::string p = std::string(env) + "/import";
        if (is_dir(p)) return p;
    }

    // 3. Install-relative: <exe dir>/../share/miniset/import.
    std::string exe = executable_dir();
    if (!exe.empty()) {
        std::string p = exe + "/../share/miniset/import";
        if (is_dir(p)) return p;
    }

    // 4. Build-tree fallback.
#ifdef MINISET_APPDATA_BUILD
    if (is_dir(MINISET_APPDATA_BUILD)) return MINISET_APPDATA_BUILD;
#endif

    return std::string();
}

}  // namespace isisimport
