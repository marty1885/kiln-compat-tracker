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
        "SELECT w.id, w.name, w.auth_token, w.arch, w.os, w.os_version, w.distro, w.max_jobs, "
        "w.resource_tier_max::text, w.dep_level_max::text, "
        "to_char(w.last_seen AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS last_seen, "
        "EXTRACT(EPOCH FROM now() - w.last_seen)::bigint AS age_seconds, "
        "p.name AS current_job "
        "FROM workers w "
        "LEFT JOIN active_jobs aj ON aj.worker_id = w.id AND aj.reaped_at IS NULL "
        "LEFT JOIN projects p ON p.id = aj.project_id "
        "ORDER BY w.name");

    std::vector<WorkerAdminInfo> result;
    for (const auto &row : r) {
        result.push_back({
            .id = row["id"].as<int64_t>(),
            .name = row["name"].as<std::string>(),
            .auth_token_prefix = row["auth_token"].as<std::string>().substr(0, 8),
            .arch = row["arch"].as<std::string>(),
            .os = row["os"].as<std::string>(),
            .os_version = row["os_version"].as<std::string>(),
            .distro = row["distro"].as<std::string>(),
            .max_jobs = row["max_jobs"].as<int>(),
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
        wc.resource_tier_max != "large" &&
        wc.resource_tier_max != "xlarge")
        co_return error_response(k400BadRequest, "Invalid resource tier");

    if (wc.dep_level_max != "base" &&
        wc.dep_level_max != "moderate" &&
        wc.dep_level_max != "full")
        co_return error_response(k400BadRequest, "Invalid dep level");

    auto token = generate_token();
    auto db = app().getDbClient();

    try {
        auto r = co_await db->execSqlCoro(
            "INSERT INTO workers (name, auth_token, resource_tier_max, dep_level_max) "
            "VALUES ($1, $2, '" + wc.resource_tier_max + "'::resource_tier, "
            "'" + wc.dep_level_max + "'::dep_level) "
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

Task<HttpResponsePtr> WorkerMgmtApi::update(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    WorkerUpdateRequest wu{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(wu, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    if (wu.name && wu.name->empty())
        co_return error_response(k400BadRequest, "Worker name cannot be empty");

    if (wu.resource_tier_max) {
        auto &t = *wu.resource_tier_max;
        if (t != "small" && t != "medium" && t != "large" && t != "xlarge")
            co_return error_response(k400BadRequest, "Invalid resource tier");
    }

    if (wu.dep_level_max) {
        auto &d = *wu.dep_level_max;
        if (d != "base" && d != "moderate" && d != "full")
            co_return error_response(k400BadRequest, "Invalid dep level");
    }

    // Build SET clauses for provided fields
    std::vector<std::string> clauses;
    int param = 2; // $1 is id

    if (wu.name)
        clauses.push_back("name = $" + std::to_string(param++));
    if (wu.resource_tier_max)
        clauses.push_back("resource_tier_max = $" + std::to_string(param++) + "::resource_tier");
    if (wu.dep_level_max)
        clauses.push_back("dep_level_max = $" + std::to_string(param++) + "::dep_level");

    if (clauses.empty())
        co_return error_response(k400BadRequest, "No fields to update");

    std::string sql = "UPDATE workers SET ";
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i > 0) sql += ", ";
        sql += clauses[i];
    }
    sql += " WHERE id = $1 RETURNING id";

    auto db = app().getDbClient();
    auto binder = *db << sql;
    binder << id;
    if (wu.name)              binder << *wu.name;
    if (wu.resource_tier_max) binder << *wu.resource_tier_max;
    if (wu.dep_level_max)     binder << *wu.dep_level_max;

    try {
        auto r = co_await orm::internal::SqlAwaiter(std::move(binder));

        if (r.empty())
            co_return error_response(k404NotFound);

        co_return error_response(k200OK);
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

Task<HttpResponsePtr> WorkerMgmtApi::revealToken(HttpRequestPtr req, int64_t id) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    VerifyPasswordRequest vpr{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(vpr, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    if (vpr.password.empty())
        co_return error_response(k400BadRequest, "Password required");

    // Re-verify the calling admin's password before revealing a secret
    auto session = req->session();
    auto adminId = session->get<int64_t>("admin_id");

    auto db = app().getDbClient();
    auto adminResult = co_await db->execSqlCoro(
        "SELECT password_hash FROM admins WHERE id = $1", adminId);

    if (adminResult.empty())
        co_return error_response(k401Unauthorized, "Admin account not found");

    if (!verify_password(vpr.password, adminResult[0]["password_hash"].as<std::string>()))
        co_return error_response(k401Unauthorized, "Incorrect password");

    auto tokenResult = co_await db->execSqlCoro(
        "SELECT auth_token FROM workers WHERE id = $1", id);

    if (tokenResult.empty())
        co_return error_response(k404NotFound);

    co_return json_response(WorkerTokenResponse{
        .auth_token = tokenResult[0]["auth_token"].as<std::string>(),
    });
}

} // namespace kiln
