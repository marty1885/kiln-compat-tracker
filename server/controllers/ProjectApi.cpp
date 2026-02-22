#include "ProjectApi.h"
#include "common/protocol.h"
#include "server/auth.h"
#include "server/json_response.h"
#include <drogon/orm/DbClient.h>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

Task<HttpResponsePtr> ProjectApi::list(HttpRequestPtr req) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT id, name, repo_url, branch, pinned_commit, extra_cmake_args, "
        "run_tests, test_resource_tier_min::text, resource_tier::text, "
        "dep_level::text, os_filter, cooldown_minutes, enabled "
        "FROM projects ORDER BY name");

    std::vector<ProjectInfo> projects;
    for (const auto &row : r) {
        projects.push_back({
            .id = row["id"].as<int64_t>(),
            .name = row["name"].as<std::string>(),
            .repo_url = row["repo_url"].as<std::string>(),
            .branch = row["branch"].as<std::string>(),
            .pinned_commit = row["pinned_commit"].isNull()
                ? std::nullopt : std::optional{row["pinned_commit"].as<std::string>()},
            .extra_cmake_args = row["extra_cmake_args"].isNull()
                ? std::nullopt : std::optional{row["extra_cmake_args"].as<std::string>()},
            .run_tests = row["run_tests"].as<bool>(),
            .test_resource_tier_min = row["test_resource_tier_min"].isNull()
                ? std::nullopt : std::optional{row["test_resource_tier_min"].as<std::string>()},
            .resource_tier = row["resource_tier"].as<std::string>(),
            .dep_level = row["dep_level"].as<std::string>(),
            .os_filter = row["os_filter"].as<std::string>(),
            .cooldown_minutes = row["cooldown_minutes"].as<int>(),
            .enabled = row["enabled"].as<bool>(),
        });
    }
    co_return json_response(projects);
}

Task<HttpResponsePtr> ProjectApi::create(HttpRequestPtr req) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    ProjectCreateRequest pc{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(pc, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();
    auto binder = *db <<
        "INSERT INTO projects (name, repo_url, branch, pinned_commit, extra_cmake_args, "
        "run_tests, test_resource_tier_min, resource_tier, dep_level, os_filter, cooldown_minutes) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7::resource_tier, $8::resource_tier, $9::dep_level, $10, $11) "
        "RETURNING id";

    binder << pc.name << pc.repo_url << pc.branch;
    if (pc.pinned_commit) binder << *pc.pinned_commit; else binder << nullptr;
    if (pc.extra_cmake_args) binder << *pc.extra_cmake_args; else binder << nullptr;
    binder << pc.run_tests;
    if (pc.test_resource_tier_min) binder << *pc.test_resource_tier_min; else binder << nullptr;
    binder << pc.resource_tier << pc.dep_level << pc.os_filter << pc.cooldown_minutes;

    auto r = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
    co_return json_response(IdResponse{r[0]["id"].as<int64_t>()}, k201Created);
}

Task<HttpResponsePtr> ProjectApi::update(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    ProjectCreateRequest pc{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(pc, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();
    auto binder = *db <<
        "UPDATE projects SET name=$1, repo_url=$2, branch=$3, pinned_commit=$4, "
        "extra_cmake_args=$5, run_tests=$6, test_resource_tier_min=$7::resource_tier, "
        "resource_tier=$8::resource_tier, dep_level=$9::dep_level, os_filter=$10, "
        "cooldown_minutes=$11 WHERE id=$12";

    binder << pc.name << pc.repo_url << pc.branch;
    if (pc.pinned_commit) binder << *pc.pinned_commit; else binder << nullptr;
    if (pc.extra_cmake_args) binder << *pc.extra_cmake_args; else binder << nullptr;
    binder << pc.run_tests;
    if (pc.test_resource_tier_min) binder << *pc.test_resource_tier_min; else binder << nullptr;
    binder << pc.resource_tier << pc.dep_level << pc.os_filter << pc.cooldown_minutes << id;

    co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
    co_return error_response(k200OK);
}

Task<HttpResponsePtr> ProjectApi::remove(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();
    co_await db->execSqlCoro("DELETE FROM projects WHERE id=$1", id);
    co_return error_response(k200OK);
}

Task<HttpResponsePtr> ProjectApi::toggleEnable(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "UPDATE projects SET enabled = NOT enabled WHERE id=$1 RETURNING enabled", id);

    if (r.empty())
        co_return error_response(k404NotFound);

    co_return json_response(EnabledResponse{r[0]["enabled"].as<bool>()});
}

Task<HttpResponsePtr> ProjectApi::trigger(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "UPDATE projects SET forced_rebuild_after = now() WHERE id=$1",
        id);

    // Clear any active jobs so the project is immediately re-schedulable.
    // Without this, stale jobs from crashed workers (or even live ones) block
    // the NOT EXISTS(active_jobs) check in the scheduler query.
    co_await db->execSqlCoro(
        "DELETE FROM active_jobs WHERE project_id = $1", id);

    co_return error_response(k200OK);
}

} // namespace kiln
