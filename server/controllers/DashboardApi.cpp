#include "DashboardApi.h"
#include "common/protocol.h"
#include "server/json_response.h"
#include <drogon/orm/DbClient.h>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

Task<HttpResponsePtr> DashboardApi::matrix(HttpRequestPtr req) {
    auto db = app().getDbClient();

    std::vector<MatrixCell> cells;

    // Latest build result per project per platform
    auto builds = co_await db->execSqlCoro(
        "SELECT DISTINCT ON (br.project_id, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || br.compiler) "
        "br.project_id, p.name AS project_name, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || br.compiler AS platform, "
        "br.status::text, br.cmake_fallback_status::text, "
        "br.finished_at::text, br.id AS build_result_id "
        "FROM build_results br "
        "JOIN projects p ON p.id = br.project_id "
        "LEFT JOIN workers w ON w.id = br.worker_id "
        "WHERE p.enabled = true "
        "ORDER BY br.project_id, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || br.compiler, "
        "br.finished_at DESC");

    for (const auto &row : builds) {
        MatrixCell c{
            .project_id = row["project_id"].as<int64_t>(),
            .project_name = row["project_name"].as<std::string>(),
            .platform = row["platform"].as<std::string>(),
        };

        auto status = row["status"].as<std::string>();
        auto cmakeFb = row["cmake_fallback_status"].isNull()
            ? std::nullopt : std::optional{row["cmake_fallback_status"].as<std::string>()};

        if (status == "pass")
            c.status = "pass";
        else if (cmakeFb && *cmakeFb == "pass")
            c.status = "fail"; // kiln bug
        else
            c.status = "both_fail";

        c.cmake_fallback_status = cmakeFb;
        c.finished_at = row["finished_at"].isNull()
            ? std::nullopt : std::optional{row["finished_at"].as<std::string>()};
        c.build_result_id = row["build_result_id"].as<int64_t>();
        cells.push_back(std::move(c));
    }

    // Active jobs as "building"
    auto jobs = co_await db->execSqlCoro(
        "SELECT aj.project_id, p.name AS project_name, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') AS platform "
        "FROM active_jobs aj "
        "JOIN projects p ON p.id = aj.project_id "
        "LEFT JOIN workers w ON w.id = aj.worker_id");

    for (const auto &row : jobs) {
        cells.push_back({
            .project_id = row["project_id"].as<int64_t>(),
            .project_name = row["project_name"].as<std::string>(),
            .platform = row["platform"].as<std::string>(),
            .status = "building",
        });
    }

    co_return json_response(cells);
}

Task<HttpResponsePtr> DashboardApi::workers(HttpRequestPtr req) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT w.id, w.name, w.arch, w.os, w.os_version, w.cpu_model, "
        "w.cores, w.ram_mb, w.resource_tier_max::text, w.last_seen::text, "
        "p.name AS current_job "
        "FROM workers w "
        "LEFT JOIN active_jobs aj ON aj.worker_id = w.id "
        "LEFT JOIN projects p ON p.id = aj.project_id "
        "ORDER BY w.last_seen DESC");

    std::vector<WorkerInfo> result;
    for (const auto &row : r) {
        result.push_back({
            .id = row["id"].as<int64_t>(),
            .name = row["name"].as<std::string>(),
            .arch = row["arch"].as<std::string>(),
            .os = row["os"].as<std::string>(),
            .os_version = row["os_version"].as<std::string>(),
            .cpu_model = row["cpu_model"].as<std::string>(),
            .cores = row["cores"].as<int>(),
            .ram_mb = row["ram_mb"].as<int>(),
            .resource_tier_max = row["resource_tier_max"].as<std::string>(),
            .last_seen = row["last_seen"].as<std::string>(),
            .current_job = row["current_job"].isNull()
                ? std::nullopt : std::optional{row["current_job"].as<std::string>()},
        });
    }
    co_return json_response(result);
}

Task<HttpResponsePtr> DashboardApi::projectHistory(HttpRequestPtr req, int64_t id) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT br.id, p.name AS project_name, "
        "COALESCE(kc.git_hash, '') AS kiln_hash, "
        "br.project_commit, COALESCE(w.name, '(deleted)') AS worker_name, "
        "br.compiler, br.compiler_version, br.status::text, "
        "br.test_status::text, br.cmake_fallback_status::text, "
        "br.duration_seconds, br.cmake_duration_seconds, "
        "br.started_at::text, br.finished_at::text "
        "FROM build_results br "
        "JOIN projects p ON p.id = br.project_id "
        "LEFT JOIN workers w ON w.id = br.worker_id "
        "LEFT JOIN kiln_commits kc ON kc.id = br.kiln_commit_id "
        "WHERE br.project_id = $1 "
        "ORDER BY br.finished_at DESC "
        "LIMIT 100",
        id);

    std::vector<BuildResultInfo> results;
    for (const auto &row : r) {
        results.push_back({
            .id = row["id"].as<int64_t>(),
            .project_name = row["project_name"].as<std::string>(),
            .kiln_hash = row["kiln_hash"].as<std::string>(),
            .project_commit = row["project_commit"].as<std::string>(),
            .worker_name = row["worker_name"].as<std::string>(),
            .compiler = row["compiler"].as<std::string>(),
            .compiler_version = row["compiler_version"].as<std::string>(),
            .status = row["status"].as<std::string>(),
            .test_status = row["test_status"].isNull()
                ? std::nullopt : std::optional{row["test_status"].as<std::string>()},
            .cmake_fallback_status = row["cmake_fallback_status"].isNull()
                ? std::nullopt : std::optional{row["cmake_fallback_status"].as<std::string>()},
            .duration_seconds = row["duration_seconds"].as<int>(),
            .cmake_duration_seconds = row["cmake_duration_seconds"].isNull()
                ? std::optional<int>{} : std::optional{row["cmake_duration_seconds"].as<int>()},
            .started_at = row["started_at"].as<std::string>(),
            .finished_at = row["finished_at"].as<std::string>(),
        });
    }
    co_return json_response(results);
}

Task<HttpResponsePtr> DashboardApi::buildLog(HttpRequestPtr req, int64_t id) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT log_path, cmake_log_path FROM build_results WHERE id=$1", static_cast<int64_t>(id));

    if (r.empty())
        co_return error_response(k404NotFound);

    co_return json_response(LogPathsResponse{
        .log_path = r[0]["log_path"].isNull() ? "" : r[0]["log_path"].as<std::string>(),
        .cmake_log_path = r[0]["cmake_log_path"].isNull() ? "" : r[0]["cmake_log_path"].as<std::string>(),
    });
}

Task<HttpResponsePtr> DashboardApi::trend(HttpRequestPtr req) {
    auto db = app().getDbClient();

    // Default to 30 days, allow ?days=N
    auto daysParam = req->getParameter("days");
    int days = 30;
    if (!daysParam.empty()) {
        try { days = std::stoi(daysParam); } catch (...) {}
        if (days < 1) days = 1;
        if (days > 365) days = 365;
    }

    // Count of distinct projects with a passing build per day
    auto passing = co_await db->execSqlCoro(
        "SELECT d.date::text, "
        "  COUNT(DISTINCT CASE WHEN br.status = 'pass' THEN br.project_id END) AS passing, "
        "  COUNT(DISTINCT CASE WHEN br.status != 'pass' "
        "    AND br.cmake_fallback_status = 'pass' THEN br.project_id END) AS kiln_bugs "
        "FROM generate_series("
        "  (current_date - make_interval(days => $1))::date, "
        "  current_date, '1 day') AS d(date) "
        "LEFT JOIN build_results br "
        "  ON br.finished_at::date = d.date "
        "GROUP BY d.date "
        "ORDER BY d.date",
        days);

    // Total enabled projects (constant line)
    auto totalResult = co_await db->execSqlCoro(
        "SELECT COUNT(*) AS total FROM projects WHERE enabled = true");
    int totalProjects = totalResult.empty() ? 0 : totalResult[0]["total"].as<int>();

    std::vector<TrendPoint> points;
    for (const auto &row : passing) {
        points.push_back({
            .date = row["date"].as<std::string>(),
            .passing = row["passing"].as<int>(),
            .kiln_bugs = row["kiln_bugs"].as<int>(),
            .total_projects = totalProjects,
        });
    }

    co_return json_response(points);
}

} // namespace kiln
