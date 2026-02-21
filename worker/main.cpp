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
        , compiler_version_(detect_compiler_version()) {

        client_ = HttpClient::newHttpClient(config_.server_url);
    }

    Task<> run() {
        std::cout << "Worker '" << config_.worker_name << "' starting\n";
        std::cout << "  " << sysinfo_.arch << "/" << sysinfo_.os
                  << " " << sysinfo_.os_version << "\n";
        std::cout << "  " << sysinfo_.cpu_model << " (" << sysinfo_.cores
                  << " cores, " << sysinfo_.ram_mb << " MB RAM)\n";
        std::cout << "  Compiler: " << compiler_ << " " << compiler_version_ << "\n";
        std::cout << "  Server: " << config_.server_url << "\n\n";

        while (g_running) {
            bool should_sleep = co_await loop_iteration();
            if (should_sleep)
                co_await sleep_with_jitter();
            else
                co_await sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), 5.0);
        }
    }

private:
    // Returns true if we should do a long sleep (no work), false for short pause
    Task<bool> loop_iteration() {
        co_await send_heartbeat();

        auto job = co_await poll_for_job();
        if (!job)
            co_return true;

        std::cout << "Got job #" << job->job_id << ": " << job->project_name << "\n";

        // Prepare project
        std::string project_dir;
        std::string project_commit;
        bool prep_failed = false;
        std::string prep_error;

        try {
            ensure_kiln(config_, job->kiln_git_hash);
            project_commit = prepare_project(config_, *job);
            project_dir = (std::filesystem::path(config_.project_cache_dir()) /
                           job->project_name).string();
        } catch (const std::exception &e) {
            prep_failed = true;
            prep_error = e.what();
        }

        if (prep_failed) {
            std::cerr << "  Failed to prepare project: " << prep_error << "\n";
            co_await submit_error_result(*job, "");
            co_return false;
        }

        // Run build (blocking — runs in the calling thread)
        auto build_result = run_build(config_, *job, project_dir);
        build_result.project_commit = project_commit;

        // Submit result
        co_await submit_result(*job, build_result);

        std::cout << "  Result: " << build_result.status;
        if (build_result.cmake_fallback_status)
            std::cout << " (cmake: " << *build_result.cmake_fallback_status << ")";
        std::cout << " in " << build_result.duration_seconds << "s\n\n";

        co_return false;
    }

    Task<> send_heartbeat() {
        HeartbeatRequest hb{
            .name = config_.worker_name,
            .arch = sysinfo_.arch,
            .os = sysinfo_.os,
            .os_version = sysinfo_.os_version,
            .cpu_model = sysinfo_.cpu_model,
            .cores = sysinfo_.cores,
            .ram_mb = sysinfo_.ram_mb,
            .resource_tier_max = config_.resource_tier_max,
        };

        std::string body;
        if (auto err = glz::write_json(hb, body); err) {
            std::cerr << "Failed to serialize heartbeat\n";
            co_return;
        }

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/heartbeat");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        auto resp = co_await client_->sendRequestCoro(req);
        if (!resp || resp->statusCode() != k200OK) {
            std::cerr << "Heartbeat failed";
            if (resp) std::cerr << " (" << resp->statusCode() << ")";
            std::cerr << "\n";
        }
    }

    Task<std::optional<PollResponse>> poll_for_job() {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/poll");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody("{}");
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        auto resp = co_await client_->sendRequestCoro(req);
        if (!resp)
            co_return std::nullopt;

        if (resp->statusCode() == k204NoContent)
            co_return std::nullopt;

        if (resp->statusCode() != k200OK) {
            std::cerr << "Poll failed: " << resp->statusCode() << "\n";
            co_return std::nullopt;
        }

        PollResponse job{};
        auto body = std::string(resp->body());
        if (auto err = glz::read_json(job, body); err) {
            std::cerr << "Failed to parse poll response: "
                      << glz::format_error(err, body) << "\n";
            co_return std::nullopt;
        }

        co_return job;
    }

    Task<> submit_result(const PollResponse &job, const BuildResult &br) {
        ResultRequest rr{
            .job_id = job.job_id,
            .status = br.status,
            .duration_seconds = br.duration_seconds,
            .compiler = compiler_,
            .compiler_version = compiler_version_,
            .project_commit = br.project_commit,
            .cmake_fallback_status = br.cmake_fallback_status,
            .cmake_duration_seconds = br.cmake_duration_seconds,
            .cmake_version = br.cmake_version,
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

        auto resp = co_await client_->sendRequestCoro(req);
        if (!resp || resp->statusCode() != k200OK) {
            std::cerr << "Failed to submit result";
            if (resp) std::cerr << " (" << resp->statusCode() << ": "
                                << std::string(resp->body()) << ")";
            std::cerr << "\n";
        }
    }

    Task<> submit_error_result(const PollResponse &job, const std::string &commit) {
        ResultRequest rr{
            .job_id = job.job_id,
            .status = "error",
            .duration_seconds = 0,
            .compiler = compiler_,
            .compiler_version = compiler_version_,
            .project_commit = commit,
        };

        std::string body;
        if (auto err = glz::write_json(rr, body); err) co_return;

        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath("/api/v1/worker/result");
        req->setContentTypeCode(CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("Authorization", "Bearer " + config_.auth_token);

        co_await client_->sendRequestCoro(req);
    }

    Task<> sleep_with_jitter() {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> jitter(0, config_.poll_interval_seconds / 3);
        double seconds = config_.poll_interval_seconds + jitter(rng);
        co_await sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), seconds);
    }

    WorkerConfig config_;
    SystemInfo sysinfo_;
    std::string compiler_;
    std::string compiler_version_;
    HttpClientPtr client_;
};

int main(int argc, char *argv[]) {
    std::string config_path = "worker/config.yaml";
    if (argc > 1)
        config_path = argv[1];

    WorkerConfig config;
    try {
        config = WorkerConfig::load(config_path);
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
