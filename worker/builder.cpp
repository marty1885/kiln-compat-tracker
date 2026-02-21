#include "builder.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace kiln {

static std::string trim_newlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::string prepare_project(const WorkerConfig &config, const PollResponse &job) {
    auto project_dir = fs::path(config.project_cache_dir()) / job.project_name;

    if (fs::exists(project_dir / ".git")) {
        // Fetch updates
        std::cout << "  Fetching " << job.project_name << "...\n";
        auto r = run_command("git -C " + project_dir.string() + " fetch --all --prune");
        if (r.exit_code != 0)
            std::cerr << "  Warning: git fetch failed: " << r.output << "\n";
    } else {
        // Fresh clone
        std::cout << "  Cloning " << job.repo_url << "...\n";
        fs::create_directories(project_dir.parent_path());
        auto r = run_command("git clone " + job.repo_url + " " + project_dir.string());
        if (r.exit_code != 0)
            throw std::runtime_error("git clone failed: " + r.output);
    }

    // Checkout the right ref
    if (job.pinned_commit) {
        run_command("git -C " + project_dir.string() + " checkout " + *job.pinned_commit);
    } else if (job.branch != "HEAD") {
        run_command("git -C " + project_dir.string() + " checkout " + job.branch);
        run_command("git -C " + project_dir.string() + " pull --ff-only");
    } else {
        // Default branch
        run_command("git -C " + project_dir.string() + " checkout "
                    "$(git -C " + project_dir.string() + " symbolic-ref refs/remotes/origin/HEAD "
                    "| sed 's@^refs/remotes/origin/@@')");
        run_command("git -C " + project_dir.string() + " pull --ff-only");
    }

    // Get actual HEAD hash
    auto hash_result = run_command("git -C " + project_dir.string() + " rev-parse HEAD");
    return trim_newlines(hash_result.output);
}

void ensure_kiln(const WorkerConfig &config, const std::string &kiln_git_hash) {
    auto kiln_dir = fs::path(config.kiln_source_dir());
    auto kiln_binary = kiln_dir / "build" / "kiln";

    // Clone if not present
    if (!fs::exists(kiln_dir / ".git")) {
        std::cout << "  Cloning kiln...\n";
        fs::create_directories(kiln_dir.parent_path());
        auto r = run_command("git clone git@github.com:marty1885/kiln.git " + kiln_dir.string());
        if (r.exit_code != 0)
            throw std::runtime_error("Failed to clone kiln: " + r.output);
    }

    // Check current hash
    auto cur = run_command("git -C " + kiln_dir.string() + " rev-parse HEAD");
    auto current_hash = trim_newlines(cur.output);

    if (!kiln_git_hash.empty() && current_hash == kiln_git_hash && fs::exists(kiln_binary)) {
        std::cout << "  Kiln already at " << kiln_git_hash.substr(0, 10) << "\n";
        return;
    }

    // Fetch and checkout requested hash
    std::cout << "  Updating kiln to " << (kiln_git_hash.empty() ? "latest" : kiln_git_hash.substr(0, 10)) << "...\n";
    auto r = run_command("git -C " + kiln_dir.string() + " fetch --all --prune");
    if (r.exit_code != 0)
        std::cerr << "  Warning: kiln git fetch failed: " << r.output << "\n";

    if (!kiln_git_hash.empty()) {
        r = run_command("git -C " + kiln_dir.string() + " checkout " + kiln_git_hash);
        if (r.exit_code != 0)
            throw std::runtime_error("Failed to checkout kiln hash " + kiln_git_hash + ": " + r.output);
    }

    // Build kiln: mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja && ninja
    auto build_dir = kiln_dir / "build";
    fs::create_directories(build_dir);

    std::string build_cmd = "cmake -S " + kiln_dir.string() + " -B " + build_dir.string() +
                            " -DCMAKE_BUILD_TYPE=Release -G Ninja"
                            " && ninja -C " + build_dir.string();

    std::cout << "  Building kiln...\n";
    r = run_command(build_cmd);
    if (r.exit_code != 0)
        throw std::runtime_error("Failed to build kiln: " + r.output);

    if (!fs::exists(kiln_binary))
        throw std::runtime_error("Kiln build succeeded but binary not found at " + kiln_binary.string());

    std::cout << "  Kiln built successfully\n";
}

BuildResult run_build(const WorkerConfig &config, const PollResponse &job,
                      const std::string &project_dir) {
    BuildResult result;
    auto kiln_binary = (fs::path(config.kiln_source_dir()) / "build" / "kiln").string();

    // Determine build command
    std::string kiln_cmd;
    if (job.build_command) {
        kiln_cmd = *job.build_command;
    } else {
        kiln_cmd = kiln_binary + " -C " + project_dir + " --config release";
    }

    // Clean any previous build
    auto build_dir = project_dir + "/build";
    if (fs::exists(build_dir))
        fs::remove_all(build_dir);

    // Run kiln build
    std::cout << "  Running kiln build: " << kiln_cmd << "\n";
    auto kiln_result = run_command(kiln_cmd);
    result.duration_seconds = kiln_result.duration_seconds;
    result.log = std::move(kiln_result.output);

    if (kiln_result.exit_code == 0) {
        result.status = "pass";
        return result;
    }

    // Kiln failed — run cmake fallback
    result.status = "fail";
    std::cout << "  Kiln failed (exit " << kiln_result.exit_code << "), trying cmake fallback...\n";

    // Get cmake version
    auto cmake_ver = run_command("cmake --version | head -1");
    if (cmake_ver.exit_code == 0)
        result.cmake_version = trim_newlines(cmake_ver.output);

    // Clean again for cmake
    if (fs::exists(build_dir))
        fs::remove_all(build_dir);

    std::string cmake_cmd = "cmake -S " + project_dir + " -B " + build_dir +
                            " -DCMAKE_BUILD_TYPE=Release && cmake --build " + build_dir +
                            " -j$(nproc)";

    std::cout << "  Running cmake: " << cmake_cmd << "\n";
    auto cmake_result = run_command(cmake_cmd);
    result.cmake_duration_seconds = cmake_result.duration_seconds;
    result.cmake_log = std::move(cmake_result.output);

    result.cmake_fallback_status = (cmake_result.exit_code == 0) ? "pass" : "fail";

    return result;
}

} // namespace kiln
