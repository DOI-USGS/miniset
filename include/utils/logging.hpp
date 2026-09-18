#ifndef MINISET_UTILS_LOGGING_HPP
#define MINISET_UTILS_LOGGING_HPP

#include <iostream>
#include <sstream>
#include <string>

namespace miniset {
namespace logging {

enum class Level {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

// Simple logger class
class Logger {
public:
    static Level& get_level() {
        static Level level = Level::INFO;
        return level;
    }

    static void set_level(Level lvl) {
        get_level() = lvl;
    }

    static void log(Level level, const std::string& msg) {
        if (level < get_level()) return;

        const char* prefix = "";
        switch (level) {
            case Level::DEBUG:   prefix = "[DEBUG] "; break;
            case Level::INFO:    prefix = "[INFO] "; break;
            case Level::WARNING: prefix = "[WARN] "; break;
            case Level::ERROR:   prefix = "[ERROR] "; break;
        }
        std::cerr << prefix << msg << std::endl;
    }

    template<typename... Args>
    static void debug(Args&&... args) {
        log_impl(Level::DEBUG, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void info(Args&&... args) {
        log_impl(Level::INFO, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warning(Args&&... args) {
        log_impl(Level::WARNING, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(Args&&... args) {
        log_impl(Level::ERROR, std::forward<Args>(args)...);
    }

private:
    template<typename... Args>
    static void log_impl(Level level, Args&&... args) {
        std::ostringstream oss;
        (oss << ... << args);
        log(level, oss.str());
    }
};

// Convenience macros
#define MINISET_LOG_DEBUG(...) ::miniset::logging::Logger::debug(__VA_ARGS__)
#define MINISET_LOG_INFO(...) ::miniset::logging::Logger::info(__VA_ARGS__)
#define MINISET_LOG_WARNING(...) ::miniset::logging::Logger::warning(__VA_ARGS__)
#define MINISET_LOG_ERROR(...) ::miniset::logging::Logger::error(__VA_ARGS__)

} // namespace logging
} // namespace miniset

#endif // MINISET_UTILS_LOGGING_HPP
