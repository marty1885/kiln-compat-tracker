#include "WorkerMgmtApi.h"
#include "common/protocol.h"
#include "server/auth.h"
#include "server/json_response.h"
#include <drogon/orm/DbClient.h>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

Task<HttpResponsePtr> WorkerMgmtApi::list(HttpRequestPtr req) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT w.id, w.name, w.auth_token, w.arch, w.os, w.os_version, "
        "w.resource_tier_max::text, w.last_seen::text, "
        "p.name AS current_job "
        "FROM workers w "
        "LEFT JOIN active_jobs aj ON aj.worker_id = w.id "
        "LEFT JOIN projects p ON p.id = aj.project_id "
        "ORDER BY w.name");

    std::vector<WorkerAdminInfo> result;
    for (const auto &row : r) {
        result.push_back({
            .id = row["id"].as<int64_t>(),
            .name = row["name"].as<std::string>(),
            .auth_token = row["auth_token"].as<std::string>(),
            .arch = row["arch"].as<std::string>(),
            .os = row["os"].as<std::string>(),
            .os_version = row["os_version"].as<std::string>(),
            .resource_tier_max = row["resource_tier_max"].as<std::string>(),
            .last_seen = row["last_seen"].as<std::string>(),
            .current_job = row["current_job"].isNull()
                ? std::nullopt : std::optional{row["current_job"].as<std::string>()},
        });
    }
    co_return json_response(result);
}

Task<HttpResponsePtr> WorkerMgmtApi::create(HttpRequestPtr req) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    WorkerCreateRequest wc{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(wc, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    if (wc.name.empty())
        co_return error_response(k400BadRequest, "Worker name required");

    // Validate resource tier
    if (wc.resource_tier_max != "small" &&
        wc.resource_tier_max != "medium" &&
        wc.resource_tier_max != "large")
        co_return error_response(k400BadRequest, "Invalid resource tier");

    auto token = generate_token();
    auto db = app().getDbClient();

    try {
        auto r = co_await db->execSqlCoro(
            "INSERT INTO workers (name, auth_token, resource_tier_max) "
            "VALUES ($1, $2, '" + wc.resource_tier_max + "'::resource_tier) "
            "RETURNING id",
            wc.name, token);

        co_return json_response(
            WorkerCreateResponse{
                .id = r[0]["id"].as<int64_t>(),
                .auth_token = token,
            },
            k201Created);
    } catch (const DrogonDbException &) {
        co_return error_response(k409Conflict, "Worker name already exists");
    }
}

Task<HttpResponsePtr> WorkerMgmtApi::remove(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "DELETE FROM workers WHERE id = $1 RETURNING id", id);

    if (r.empty())
        co_return error_response(k404NotFound);

    co_return error_response(k200OK);
}

} // namespace kiln
