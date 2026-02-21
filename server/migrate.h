#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <iostream>
#include <string>
#include <vector>

namespace kiln {

// Each migration is a version number and a list of SQL statements.
// Add new migrations at the end — never modify existing ones.
struct Migration {
    int version;
    std::vector<std::string> statements;
};

// All migrations in order. The server applies any that haven't been run yet.
// To add a new migration: append to this list with the next version number.
inline const std::vector<Migration> &migrations() {
    static const std::vector<Migration> m = {
        {1, {
            "DO $$ BEGIN CREATE TYPE resource_tier AS ENUM ('small', 'medium', 'large'); EXCEPTION WHEN duplicate_object THEN NULL; END $$",
            "DO $$ BEGIN CREATE TYPE build_status AS ENUM ('pass', 'fail', 'timeout', 'error'); EXCEPTION WHEN duplicate_object THEN NULL; END $$",

            "CREATE TABLE IF NOT EXISTS kiln_commits ("
            "  id SERIAL PRIMARY KEY,"
            "  git_hash TEXT UNIQUE NOT NULL,"
            "  git_timestamp TIMESTAMPTZ,"
            "  polled_at TIMESTAMPTZ NOT NULL DEFAULT now())",

            "CREATE TABLE IF NOT EXISTS projects ("
            "  id SERIAL PRIMARY KEY,"
            "  name TEXT UNIQUE NOT NULL,"
            "  repo_url TEXT NOT NULL,"
            "  branch TEXT NOT NULL DEFAULT 'HEAD',"
            "  pinned_commit TEXT,"
            "  build_command TEXT,"
            "  run_tests BOOLEAN NOT NULL DEFAULT false,"
            "  test_resource_tier_min resource_tier,"
            "  resource_tier resource_tier NOT NULL DEFAULT 'small',"
            "  cooldown_minutes INT NOT NULL DEFAULT 30,"
            "  enabled BOOLEAN NOT NULL DEFAULT true)",

            "CREATE TABLE IF NOT EXISTS workers ("
            "  id SERIAL PRIMARY KEY,"
            "  name TEXT UNIQUE NOT NULL,"
            "  auth_token TEXT UNIQUE NOT NULL,"
            "  arch TEXT NOT NULL DEFAULT '',"
            "  os TEXT NOT NULL DEFAULT '',"
            "  os_version TEXT NOT NULL DEFAULT '',"
            "  cpu_model TEXT NOT NULL DEFAULT '',"
            "  cores INT NOT NULL DEFAULT 0,"
            "  ram_mb INT NOT NULL DEFAULT 0,"
            "  resource_tier_max resource_tier NOT NULL DEFAULT 'small',"
            "  last_seen TIMESTAMPTZ NOT NULL DEFAULT now())",

            "CREATE TABLE IF NOT EXISTS build_results ("
            "  id SERIAL PRIMARY KEY,"
            "  project_id INT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
            "  kiln_commit_id INT REFERENCES kiln_commits(id),"
            "  project_commit TEXT NOT NULL DEFAULT '',"
            "  worker_id INT NOT NULL REFERENCES workers(id),"
            "  compiler TEXT NOT NULL DEFAULT '',"
            "  compiler_version TEXT NOT NULL DEFAULT '',"
            "  status build_status NOT NULL,"
            "  test_status build_status,"
            "  test_duration_seconds INT,"
            "  cmake_fallback_status build_status,"
            "  cmake_version TEXT,"
            "  duration_seconds INT NOT NULL DEFAULT 0,"
            "  cmake_duration_seconds INT,"
            "  log_path TEXT,"
            "  cmake_log_path TEXT,"
            "  started_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
            "  finished_at TIMESTAMPTZ NOT NULL DEFAULT now())",

            "CREATE TABLE IF NOT EXISTS active_jobs ("
            "  id SERIAL PRIMARY KEY,"
            "  project_id INT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
            "  kiln_commit_id INT REFERENCES kiln_commits(id),"
            "  worker_id INT NOT NULL REFERENCES workers(id),"
            "  assigned_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
            "  heartbeat_at TIMESTAMPTZ NOT NULL DEFAULT now())",

            "CREATE TABLE IF NOT EXISTS config ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL)",

            "INSERT INTO config (key, value) VALUES ('current_kiln_hash', '') ON CONFLICT DO NOTHING",

            "CREATE INDEX IF NOT EXISTS idx_build_results_project ON build_results(project_id, finished_at DESC)",
            "CREATE INDEX IF NOT EXISTS idx_build_results_status ON build_results(project_id, status)",
            "CREATE INDEX IF NOT EXISTS idx_active_jobs_project ON active_jobs(project_id)",
            "CREATE INDEX IF NOT EXISTS idx_active_jobs_heartbeat ON active_jobs(heartbeat_at)",
            "CREATE INDEX IF NOT EXISTS idx_workers_token ON workers(auth_token)",
        }},
        {2, {
            // Widen all ids from SERIAL (int4) to BIGSERIAL (int8) so they
            // match the int64_t used in C++ / Drogon binary protocol.
            "ALTER TABLE kiln_commits ALTER COLUMN id SET DATA TYPE BIGINT",
            "ALTER TABLE projects ALTER COLUMN id SET DATA TYPE BIGINT",
            "ALTER TABLE workers ALTER COLUMN id SET DATA TYPE BIGINT",
            "ALTER TABLE build_results ALTER COLUMN id SET DATA TYPE BIGINT",
            "ALTER TABLE build_results ALTER COLUMN project_id SET DATA TYPE BIGINT",
            "ALTER TABLE build_results ALTER COLUMN kiln_commit_id SET DATA TYPE BIGINT",
            "ALTER TABLE build_results ALTER COLUMN worker_id SET DATA TYPE BIGINT",
            "ALTER TABLE active_jobs ALTER COLUMN id SET DATA TYPE BIGINT",
            "ALTER TABLE active_jobs ALTER COLUMN project_id SET DATA TYPE BIGINT",
            "ALTER TABLE active_jobs ALTER COLUMN kiln_commit_id SET DATA TYPE BIGINT",
            "ALTER TABLE active_jobs ALTER COLUMN worker_id SET DATA TYPE BIGINT",
        }},
        {3, {
            // Admin authentication
            "CREATE TABLE IF NOT EXISTS admins ("
            "  id BIGSERIAL PRIMARY KEY,"
            "  username TEXT UNIQUE NOT NULL,"
            "  password_hash TEXT NOT NULL,"
            "  created_at TIMESTAMPTZ NOT NULL DEFAULT now())",

            // Invite-link registration
            "CREATE TABLE IF NOT EXISTS invite_tokens ("
            "  id BIGSERIAL PRIMARY KEY,"
            "  token TEXT UNIQUE NOT NULL,"
            "  created_by BIGINT NOT NULL REFERENCES admins(id),"
            "  used_by BIGINT REFERENCES admins(id),"
            "  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
            "  used_at TIMESTAMPTZ)",

            // Allow worker deletion without losing build history
            "ALTER TABLE build_results DROP CONSTRAINT IF EXISTS build_results_worker_id_fkey",
            "ALTER TABLE build_results ALTER COLUMN worker_id DROP NOT NULL",
            "DO $$ BEGIN "
            "ALTER TABLE build_results ADD CONSTRAINT build_results_worker_id_fkey "
            "  FOREIGN KEY (worker_id) REFERENCES workers(id) ON DELETE SET NULL; "
            "EXCEPTION WHEN duplicate_object THEN NULL; END $$",

            // Cascade active jobs when worker is deleted
            "ALTER TABLE active_jobs DROP CONSTRAINT IF EXISTS active_jobs_worker_id_fkey",
            "DO $$ BEGIN "
            "ALTER TABLE active_jobs ADD CONSTRAINT active_jobs_worker_id_fkey "
            "  FOREIGN KEY (worker_id) REFERENCES workers(id) ON DELETE CASCADE; "
            "EXCEPTION WHEN duplicate_object THEN NULL; END $$",
        }},
        {4, {
            // Distro tracking and per-platform scheduling
            "ALTER TABLE workers ADD COLUMN IF NOT EXISTS distro TEXT NOT NULL DEFAULT ''",
            "ALTER TABLE workers ADD COLUMN IF NOT EXISTS compiler TEXT NOT NULL DEFAULT ''",
            "ALTER TABLE workers ADD COLUMN IF NOT EXISTS compiler_version TEXT NOT NULL DEFAULT ''",
        }},
        {5, {
            // Replace destructive trigger (backdating finished_at) with a
            // non-destructive forced_rebuild_after timestamp. The scheduler
            // only counts a recent build as suppressing a new one if it also
            // occurred after this timestamp.
            "ALTER TABLE projects ADD COLUMN IF NOT EXISTS forced_rebuild_after TIMESTAMPTZ",
        }},
        {6, {
            // Allow the job reaper to mark stale jobs without deleting them,
            // so a worker that comes back online can still submit its result.
            "ALTER TABLE active_jobs ADD COLUMN IF NOT EXISTS reaped_at TIMESTAMPTZ",
        }},
        {7, {
            // Dependency level enum for matching projects to workers based on
            // what system dependencies are installed.
            "DO $$ BEGIN CREATE TYPE dep_level AS ENUM ('base', 'moderate', 'full'); EXCEPTION WHEN duplicate_object THEN NULL; END $$",
            "ALTER TABLE projects ADD COLUMN IF NOT EXISTS dep_level dep_level NOT NULL DEFAULT 'base'",
            "ALTER TABLE workers ADD COLUMN IF NOT EXISTS dep_level_max dep_level NOT NULL DEFAULT 'base'",
        }},
        {8, {
            // OS filter: restrict which OSes a project can build on.
            // Empty = any OS (default). Non-empty = comma-separated list.
            "ALTER TABLE projects ADD COLUMN IF NOT EXISTS os_filter TEXT NOT NULL DEFAULT ''",
        }},
        // Future migrations go here:
    };
    return m;
}

// Run all pending migrations synchronously at startup.
// Uses a blocking DB client since Drogon's event loop isn't running yet.
// Acquires an advisory lock to prevent races between concurrent instances.
inline void run_migrations(const drogon::orm::DbClientPtr &db) {
    // Ensure schema_version table exists
    db->execSqlSync(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version INT PRIMARY KEY,"
        "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");

    // Acquire advisory lock (blocks until available, released at session end)
    db->execSqlSync("SELECT pg_advisory_lock(7621)");

    // Get current version (re-check under lock)
    auto vr = db->execSqlSync("SELECT COALESCE(MAX(version), 0) AS v FROM schema_version");
    int current = vr[0]["v"].as<int>();

    for (const auto &m : migrations()) {
        if (m.version <= current)
            continue;

        std::cout << "Applying migration " << m.version << "...\n";
        auto txn = db->newTransaction();
        for (const auto &sql : m.statements) {
            try {
                txn->execSqlSync(sql);
            } catch (const drogon::orm::DrogonDbException &e) {
                std::cerr << "  Migration " << m.version << " failed: "
                          << e.base().what() << "\n"
                          << "  SQL: " << sql.substr(0, 120) << "\n";
                db->execSqlSync("SELECT pg_advisory_unlock(7621)");
                throw;
            }
        }
        txn->execSqlSync(
            "INSERT INTO schema_version (version) VALUES ($1)", m.version);
        std::cout << "  Migration " << m.version << " applied.\n";
    }

    db->execSqlSync("SELECT pg_advisory_unlock(7621)");
}

} // namespace kiln
