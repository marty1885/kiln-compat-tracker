#include "WorkerApi.h"
#include "common/protocol.h"
#include "server/json_response.h"
#include "server/config.h"
#include <drogon/orm/DbClient.h>
#include <filesystem>
#include <fstream>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

static std::string sanitize_for_path(const std::string &name) {
    std::string r;
    for (char c : name)
        r += (std::isalnum(c) || c == '-' || c == '_') ? c : '_';
    return r;
}

static std::string extractBearer(const HttpRequestPtr &req) {
    auto auth = req->getHeader("Authorization");
    if (auth.size() > 7 && auth.starts_with("Bearer "))
        return auth.substr(7);
    return {};
}

Task<HttpResponsePtr> WorkerApi::heartbeat(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized, "No Bearer token in Authorization header");

    HeartbeatRequest hb{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(hb, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "UPDATE workers SET arch=$1, os=$2, os_version=$3, distro=$4, cpu_model=$5, "
        "cores=$6, ram_mb=$7, compiler=$8, compiler_version=$9, last_seen=now() "
        "WHERE auth_token=$10 RETURNING id",
        hb.arch, hb.os, hb.os_version, hb.distro, hb.cpu_model,
        hb.cores, hb.ram_mb, hb.compiler, hb.compiler_version, token);

    if (r.empty())
        co_return error_response(k401Unauthorized, "Unknown auth token");

    co_return error_response(k200OK);
}

Task<HttpResponsePtr> WorkerApi::poll(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized, "No Bearer token in Authorization header");

    auto db = app().getDbClient();

    // Look up worker
    auto workerResult = co_await db->execSqlCoro(
        "SELECT id, resource_tier_max::text FROM workers WHERE auth_token=$1",
        token);

    if (workerResult.empty())
        co_return error_response(k401Unauthorized);

    auto workerId = workerResult[0]["id"].as<int64_t>();

    // Scheduler query — per-platform cooldown: a project is available for this
    // worker only if no recent build exists from the same platform
    // (arch + os + distro + compiler). Different platforms can build in parallel.
    auto projects = co_await db->execSqlCoro(
        "SELECT p.id, p.name, p.repo_url, p.branch, p.pinned_commit, "
        "p.build_command, p.run_tests "
        "FROM projects p "
        "CROSS JOIN workers w "
        "WHERE w.id = $1 "
        "  AND p.enabled = true "
        "  AND p.resource_tier <= w.resource_tier_max "
        "  AND p.dep_level <= w.dep_level_max "
        "  AND (p.os_filter = '' OR w.os = ANY(string_to_array(p.os_filter, ','))) "
        // No live (non-reaped) active job for this project on any platform
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM active_jobs aj "
        "    WHERE aj.project_id = p.id AND aj.reaped_at IS NULL"
        "  ) "
        // Global cooldown (anti-stampede): no build of this project by any
        // platform within the cooldown window. Prevents all workers from piling
        // on the same project simultaneously when it first becomes available.
        // Falls back to pure time-based if current_kiln_hash is not yet set.
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM build_results br "
        "    LEFT JOIN kiln_commits kc ON kc.id = br.kiln_commit_id "
        "    JOIN config cfg ON cfg.key = 'current_kiln_hash' "
        "    WHERE br.project_id = p.id "
        "      AND br.finished_at > now() - (p.cooldown_minutes * interval '1 minute')"
        "      AND br.finished_at > COALESCE(p.forced_rebuild_after, '-infinity'::timestamptz)"
        "      AND (cfg.value = '' OR kc.git_hash = cfg.value)"
        "  ) "
        // Per-platform coverage: this exact platform hasn't already produced a
        // result for this project with the current kiln hash since the last
        // forced rebuild. Prevents the same machine from repeatedly rebuilding
        // the same project once it already has coverage.
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM build_results br "
        "    LEFT JOIN kiln_commits kc ON kc.id = br.kiln_commit_id "
        "    JOIN config cfg ON cfg.key = 'current_kiln_hash' "
        "    JOIN workers bw ON bw.id = br.worker_id "
        "    WHERE br.project_id = p.id "
        "      AND br.finished_at > COALESCE(p.forced_rebuild_after, '-infinity'::timestamptz)"
        "      AND (cfg.value = '' OR kc.git_hash = cfg.value)"
        "      AND bw.arch = w.arch AND bw.os = w.os "
        "      AND bw.distro = w.distro AND bw.compiler = w.compiler"
        "  ) "
        "ORDER BY ("
        "  SELECT MAX(br.finished_at) FROM build_results br "
        "  JOIN workers bw ON bw.id = br.worker_id "
        "  WHERE br.project_id = p.id "
        "    AND bw.arch = w.arch AND bw.os = w.os "
        "    AND bw.distro = w.distro AND bw.compiler = w.compiler"
        ") ASC NULLS FIRST, "
        "p.resource_tier DESC "
        "LIMIT 1",
        workerId);

    if (projects.empty())
        co_return error_response(k204NoContent);

    const auto &row = projects[0];
    auto projectId = row["id"].as<int64_t>();

    // Get current kiln hash and upsert into kiln_commits so we can track it
    auto configResult = co_await db->execSqlCoro(
        "SELECT value FROM config WHERE key='current_kiln_hash'");
    auto kilnHash = configResult.empty() ? "" : configResult[0]["value"].as<std::string>();

    std::optional<int64_t> kilnCommitId;
    if (!kilnHash.empty()) {
        auto kc = co_await db->execSqlCoro(
            "INSERT INTO kiln_commits (git_hash) VALUES ($1) "
            "ON CONFLICT (git_hash) DO UPDATE SET git_hash = EXCLUDED.git_hash "
            "RETURNING id",
            kilnHash);
        if (!kc.empty())
            kilnCommitId = kc[0]["id"].as<int64_t>();
    }

    // Create active job, linking to kiln commit if known
    int64_t newJobId;
    if (kilnCommitId) {
        auto r = co_await db->execSqlCoro(
            "INSERT INTO active_jobs (project_id, worker_id, kiln_commit_id) "
            "VALUES ($1, $2, $3) RETURNING id",
            projectId, workerId, *kilnCommitId);
        newJobId = r[0]["id"].as<int64_t>();
    } else {
        auto r = co_await db->execSqlCoro(
            "INSERT INTO active_jobs (project_id, worker_id) VALUES ($1, $2) RETURNING id",
            projectId, workerId);
        newJobId = r[0]["id"].as<int64_t>();
    }

    PollResponse poll{
        .job_id = newJobId,
        .project_name = row["name"].as<std::string>(),
        .repo_url = row["repo_url"].as<std::string>(),
        .branch = row["branch"].as<std::string>(),
        .pinned_commit = row["pinned_commit"].isNull()
            ? std::nullopt
            : std::optional{row["pinned_commit"].as<std::string>()},
        .build_command = row["build_command"].isNull()
            ? std::nullopt
            : std::optional{row["build_command"].as<std::string>()},
        .run_tests = row["run_tests"].as<bool>(),
        .kiln_git_hash = kilnHash,
    };
    co_return json_response(poll);
}

Task<HttpResponsePtr> WorkerApi::jobHeartbeat(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized);

    JobHeartbeatRequest jh{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(jh, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();
    // Verify the job belongs to the authenticated worker before updating.
    co_await db->execSqlCoro(
        "UPDATE active_jobs SET heartbeat_at=now() "
        "WHERE id=$1 AND worker_id=(SELECT id FROM workers WHERE auth_token=$2)",
        jh.job_id, token);

    co_return error_response(k200OK);
}

Task<HttpResponsePtr> WorkerApi::result(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized);

    ResultRequest rr{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(rr, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();

    // Look up worker + active job
    auto lookup = co_await db->execSqlCoro(
        "SELECT w.id as worker_id, aj.project_id, aj.kiln_commit_id, p.name as project_name "
        "FROM workers w "
        "JOIN active_jobs aj ON aj.id=$1 AND aj.worker_id=w.id "
        "JOIN projects p ON p.id = aj.project_id "
        "WHERE w.auth_token=$2",
        rr.job_id, token);

    if (lookup.empty())
        co_return error_response(k404NotFound, "Job not found or not owned by this worker");

    auto workerId = lookup[0]["worker_id"].as<int64_t>();
    auto projectId = lookup[0]["project_id"].as<int64_t>();
    auto projectName = lookup[0]["project_name"].as<std::string>();

    // Resolve kiln_commit_id: prefer the hash reported by the worker (authoritative),
    // fall back to whatever was set on the active_job at poll time.
    std::optional<int64_t> kilnCommitId;
    if (!rr.kiln_git_hash.empty()) {
        auto kc = co_await db->execSqlCoro(
            "INSERT INTO kiln_commits (git_hash) VALUES ($1) "
            "ON CONFLICT (git_hash) DO UPDATE SET git_hash = EXCLUDED.git_hash "
            "RETURNING id",
            rr.kiln_git_hash);
        if (!kc.empty())
            kilnCommitId = kc[0]["id"].as<int64_t>();
    } else if (!lookup[0]["kiln_commit_id"].isNull()) {
        kilnCommitId = lookup[0]["kiln_commit_id"].as<int64_t>();
    }

    // Insert build result using stream binder for nullable fields
    auto binder = *db <<
        "INSERT INTO build_results "
        "(project_id, kiln_commit_id, project_commit, worker_id, "
        "compiler, compiler_version, status, test_status, test_duration_seconds, "
        "cmake_fallback_status, cmake_version, duration_seconds, "
        "cmake_duration_seconds, started_at, finished_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, CAST($7 AS build_status), "
        "CAST($8 AS build_status), $9, CAST($10 AS build_status), $11, $12, $13, "
        "now() - make_interval(secs => $14), now()) RETURNING id";

    binder << projectId;
    if (kilnCommitId) binder << *kilnCommitId; else binder << nullptr;
    binder << rr.project_commit << workerId
           << rr.compiler << rr.compiler_version << rr.status;
    if (rr.test_status) binder << *rr.test_status; else binder << nullptr;
    if (rr.test_duration_seconds) binder << *rr.test_duration_seconds; else binder << nullptr;
    if (rr.cmake_fallback_status) binder << *rr.cmake_fallback_status; else binder << nullptr;
    if (rr.cmake_version) binder << *rr.cmake_version; else binder << nullptr;
    binder << rr.duration_seconds;
    if (rr.cmake_duration_seconds) binder << *rr.cmake_duration_seconds; else binder << nullptr;
    binder << static_cast<double>(rr.duration_seconds);

    auto insertResult = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
    auto buildId = insertResult[0]["id"].as<int64_t>();

    // Write log files
    auto dir = std::filesystem::path(kiln::log_dir) / sanitize_for_path(projectName);
    std::filesystem::create_directories(dir);

    std::optional<std::string> logPath, cmakeLogPath;
    if (rr.log) {
        auto p = (dir / (std::to_string(buildId) + ".log")).string();
        std::ofstream(p) << *rr.log;
        logPath = p;
    }
    if (rr.cmake_log) {
        auto p = (dir / (std::to_string(buildId) + ".cmake.log")).string();
        std::ofstream(p) << *rr.cmake_log;
        cmakeLogPath = p;
    }
    if (logPath || cmakeLogPath) {
        auto b2 = *db << "UPDATE build_results SET log_path=$1, cmake_log_path=$2 WHERE id=$3";
        if (logPath) b2 << *logPath; else b2 << nullptr;
        if (cmakeLogPath) b2 << *cmakeLogPath; else b2 << nullptr;
        b2 << buildId;
        co_await drogon::orm::internal::SqlAwaiter(std::move(b2));
    }

    // Delete the active job
    co_await db->execSqlCoro("DELETE FROM active_jobs WHERE id=$1", rr.job_id);

    co_return error_response(k200OK);
}

} // namespace kiln
