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
        "SELECT id, name, repo_url, branch, pinned_commit, build_command, "
        "run_tests, test_resource_tier_min::text, resource_tier::text, "
        "cooldown_minutes, enabled "
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
            .build_command = row["build_command"].isNull()
                ? std::nullopt : std::optional{row["build_command"].as<std::string>()},
            .run_tests = row["run_tests"].as<bool>(),
            .test_resource_tier_min = row["test_resource_tier_min"].isNull()
                ? std::nullopt : std::optional{row["test_resource_tier_min"].as<std::string>()},
            .resource_tier = row["resource_tier"].as<std::string>(),
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
        "INSERT INTO projects (name, repo_url, branch, pinned_commit, build_command, "
        "run_tests, test_resource_tier_min, resource_tier, cooldown_minutes) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7::resource_tier, $8::resource_tier, $9) "
        "RETURNING id";

    binder << pc.name << pc.repo_url << pc.branch;
    if (pc.pinned_commit) binder << *pc.pinned_commit; else binder << nullptr;
    if (pc.build_command) binder << *pc.build_command; else binder << nullptr;
    binder << pc.run_tests;
    if (pc.test_resource_tier_min) binder << *pc.test_resource_tier_min; else binder << nullptr;
    binder << pc.resource_tier << pc.cooldown_minutes;

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
        "build_command=$5, run_tests=$6, test_resource_tier_min=$7::resource_tier, "
        "resource_tier=$8::resource_tier, cooldown_minutes=$9 "
        "WHERE id=$10";

    binder << pc.name << pc.repo_url << pc.branch;
    if (pc.pinned_commit) binder << *pc.pinned_commit; else binder << nullptr;
    if (pc.build_command) binder << *pc.build_command; else binder << nullptr;
    binder << pc.run_tests;
    if (pc.test_resource_tier_min) binder << *pc.test_resource_tier_min; else binder << nullptr;
    binder << pc.resource_tier << pc.cooldown_minutes << id;

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

    co_return error_response(k200OK);
}

} // namespace kiln
