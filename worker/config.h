#pragma once

#include <string>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <stdexcept>

namespace kiln {

struct WorkerConfig {
    std::string server_url;
    std::string auth_token;
    std::string workspace_dir;
    int poll_interval_seconds = 30;

    // Derived paths
    std::string kiln_source_dir() const { return (std::filesystem::path(workspace_dir) / "kiln").string(); }
    std::string project_cache_dir() const { return (std::filesystem::path(workspace_dir) / "projects").string(); }

    static WorkerConfig load(const std::string &path) {
        if (!std::filesystem::exists(path))
            throw std::runtime_error("Config file not found: " + path);

        auto yaml = YAML::LoadFile(path);
        WorkerConfig c;
        c.server_url = yaml["server_url"].as<std::string>();
        c.auth_token = yaml["auth_token"].as<std::string>();

        if (yaml["workspace_dir"])
            c.workspace_dir = yaml["workspace_dir"].as<std::string>();
        else
            c.workspace_dir = "/tmp/kiln-ci";
        if (yaml["poll_interval_seconds"])
            c.poll_interval_seconds = yaml["poll_interval_seconds"].as<int>();

        return c;
    }
};

} // namespace kiln
