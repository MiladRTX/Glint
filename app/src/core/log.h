#pragma once
#include <string>

namespace glint {

// Log file in the settings folder, one timestamped line per call.
void log_open(const std::string& dir);
void log_close();
void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string log_path();
std::string previous_log_path();

// Zip of the logs and settings files for a bug report.
bool create_diagnostics(const std::string& out_dir, const std::string& system_info,
                        std::string& out_path, std::string& error);

// Small Windows helpers.
std::string downloads_dir();
std::string windows_version();
void reveal_in_explorer(const std::string& path);
double process_cpu_seconds();
void attach_parent_console();

}
