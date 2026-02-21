#pragma once

#include <string>

namespace kiln {

struct SystemInfo {
    std::string arch;
    std::string os;
    std::string os_version;
    std::string cpu_model;
    int cores{};
    int ram_mb{};
};

SystemInfo detect_system_info();

struct SubprocessResult {
    int exit_code{};
    std::string output;
    int duration_seconds{};
};

// Run a command, capture combined stdout+stderr. Timeout in seconds (0 = no timeout).
SubprocessResult run_command(const std::string &cmd, int timeout_seconds = 0);

// Detect default compiler name and version
std::string detect_compiler();
std::string detect_compiler_version();

} // namespace kiln
