#include "platform.h"

#include <sys/utsname.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <array>
#include <thread>

namespace kiln {

SystemInfo detect_system_info() {
    SystemInfo info;

    struct utsname u;
    if (uname(&u) == 0) {
        info.arch = u.machine;
        info.os = u.sysname;
        // Lowercase the OS name
        for (auto &c : info.os) c = static_cast<char>(std::tolower(c));
        info.os_version = u.release;
    }

    // CPU model from /proc/cpuinfo
    if (std::ifstream f("/proc/cpuinfo"); f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.starts_with("model name")) {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    info.cpu_model = line.substr(pos + 2);
                break;
            }
        }
    }

    // Core count
    info.cores = static_cast<int>(std::thread::hardware_concurrency());

    // RAM from /proc/meminfo
    if (std::ifstream f("/proc/meminfo"); f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.starts_with("MemTotal:")) {
                std::istringstream iss(line);
                std::string label;
                long kb{};
                iss >> label >> kb;
                info.ram_mb = static_cast<int>(kb / 1024);
                break;
            }
        }
    }

    return info;
}

SubprocessResult run_command(const std::string &cmd, int timeout_seconds) {
    auto start = std::chrono::steady_clock::now();

    // popen for simplicity — good enough for MVP
    std::string full_cmd = cmd + " 2>&1";
    FILE *pipe = popen(full_cmd.c_str(), "r");
    if (!pipe)
        return {.exit_code = -1, .output = "Failed to execute: " + cmd};

    std::string output;
    std::array<char, 4096> buf;
    while (auto n = fread(buf.data(), 1, buf.size(), pipe))
        output.append(buf.data(), n);

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    auto end = std::chrono::steady_clock::now();
    int duration = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(end - start).count());

    return {
        .exit_code = exit_code,
        .output = std::move(output),
        .duration_seconds = duration,
    };
}

std::string detect_compiler() {
    // Try to find what's available
    auto r = run_command("c++ --version 2>/dev/null | head -1");
    if (r.exit_code == 0 && !r.output.empty()) {
        if (r.output.find("clang") != std::string::npos) return "clang";
        if (r.output.find("GCC") != std::string::npos || r.output.find("g++") != std::string::npos)
            return "gcc";
    }
    return "unknown";
}

std::string detect_compiler_version() {
    auto r = run_command("c++ -dumpversion 2>/dev/null");
    if (r.exit_code == 0 && !r.output.empty()) {
        // Trim trailing newline
        auto ver = r.output;
        while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r'))
            ver.pop_back();
        return ver;
    }
    return "unknown";
}

} // namespace kiln
