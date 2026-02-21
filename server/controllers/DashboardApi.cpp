#include "DashboardApi.h"
#include "common/protocol.h"
#include "server/config.h"
#include "server/json_response.h"
#include <drogon/orm/DbClient.h>
#include <filesystem>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

Task<HttpResponsePtr> DashboardApi::matrix(HttpRequestPtr req) {
    auto db = app().getDbClient();

    std::vector<MatrixCell> cells;

    // Latest build result per project per platform (arch-os-distro-compiler)
    auto builds = co_await db->execSqlCoro(
        "SELECT DISTINCT ON (br.project_id, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || COALESCE(w.distro,'') || '-' || br.compiler) "
        "br.project_id, p.name AS project_name, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || COALESCE(w.distro,'') || '-' || br.compiler AS platform, "
        "br.status::text, br.cmake_fallback_status::text, "
        "br.finished_at::text, br.id AS build_result_id "
        "FROM build_results br "
        "JOIN projects p ON p.id = br.project_id "
        "LEFT JOIN workers w ON w.id = br.worker_id "
        "WHERE p.enabled = true "
        "ORDER BY br.project_id, "
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || COALESCE(w.distro,'') || '-' || br.compiler, "
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
        "COALESCE(w.arch,'') || '-' || COALESCE(w.os,'') || '-' || "
        "COALESCE(w.distro,'') || '-' || COALESCE(w.compiler,'') AS platform "
        "FROM active_jobs aj "
        "JOIN projects p ON p.id = aj.project_id "
        "LEFT JOIN workers w ON w.id = aj.worker_id "
        "WHERE aj.reaped_at IS NULL");

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
        "SELECT w.id, w.name, w.arch, w.os, w.os_version, w.distro, w.cpu_model, "
        "w.cores, w.ram_mb, w.resource_tier_max::text, w.dep_level_max::text, "
        "to_char(w.last_seen AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS last_seen, "
        "EXTRACT(EPOCH FROM now() - w.last_seen)::bigint AS age_seconds, "
        "p.name AS current_job "
        "FROM workers w "
        "LEFT JOIN active_jobs aj ON aj.worker_id = w.id AND aj.reaped_at IS NULL "
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
            .distro = row["distro"].as<std::string>(),
            .cpu_model = row["cpu_model"].as<std::string>(),
            .cores = row["cores"].as<int>(),
            .ram_mb = row["ram_mb"].as<int>(),
            .resource_tier_max = row["resource_tier_max"].as<std::string>(),
            .dep_level_max = row["dep_level_max"].as<std::string>(),
            .last_seen = row["last_seen"].as<std::string>(),
            .age_seconds = row["age_seconds"].as<int64_t>(),
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
        "br.started_at::text, br.finished_at::text, "
        "br.log_path IS NOT NULL AS has_log, "
        "br.cmake_log_path IS NOT NULL AS has_cmake_log "
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
            .has_log = row["has_log"].as<bool>(),
            .has_cmake_log = row["has_cmake_log"].as<bool>(),
        });
    }
    co_return json_response(results);
}

// Validates that a log path from the DB is inside the expected log directory
// before serving it, as defense-in-depth against a compromised DB row leading
// to an arbitrary file read.
static bool is_safe_log_path(const std::string &path) {
    auto canonical_log_dir = std::filesystem::weakly_canonical(kiln::log_dir);
    auto candidate = std::filesystem::weakly_canonical(path);
    auto [end, _] = std::mismatch(canonical_log_dir.begin(), canonical_log_dir.end(),
                                   candidate.begin());
    return end == canonical_log_dir.end();
}

Task<HttpResponsePtr> DashboardApi::buildLog(HttpRequestPtr req, int64_t id) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT log_path FROM build_results WHERE id=$1", id);
    if (r.empty() || r[0]["log_path"].isNull())
        co_return error_response(k404NotFound, "No log available");
    auto path = r[0]["log_path"].as<std::string>();
    if (!is_safe_log_path(path))
        co_return error_response(k403Forbidden, "Invalid log path");
    if (!std::filesystem::exists(path))
        co_return error_response(k404NotFound, "Log file missing on disk");
    auto resp = HttpResponse::newFileResponse(path);
    resp->addHeader("Content-Disposition",
        "attachment; filename=\"build-" + std::to_string(id) + ".log\"");
    co_return resp;
}

Task<HttpResponsePtr> DashboardApi::buildCmakeLog(HttpRequestPtr req, int64_t id) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT cmake_log_path FROM build_results WHERE id=$1", id);
    if (r.empty() || r[0]["cmake_log_path"].isNull())
        co_return error_response(k404NotFound, "No CMake log available");
    auto path = r[0]["cmake_log_path"].as<std::string>();
    if (!is_safe_log_path(path))
        co_return error_response(k403Forbidden, "Invalid log path");
    if (!std::filesystem::exists(path))
        co_return error_response(k404NotFound, "CMake log file missing on disk");
    auto resp = HttpResponse::newFileResponse(path);
    resp->addHeader("Content-Disposition",
        "attachment; filename=\"cmake-build-" + std::to_string(id) + ".log\"");
    co_return resp;
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

    // Per-project, per-day: did it pass? did it show a kiln bug?
    // Then aggregate: kiln_bugs only counts projects with NO passing build that day.
    auto passing = co_await db->execSqlCoro(
        "WITH daily AS ("
        "  SELECT br.finished_at::date AS day, br.project_id,"
        "    BOOL_OR(br.status = 'pass') AS any_pass,"
        "    BOOL_OR(br.status != 'pass' AND br.cmake_fallback_status = 'pass') AS any_kiln_bug"
        "  FROM build_results br"
        "  GROUP BY br.finished_at::date, br.project_id"
        ")"
        "SELECT d.date::text,"
        "  COUNT(DISTINCT CASE WHEN ds.any_pass THEN ds.project_id END) AS passing,"
        "  COUNT(DISTINCT CASE WHEN NOT ds.any_pass AND ds.any_kiln_bug THEN ds.project_id END) AS kiln_bugs "
        "FROM generate_series("
        "  (current_date - make_interval(days => $1))::date,"
        "  current_date, '1 day') AS d(date) "
        "LEFT JOIN daily ds ON ds.day = d.date "
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
