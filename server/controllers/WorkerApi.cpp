#include "WorkerApi.h"
#include "common/protocol.h"
#include "server/json_response.h"
#include <drogon/orm/DbClient.h>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

static std::string extractBearer(const HttpRequestPtr &req) {
    auto auth = req->getHeader("Authorization");
    if (auth.size() > 7 && auth.starts_with("Bearer "))
        return auth.substr(7);
    return {};
}

Task<HttpResponsePtr> WorkerApi::heartbeat(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized);

    HeartbeatRequest hb{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(hb, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    // Validate resource_tier to prevent injection (we inline it in SQL
    // because Drogon's binary protocol can't cast text params to enums)
    if (hb.resource_tier_max != "small" &&
        hb.resource_tier_max != "medium" &&
        hb.resource_tier_max != "large")
        co_return error_response(k400BadRequest, "invalid resource_tier_max");

    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "UPDATE workers SET arch=$1, os=$2, os_version=$3, cpu_model=$4, "
        "cores=$5, ram_mb=$6, resource_tier_max='" + hb.resource_tier_max + "'::resource_tier, last_seen=now() "
        "WHERE auth_token=$7",
        hb.arch, hb.os, hb.os_version, hb.cpu_model,
        hb.cores, hb.ram_mb, token);

    co_return error_response(k200OK);
}

Task<HttpResponsePtr> WorkerApi::poll(HttpRequestPtr req) {
    auto token = extractBearer(req);
    if (token.empty())
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();

    // Look up worker
    auto workerResult = co_await db->execSqlCoro(
        "SELECT id, resource_tier_max::text FROM workers WHERE auth_token=$1",
        token);

    if (workerResult.empty())
        co_return error_response(k401Unauthorized);

    auto workerId = workerResult[0]["id"].as<int64_t>();

    // Scheduler query — join against the worker row to compare resource_tier
    // directly in SQL, avoiding binary-protocol enum parameter issues
    auto projects = co_await db->execSqlCoro(
        "SELECT p.id, p.name, p.repo_url, p.branch, p.pinned_commit, "
        "p.build_command, p.run_tests "
        "FROM projects p "
        "CROSS JOIN workers w "
        "LEFT JOIN build_results br "
        "  ON br.project_id = p.id AND br.status = 'pass' "
        "  AND br.finished_at = ("
        "    SELECT MAX(br2.finished_at) FROM build_results br2 "
        "    WHERE br2.project_id = p.id AND br2.status = 'pass'"
        "  ) "
        "LEFT JOIN active_jobs aj ON aj.project_id = p.id "
        "WHERE w.id = $1 "
        "  AND p.enabled = true "
        "  AND p.resource_tier <= w.resource_tier_max "
        "  AND aj.id IS NULL "
        "  AND ("
        "    br.id IS NULL "
        "    OR br.finished_at < now() - (p.cooldown_minutes * interval '1 minute')"
        "  ) "
        "ORDER BY COALESCE(br.finished_at, '1970-01-01') ASC, "
        "  p.resource_tier DESC "
        "LIMIT 1",
        workerId);

    if (projects.empty())
        co_return error_response(k204NoContent);

    const auto &row = projects[0];
    auto projectId = row["id"].as<int64_t>();

    // Get current kiln hash
    auto configResult = co_await db->execSqlCoro(
        "SELECT value FROM config WHERE key='current_kiln_hash'");
    auto kilnHash = configResult.empty() ? "" : configResult[0]["value"].as<std::string>();

    // Create active job
    auto jobResult = co_await db->execSqlCoro(
        "INSERT INTO active_jobs (project_id, worker_id) VALUES ($1, $2) RETURNING id",
        projectId, workerId);

    PollResponse poll{
        .job_id = jobResult[0]["id"].as<int64_t>(),
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
    co_await db->execSqlCoro(
        "UPDATE active_jobs SET heartbeat_at=now() WHERE id=$1", jh.job_id);

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
        "SELECT w.id as worker_id, aj.project_id, aj.kiln_commit_id "
        "FROM workers w "
        "JOIN active_jobs aj ON aj.id=$1 AND aj.worker_id=w.id "
        "WHERE w.auth_token=$2",
        rr.job_id, token);

    if (lookup.empty())
        co_return error_response(k404NotFound, "Job not found or not owned by this worker");

    auto workerId = lookup[0]["worker_id"].as<int64_t>();
    auto projectId = lookup[0]["project_id"].as<int64_t>();

    // Insert build result using stream binder for nullable fields
    auto binder = *db <<
        "INSERT INTO build_results "
        "(project_id, kiln_commit_id, project_commit, worker_id, "
        "compiler, compiler_version, status, test_status, test_duration_seconds, "
        "cmake_fallback_status, cmake_version, duration_seconds, "
        "cmake_duration_seconds, started_at, finished_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, CAST($7 AS build_status), "
        "CAST($8 AS build_status), $9, CAST($10 AS build_status), $11, $12, $13, "
        "now() - make_interval(secs => $14), now())";

    binder << projectId;
    if (lookup[0]["kiln_commit_id"].isNull()) binder << nullptr;
    else binder << lookup[0]["kiln_commit_id"].as<int64_t>();
    binder << rr.project_commit << workerId
           << rr.compiler << rr.compiler_version << rr.status;
    if (rr.test_status) binder << *rr.test_status; else binder << nullptr;
    if (rr.test_duration_seconds) binder << *rr.test_duration_seconds; else binder << nullptr;
    if (rr.cmake_fallback_status) binder << *rr.cmake_fallback_status; else binder << nullptr;
    if (rr.cmake_version) binder << *rr.cmake_version; else binder << nullptr;
    binder << rr.duration_seconds;
    if (rr.cmake_duration_seconds) binder << *rr.cmake_duration_seconds; else binder << nullptr;
    binder << static_cast<double>(rr.duration_seconds);

    co_await drogon::orm::internal::SqlAwaiter(std::move(binder));

    // Delete the active job
    co_await db->execSqlCoro("DELETE FROM active_jobs WHERE id=$1", rr.job_id);

    co_return error_response(k200OK);
}

} // namespace kiln
