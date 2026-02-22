#pragma once

#include "common/protocol.h"
#include "worker/platform.h"
#include "worker/config.h"
#include <filesystem>
#include <string>
#include <optional>

namespace kiln {

// Get the cache directory for a project: <cache_dir>/<project_id>/<repo_name>
std::filesystem::path project_dir_for(const WorkerConfig &config, const PollResponse &job);

struct BuildResult {
    std::string status;           // pass, fail, timeout, error
    int duration_seconds{};
    std::string log;
    std::string project_commit;

    std::optional<std::string> test_status;
    std::optional<int> test_duration_seconds;

    std::optional<std::string> cmake_fallback_status;
    std::optional<int> cmake_duration_seconds;
    std::optional<std::string> cmake_version;
    std::string cmake_log;
};

// Clone/fetch project repo into cache dir, return the actual HEAD commit hash
std::string prepare_project(const WorkerConfig &config, const PollResponse &job);

// Ensure kiln repo is cloned, checked out to the right hash, and built.
// Returns the actual git hash of the kiln HEAD that was built.
std::string ensure_kiln(const WorkerConfig &config, const std::string &kiln_git_hash);

// Run kiln build, and cmake fallback if kiln fails
BuildResult run_build(const WorkerConfig &config, const PollResponse &job,
                      const std::string &project_dir);

} // namespace kiln
