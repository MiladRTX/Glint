#pragma once

#include <string>

namespace glint {

// glint.log in the settings folder. Opening moves the previous run's log
// to glint.previous.log. Thread-safe; capped at 8 MB per run. Never log
// stream keys, tokens or full ingest URLs.
void log_open(const std::string& dir);
void log_close();
void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string log_path();
std::string previous_log_path();

// Writes Glint-diagnostics-<timestamp>.zip into `out_dir`: both logs,
// the shareable settings files and `system_info` as system.txt. No secrets.
bool create_diagnostics(const std::string& out_dir, const std::string& system_info, std::string& out_path,
                        std::string& error);

// %USERPROFILE%\Downloads, falling back to the settings folder.
std::string downloads_dir();

// "Windows 10.0.26200" from the kernel (not the compatibility-shimmed API).
std::string windows_version();

// Opens Explorer with the file selected.
void reveal_in_explorer(const std::string& path);

// CPU time (user + kernel) used by this process so far, in seconds.
double process_cpu_seconds();

// Glint.exe is a Windows app (no console window opens with it). Started
// from a terminal, this sends printf output (headless and test modes) back to
// that terminal; output redirected to a file or pipe already arrives as is.
void attach_parent_console();

} // namespace glint
