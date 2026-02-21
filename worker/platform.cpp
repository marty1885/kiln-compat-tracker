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

    // Distro from /etc/os-release (NAME + VERSION_ID fields)
    // Rolling distros (Arch, openSUSE TW) have no VERSION_ID — just use NAME.
    // Versioned distros (Ubuntu, Fedora) have VERSION_ID — combine as "Ubuntu 22.04".
    if (std::ifstream f("/etc/os-release"); f) {
        std::string line;
        std::string distro_name, distro_version;
        while (std::getline(f, line)) {
            auto strip_quotes = [](std::string val) {
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                    val = val.substr(1, val.size() - 2);
                return val;
            };
            if (line.starts_with("NAME=") && distro_name.empty())
                distro_name = strip_quotes(line.substr(5));
            else if (line.starts_with("VERSION_ID=") && distro_version.empty())
                distro_version = strip_quotes(line.substr(11));
            if (!distro_name.empty() && !distro_version.empty())
                break;
        }
        info.distro = distro_version.empty()
            ? distro_name
            : distro_name + " " + distro_version;
    }

    // CPU model — try /proc/cpuinfo first (x86 has "model name"),
    // fall back to lscpu which works on both x86 and ARM.
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
    if (info.cpu_model.empty()) {
        auto r = run_command("lscpu 2>/dev/null");
        if (r.exit_code == 0) {
            std::istringstream iss(r.output);
            std::string line;
            std::string model_name, model;
            auto extract = [](const std::string &l, size_t prefix_len) {
                auto val = l.substr(prefix_len);
                auto start = val.find_first_not_of(" \t");
                if (start != std::string::npos)
                    val = val.substr(start);
                // Treat "-" as empty (common on ARM)
                if (val == "-") val.clear();
                return val;
            };
            while (std::getline(iss, line)) {
                if (model_name.empty() && line.starts_with("Model name:"))
                    model_name = extract(line, 11);
                else if (model.empty() && line.starts_with("Model:"))
                    model = extract(line, 6);
            }
            // Prefer "Model name:" but fall back to "Model:" (e.g. Rockchip RK3588)
            info.cpu_model = model_name.empty() ? model : model_name;
        }
    }

    // Core count — std::thread::hardware_concurrency() works on Linux and *BSD
    info.cores = static_cast<int>(std::thread::hardware_concurrency());

    // RAM from /proc/meminfo (Linux)
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

    // *BSD fallbacks (sysctl) for fields not populated above
    if (info.cpu_model.empty()) {
        auto r = run_command("sysctl -n hw.model 2>/dev/null");
        if (r.exit_code == 0 && !r.output.empty()) {
            auto s = r.output;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            info.cpu_model = std::move(s);
        }
    }
    if (info.ram_mb == 0) {
        // hw.physmem is in bytes
        auto r = run_command("sysctl -n hw.physmem 2>/dev/null");
        if (r.exit_code == 0 && !r.output.empty()) {
            try { info.ram_mb = static_cast<int>(std::stoll(r.output) / 1024 / 1024); }
            catch (...) {}
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
        // GCC-specific flag: -dumpfullversion (clang doesn't support it).
        // Catches Debian/Ubuntu where c++ --version shows e.g.
        //   c++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
        auto dfv = run_command("c++ -dumpfullversion 2>/dev/null");
        if (dfv.exit_code == 0 && !dfv.output.empty())
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
