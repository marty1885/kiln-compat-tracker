#include "worker/config.h"
#include "worker/platform.h"
#include "worker/builder.h"
#include "common/protocol.h"

#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <glaze/glaze.hpp>

#include <iostream>
#include <random>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace kiln;
using namespace drogon;

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

class Worker {
public:
    Worker(WorkerConfig config)
        : config_(std::move(config))
        , sysinfo_(detect_system_info())
        , compiler_(detect_compiler())
        , compiler_version_(detect_compiler_version())
        , rng_(std::random_device{}()) {

        client_ = HttpClient::newHttpClient(config_.server_url);
    }

    Task<> run() {
        std::cout << "Worker starting\n";
        std::cout << "  " << sysinfo_.arch << "/" << sysinfo_.os
                  << " " << sysinfo_.os_version << "\n";
        std::cout << "  " << sysinfo_.cpu_model << " (" << sysinfo_.cores
                  << " cores, " << sysinfo_.ram_mb << " MB RAM)\n";
        std::cout << "  Compiler: " << compiler_ << " " << compiler_version_ << "\n";
        std::cout << "  Server: " << config_.server_url << "\n\n";

        int backoff = 0;
        while (g_running) {
            // co_await is forbidden inside catch handlers, so capture the outcome
            // first, then do all sleeping after the try/catch.
            LoopResult result = LoopResult::network_error;
            try {
                result = co_await loop_iteration();
            } catch (const std::exception &e) {
                std::cerr << "Unexpected worker loop error: " << e.what() << "\n";
                // result stays network_error, backoff will apply below
            }

            if (result == LoopResult::network_error) {
                backoff = (backoff == 0) ? 2 : std::min(backoff * 2, 60);
                std::cerr << "Network unavailable — retrying in " << backoff << "s\n";
                co_await jittered_sleep(backoff);
            } else {
                backoff = 0;
                if (result == LoopResult::work_done)
                    co_await sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 5.0);
                else
                    co_await sleep_with_jitter();
            }
        }
    }

private:
    enum class LoopResult { work_done, no_work, network_error };

    struct PollOutcome {
        bool network_ok{true};
        std::optional<PollResponse> job;
    };

    // Wraps sendRequestCoro so it never throws — returns nullptr on network failure.
    Task<HttpResponsePtr> send_request(const HttpRequestPtr &req) {
        try {
            co_return co_await client_->sendRequestCoro(req);
        } catch (const std::exception &e) {
            std::cerr << "Network error: " << e.what() << "\n";
            co_return nullptr;
        }
    }

    Task<LoopResult> loop_iteration() {
        // Rate-limit heartbeats: send at most once per poll_interval_seconds,
        // regardless of how fast the poll loop runs (e.g. after completing work).
        auto now = std::chrono::steady_clock::now();
        bool time_for_heartbeat =
            last_heartbeat_ == std::chrono::steady_clock::time_point{} ||
            now - last_heartbeat_ >= std::chrono::seconds(config_.poll_interval_seconds);

        if (time_for_heartbeat) {
            if (!co_await send_heartbeat())
                co_return LoopResult::network_error;
            last_heartbeat_ = std::chrono::steady_clock::now();
        }

        auto outcome = co_await poll_for_job();
        if (!outcome.network_ok)
            co_return LoopResult::network_error;
        if (!outcome.job)
            co_return LoopResult::no_work;

        const auto &job = *outcome.job;
        std::cout << "Got job #" << job.job_id << ": " << job.project_name << "\n";

        // Prepare project
        std::string project_dir;
        std::string project_commit;
        std::string actual_kiln_hash;
        bool prep_failed = false;
        std::string prep_error;

        try {
            actual_kiln_hash = ensure_kiln(config_, job.kiln_git_hash);
            project_commit = prepare_project(config_, job);
            project_dir = (std::filesystem::path(config_.project_cache_dir()) /
                           job.project_name).string();
        } catch (const std::exception &e) {
            prep_failed = true;
            prep_error = e.what();
        }

        if (prep_failed) {
            std::cerr << "  Failed to prepare project: " << prep_error << "\n";
            co_await submit_error_result(job, "", actual_kiln_hash);
            co_return LoopResult::work_done;
        }

        // Run build (blocking — runs in the calling thread)
        auto build_result = run_build(config_, job, project_dir);
        build_result.project_commit = project_commit;

        // Submit result
        co_await submit_result(job, build_result, actual_kiln_hash);

        std::cout << "  Result: " << build_result.status;
        if (build_result.cmake_fallback_status)
            std::cout << " (cmake: " << *build_result.cmake_fallback_status << ")";
        std::cout << " in " << build_result.duration_seconds << "s\n\n";

        co_return LoopResult::work_done;
    }

    // Returns true if the server was reachable (even if it returned an error code).
    Task<bool> send_heartbeat() {
        HeartbeatRequest hb{
            .arch = sysinfo_.arch,
            .os = sysinfo_.os,
            .os_version = sysinfo_.os_version,
            .distro = sysinfo_.distro,
            .cpu_model = sysinfo_.cpu_model,
            .cores = sysinfo_.cores,
            .ram_mb = sysinfo_.ram_mb,
            .compiler = compiler_,
            .compiler_version = compiler_version_,
        };

        std::string body;
        if (auto err = glz::write_json(hb, body); err) {
            std::cerr << "Failed to serialize heartbeat\n";
            co_return true; // serialization failure is not a network error
        }

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/heartbeat");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        auto resp = co_await send_request(req);
        if (!resp)
            co_return false; // network unreachable
        if (resp->statusCode() == k401Unauthorized) {
            std::cerr << "Heartbeat rejected (401): " << std::string(resp->body()) << "\n";
        } else if (resp->statusCode() != k200OK) {
            std::cerr << "Heartbeat rejected (" << resp->statusCode() << ")\n";
        }
        co_return true;
    }

    Task<PollOutcome> poll_for_job() {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/poll");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody("{}");
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        auto resp = co_await send_request(req);
        if (!resp)
            co_return PollOutcome{.network_ok = false};

        if (resp->statusCode() == k204NoContent)
            co_return PollOutcome{};

        if (resp->statusCode() == k401Unauthorized) {
            std::cerr << "Poll rejected (401): " << std::string(resp->body()) << "\n";
            co_return PollOutcome{};
        }
        if (resp->statusCode() != k200OK) {
            std::cerr << "Poll failed: " << resp->statusCode() << "\n";
            co_return PollOutcome{};
        }

        PollResponse job{};
        auto body = std::string(resp->body());
        if (auto err = glz::read_json(job, body); err) {
            std::cerr << "Failed to parse poll response: "
                      << glz::format_error(err, body) << "\n";
            co_return PollOutcome{};
        }

        co_return PollOutcome{.job = std::move(job)};
    }

    Task<> submit_result(const PollResponse &job, const BuildResult &br,
                         const std::string &kiln_hash) {
        ResultRequest rr{
            .job_id = job.job_id,
            .status = br.status,
            .duration_seconds = br.duration_seconds,
            .compiler = compiler_,
            .compiler_version = compiler_version_,
            .project_commit = br.project_commit,
            .kiln_git_hash = kiln_hash,
            .cmake_fallback_status = br.cmake_fallback_status,
            .cmake_duration_seconds = br.cmake_duration_seconds,
            .cmake_version = br.cmake_version,
            .log = br.log.empty() ? std::nullopt : std::optional{br.log},
            .cmake_log = br.cmake_log.empty() ? std::nullopt : std::optional{br.cmake_log},
        };

        std::string body;
        if (auto err = glz::write_json(rr, body); err) {
            std::cerr << "Failed to serialize result\n";
            co_return;
        }

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/result");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        auto resp = co_await send_request(req);
        if (!resp || resp->statusCode() != k200OK) {
            std::cerr << "Failed to submit result";
            if (resp) std::cerr << " (" << resp->statusCode() << ": "
                                << std::string(resp->body()) << ")";
            std::cerr << "\n";
        }
    }

    Task<> submit_error_result(const PollResponse &job, const std::string &commit,
                               const std::string &kiln_hash = {}) {
        ResultRequest rr{
            .job_id = job.job_id,
            .status = "error",
            .duration_seconds = 0,
            .compiler = compiler_,
            .compiler_version = compiler_version_,
            .project_commit = commit,
            .kiln_git_hash = kiln_hash,
        };

        std::string body;
        if (auto err = glz::write_json(rr, body); err) co_return;

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/result");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        co_await send_request(req);
    }

    Task<> sleep_with_jitter() {
        std::uniform_int_distribution<int> dist(0, config_.poll_interval_seconds / 3);
        double seconds = config_.poll_interval_seconds + dist(rng_);
        co_await sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), seconds);
    }

    Task<> jittered_sleep(int base_seconds) {
        std::uniform_int_distribution<int> dist(0, std::max(1, base_seconds / 4));
        double seconds = base_seconds + dist(rng_);
        co_await sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), seconds);
    }

    WorkerConfig config_;
    SystemInfo sysinfo_;
    std::string compiler_;
    std::string compiler_version_;
    HttpClientPtr client_;
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point last_heartbeat_{};
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: kiln-worker <config.yaml>\n";
        return 1;
    }

    WorkerConfig config;
    try {
        config = WorkerConfig::load(argv[1]);
    } catch (const std::exception &e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto &app = drogon::app();
    app.getLoop()->queueInLoop([config = std::move(config)]() mutable {
        Worker worker(std::move(config));
        [](Worker w) -> AsyncTask {
            co_await w.run();
            drogon::app().quit();
        }(std::move(worker));
    });

    app.run();
    return 0;
}
