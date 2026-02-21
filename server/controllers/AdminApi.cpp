#include "AdminApi.h"
#include "common/protocol.h"
#include "server/auth.h"
#include "server/json_response.h"
#include <drogon/Cookie.h>
#include <drogon/orm/DbClient.h>

using namespace drogon;
using namespace drogon::orm;

namespace kiln {

Task<HttpResponsePtr> AdminApi::status(HttpRequestPtr req) {
    auto db = app().getDbClient();
    auto countResult = co_await db->execSqlCoro(
        "SELECT COUNT(*) AS cnt FROM admins");
    bool needsSetup = countResult[0]["cnt"].as<int64_t>() == 0;

    AuthStatus s{.needs_setup = needsSetup};
    auto session = req->session();
    if (session && session->find("admin_id")) {
        s.logged_in = true;
        s.username = session->get<std::string>("admin_username");
    }
    co_return json_response(s);
}

Task<HttpResponsePtr> AdminApi::registerAdmin(HttpRequestPtr req) {
    RegisterRequest rr{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(rr, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    if (rr.username.empty() || rr.password.empty())
        co_return error_response(k400BadRequest, "Username and password required");

    auto db = app().getDbClient();

    // Check if this is first-run (no admins exist)
    auto countResult = co_await db->execSqlCoro(
        "SELECT COUNT(*) AS cnt FROM admins");
    bool isFirstRun = countResult[0]["cnt"].as<int64_t>() == 0;

    int64_t inviteCreator = 0;
    if (!isFirstRun) {
        // Require valid invite token
        if (!rr.invite_token || rr.invite_token->empty())
            co_return error_response(k403Forbidden, "Invite token required");

        auto inviteResult = co_await db->execSqlCoro(
            "SELECT id, created_by FROM invite_tokens "
            "WHERE token = $1 AND used_by IS NULL",
            *rr.invite_token);

        if (inviteResult.empty())
            co_return error_response(k403Forbidden, "Invalid or already used invite token");

        inviteCreator = inviteResult[0]["id"].as<int64_t>();
    }

    // Hash password and insert admin
    auto passwordHash = hash_password(rr.password);

    try {
        auto insertResult = co_await db->execSqlCoro(
            "INSERT INTO admins (username, password_hash) VALUES ($1, $2) "
            "RETURNING id",
            rr.username, passwordHash);

        auto adminId = insertResult[0]["id"].as<int64_t>();

        // Mark invite as used (if not first-run)
        if (!isFirstRun && inviteCreator > 0) {
            co_await db->execSqlCoro(
                "UPDATE invite_tokens SET used_by = $1, used_at = now() "
                "WHERE token = $2",
                adminId, *rr.invite_token);
        }

        // Set session
        auto session = req->session();
        session->insert("admin_id", adminId);
        session->insert("admin_username", rr.username);

        co_return json_response(
            AuthStatus{.logged_in = true, .username = rr.username},
            k201Created);
    } catch (const DrogonDbException &e) {
        co_return error_response(k409Conflict, "Username already taken");
    }
}

Task<HttpResponsePtr> AdminApi::login(HttpRequestPtr req) {
    LoginRequest lr{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(lr, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    auto db = app().getDbClient();
    auto result = co_await db->execSqlCoro(
        "SELECT id, password_hash FROM admins WHERE username = $1",
        lr.username);

    if (result.empty())
        co_return error_response(k401Unauthorized, "Invalid credentials");

    auto hash = result[0]["password_hash"].as<std::string>();
    if (!verify_password(lr.password, hash))
        co_return error_response(k401Unauthorized, "Invalid credentials");

    auto adminId = result[0]["id"].as<int64_t>();
    auto session = req->session();
    session->insert("admin_id", adminId);
    session->insert("admin_username", lr.username);

    co_return json_response(
        AuthStatus{.logged_in = true, .username = lr.username});
}

Task<HttpResponsePtr> AdminApi::logout(HttpRequestPtr req) {
    auto session = req->session();
    if (session)
        session->clear();

    drogon::Cookie cookie("JSESSIONID", "");
    cookie.setPath("/");
    cookie.setMaxAge(0);

    auto resp = error_response(k200OK);
    resp->addCookie(std::move(cookie));
    co_return resp;
}

Task<HttpResponsePtr> AdminApi::invite(HttpRequestPtr req) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    auto session = req->session();
    auto adminId = session->get<int64_t>("admin_id");

    auto token = generate_token();
    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "INSERT INTO invite_tokens (token, created_by) VALUES ($1, $2)",
        token, adminId);

    // Build invite URL from request host
    auto host = req->getHeader("Host");
    auto url = "http://" + host + "/?invite=" + token;

    co_return json_response(InviteResponse{.token = token, .url = url});
}

Task<HttpResponsePtr> AdminApi::getKilnHash(HttpRequestPtr req) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT value FROM config WHERE key='current_kiln_hash'");
    auto hash = r.empty() ? "" : r[0]["value"].as<std::string>();
    co_return json_response(KilnHashInfo{.git_hash = hash});
}

Task<HttpResponsePtr> AdminApi::setKilnHash(HttpRequestPtr req) {
    if (!is_admin(req))
        co_return error_response(k401Unauthorized);

    KilnHashRequest kr{};
    auto body = std::string(req->body());
    if (auto err = glz::read_json(kr, body); err)
        co_return error_response(k400BadRequest, glz::format_error(err, body));

    if (kr.git_hash.empty())
        co_return error_response(k400BadRequest, "git_hash required");

    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "UPDATE config SET value=$1 WHERE key='current_kiln_hash'",
        kr.git_hash);

    co_return error_response(k200OK);
}

} // namespace kiln
