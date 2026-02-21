#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <glaze/glaze.hpp>

namespace kiln {

// --- Worker → Server requests ---

struct HeartbeatRequest {
    std::string name;
    std::string arch;
    std::string os;
    std::string os_version;
    std::string cpu_model;
    int cores{};
    int ram_mb{};
    std::string resource_tier_max;
};

struct PollResponse {
    int64_t job_id{};
    std::string project_name;
    std::string repo_url;
    std::string branch;
    std::optional<std::string> pinned_commit;
    std::optional<std::string> build_command;
    bool run_tests{};
    std::string kiln_git_hash;
};

struct JobHeartbeatRequest {
    int64_t job_id{};
};

struct ResultRequest {
    int64_t job_id{};
    std::string status;
    int duration_seconds{};
    std::string compiler;
    std::string compiler_version;
    std::string project_commit;
    std::optional<std::string> test_status;
    std::optional<int> test_duration_seconds;
    std::optional<std::string> cmake_fallback_status;
    std::optional<int> cmake_duration_seconds;
    std::optional<std::string> cmake_version;
};

// --- Dashboard API responses ---

struct ProjectInfo {
    int64_t id{};
    std::string name;
    std::string repo_url;
    std::string branch;
    std::optional<std::string> pinned_commit;
    std::optional<std::string> build_command;
    bool run_tests{};
    std::optional<std::string> test_resource_tier_min;
    std::string resource_tier;
    int cooldown_minutes{};
    bool enabled{};
};

struct ProjectCreateRequest {
    std::string name;
    std::string repo_url;
    std::string branch{"HEAD"};
    std::optional<std::string> pinned_commit;
    std::optional<std::string> build_command;
    bool run_tests{};
    std::optional<std::string> test_resource_tier_min;
    std::string resource_tier{"small"};
    int cooldown_minutes{30};
};

struct MatrixCell {
    int64_t project_id{};
    std::string project_name;
    std::string platform; // "arch-os-compiler"
    std::string status;   // "pass", "fail", "building", "both_fail"
    std::optional<std::string> cmake_fallback_status;
    std::optional<std::string> finished_at;
    std::optional<int64_t> build_result_id;
};

struct WorkerInfo {
    int64_t id{};
    std::string name;
    std::string arch;
    std::string os;
    std::string os_version;
    std::string cpu_model;
    int cores{};
    int ram_mb{};
    std::string resource_tier_max;
    std::string last_seen;
    std::optional<std::string> current_job;
};

struct BuildResultInfo {
    int64_t id{};
    std::string project_name;
    std::string kiln_hash;
    std::string project_commit;
    std::string worker_name;
    std::string compiler;
    std::string compiler_version;
    std::string status;
    std::optional<std::string> test_status;
    std::optional<std::string> cmake_fallback_status;
    int duration_seconds{};
    std::optional<int> cmake_duration_seconds;
    std::string started_at;
    std::string finished_at;
};

// Small response types for simple JSON replies
struct IdResponse { int64_t id{}; };
struct EnabledResponse { bool enabled{}; };
struct LogPathsResponse {
    std::string log_path;
    std::string cmake_log_path;
};

struct TrendPoint {
    std::string date;        // YYYY-MM-DD
    int passing{};           // projects with at least one pass that day
    int kiln_bugs{};         // projects where kiln failed but cmake passed
    int total_projects{};    // total enabled projects
};

// --- Auth API ---

struct AuthStatus {
    bool logged_in{};
    std::string username;
    bool needs_setup{};
};

struct RegisterRequest {
    std::string username;
    std::string password;
    std::optional<std::string> invite_token;
};

struct LoginRequest {
    std::string username;
    std::string password;
};

struct InviteResponse {
    std::string token;
    std::string url;
};

// --- Worker Management API ---

struct WorkerCreateRequest {
    std::string name;
    std::string resource_tier_max{"small"};
};

struct WorkerCreateResponse {
    int64_t id{};
    std::string auth_token;
};

struct WorkerAdminInfo {
    int64_t id{};
    std::string name;
    std::string auth_token;
    std::string arch;
    std::string os;
    std::string os_version;
    std::string resource_tier_max;
    std::string last_seen;
    std::optional<std::string> current_job;
};

} // namespace kiln
