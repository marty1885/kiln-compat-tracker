#include "server/migrate.h"
#include "server/config.h"
#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <glaze/glaze.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
struct GhCommitResponse { std::string sha; };
}

int main(int argc, char *argv[]) {
    if (auto *tok = std::getenv("GITHUB_TOKEN"); tok && tok[0] != '\0') {
        kiln::github_token = tok;
    } else {
        std::cerr << "GITHUB_TOKEN environment variable is required\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--log-dir" && i + 1 < argc) {
            kiln::log_dir = argv[++i];
        }
    }
    if (kiln::log_dir.empty()) {
        std::cerr << "Usage: kiln-server --log-dir <path>\n";
        return 1;
    }
    std::filesystem::create_directories(kiln::log_dir);
    std::cout << "Log storage: " << kiln::log_dir << "\n";
    auto &app = drogon::app();

    // Serve static files from server/static/
    app.setDocumentRoot("./server/static");

    // Database connection
    drogon::orm::PostgresConfig pgConfig;
    pgConfig.host = "localhost";
    pgConfig.port = 5432;
    pgConfig.databaseName = "kiln-compat-tracker";
    pgConfig.username = "kiln-compat-tracker";
    pgConfig.password = "";
    pgConfig.connectionNumber = 2;
    pgConfig.name = "default";
    pgConfig.isFast = false;
    pgConfig.characterSet = "utf8";
    pgConfig.timeout = 30.0;
    pgConfig.autoBatch = false;
    app.addDbClient(pgConfig);

    // Run migrations before starting the event loop
    app.registerBeginningAdvice([] {
        auto db = drogon::app().getDbClient();
        try {
            kiln::run_migrations(db);
        } catch (const std::exception &e) {
            std::cerr << "Migration failed, shutting down: " << e.what() << "\n";
            drogon::app().quit();
        }
    });

    app.enableSession(3600);
    app.addListener("0.0.0.0", 7621);

    // Reap orphaned jobs and clean up old reaped rows periodically.
    app.getLoop()->runEvery(60.0, [] {
        auto db = drogon::app().getDbClient();

        // Mark jobs as reaped if heartbeat is stale (>10 min)
        db->execSqlAsync(
            "UPDATE active_jobs SET reaped_at = now() "
            "WHERE heartbeat_at < now() - interval '10 minutes' "
            "AND reaped_at IS NULL",
            [](const drogon::orm::Result &r) {
                if (r.affectedRows() > 0)
                    std::cout << "Reaped " << r.affectedRows()
                              << " stale active job(s)\n";
            },
            [](const drogon::orm::DrogonDbException &e) {
                std::cerr << "Job reaper error: " << e.base().what() << "\n";
            });

        // Delete reaped rows older than 24h that were never reclaimed
        db->execSqlAsync(
            "DELETE FROM active_jobs "
            "WHERE reaped_at IS NOT NULL "
            "AND reaped_at < now() - interval '24 hours'",
            [](const drogon::orm::Result &r) {
                if (r.affectedRows() > 0)
                    std::cout << "Cleaned up " << r.affectedRows()
                              << " expired reaped job(s)\n";
            },
            [](const drogon::orm::DrogonDbException &e) {
                std::cerr << "Reaped job cleanup error: " << e.base().what() << "\n";
            });
    });

    // --- GitHub poller: fetch latest kiln master commit at 00:00 and 12:00 UTC ---

    auto pollKilnHead = [] {
        drogon::async_run([&]() -> drogon::Task<> {
            try {
                auto client = drogon::HttpClient::newHttpClient(
                    "https://api.github.com", drogon::app().getLoop());
                auto req = drogon::HttpRequest::newHttpRequest();
                req->setPath("/repos/marty1885/kiln/commits/master");
                req->addHeader("Authorization", "token " + kiln::github_token);
                req->addHeader("Accept", "application/vnd.github.v3+json");
                req->addHeader("User-Agent", "kiln-compat-tracker");

                auto resp = co_await client->sendRequestCoro(req);
                if (resp->statusCode() != drogon::k200OK) {
                    std::cerr << "GitHub API returned " << resp->statusCode()
                              << ": " << resp->body() << "\n";
                    co_return;
                }

                GhCommitResponse gc;
                auto body = std::string(resp->body());
                if (auto err = glz::read_json(gc, body); err) {
                    std::cerr << "Failed to parse GitHub response: "
                              << glz::format_error(err, body) << "\n";
                    co_return;
                }
                if (gc.sha.empty()) co_return;

                auto db = drogon::app().getDbClient();
                auto r = co_await db->execSqlCoro(
                    "SELECT value FROM config WHERE key='current_kiln_hash'");
                auto current = r.empty()
                    ? std::string{} : r[0]["value"].as<std::string>();

                if (current == gc.sha) {
                    std::cout << "Kiln master unchanged: "
                              << gc.sha.substr(0, 12) << "\n";
                    co_return;
                }

                std::cout << "Kiln master updated: " << gc.sha << "\n";
                co_await db->execSqlCoro(
                    "UPDATE config SET value=$1 "
                    "WHERE key='current_kiln_hash'", gc.sha);
                co_await db->execSqlCoro(
                    "INSERT INTO kiln_commits (git_hash) "
                    "VALUES ($1) ON CONFLICT DO NOTHING", gc.sha);
            } catch (const std::exception &e) {
                std::cerr << "Kiln poll error: " << e.what() << "\n";
            }
        });
    };

    // Schedule polls at 00:00 and 12:00 UTC, repeating daily.
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto today = floor<days>(now);

        // Next 00:00 and 12:00 UTC (push to tomorrow if already past)
        auto midnight = today + hours(24);
        auto noon = today + hours(12);
        if (noon <= now) noon += hours(24);

        auto schedule = [&app, pollKilnHead](system_clock::time_point tp) {
            auto us = duration_cast<microseconds>(tp.time_since_epoch()).count();
            auto date = trantor::Date(us);
            std::cout << "Kiln poll scheduled: " << date.toFormattedString(false) << " UTC\n";
            app.getLoop()->runAt(date, [&app, pollKilnHead] {
                pollKilnHead();
                // Reschedule for same time tomorrow
                app.getLoop()->runEvery(24.0 * 3600.0, pollKilnHead);
            });
        };

        schedule(midnight);
        schedule(noon);
    }

    std::cout << "Kiln Compat Tracker starting on http://localhost:7621\n";
    app
        .setClientMaxMemoryBodySize(256 * 1024 * 1024)
        .setClientMaxBodySize(256 * 1024 * 1024)
        .enableCompressedRequest()
        .run();
    return 0;
}
