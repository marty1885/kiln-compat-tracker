-- Kiln Compat Tracker schema
-- This file is the canonical schema definition.
-- The server auto-applies migrations on startup via server/migrate.h

CREATE TABLE IF NOT EXISTS schema_version (
    version INT PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Migration 1: initial schema (ids widened to BIGINT in migration 2)
CREATE TYPE resource_tier AS ENUM ('small', 'medium', 'large');
CREATE TYPE build_status AS ENUM ('pass', 'fail', 'timeout', 'error');

CREATE TABLE kiln_commits (
    id BIGSERIAL PRIMARY KEY,
    git_hash TEXT UNIQUE NOT NULL,
    git_timestamp TIMESTAMPTZ,
    polled_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE projects (
    id BIGSERIAL PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    repo_url TEXT NOT NULL,
    branch TEXT NOT NULL DEFAULT 'HEAD',
    pinned_commit TEXT,
    build_command TEXT,
    run_tests BOOLEAN NOT NULL DEFAULT false,
    test_resource_tier_min resource_tier,
    resource_tier resource_tier NOT NULL DEFAULT 'small',
    cooldown_minutes INT NOT NULL DEFAULT 30,
    enabled BOOLEAN NOT NULL DEFAULT true,
    -- Migration 5: set by trigger, suppresses cooldown for builds before this time
    forced_rebuild_after TIMESTAMPTZ
);

CREATE TABLE workers (
    id BIGSERIAL PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    auth_token TEXT UNIQUE NOT NULL,
    arch TEXT NOT NULL DEFAULT '',
    os TEXT NOT NULL DEFAULT '',
    os_version TEXT NOT NULL DEFAULT '',
    cpu_model TEXT NOT NULL DEFAULT '',
    cores INT NOT NULL DEFAULT 0,
    ram_mb INT NOT NULL DEFAULT 0,
    resource_tier_max resource_tier NOT NULL DEFAULT 'small',
    last_seen TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Migration 4: distro and compiler for per-platform scheduling
    distro TEXT NOT NULL DEFAULT '',
    compiler TEXT NOT NULL DEFAULT '',
    compiler_version TEXT NOT NULL DEFAULT ''
);

-- Migration 3: worker_id nullable, ON DELETE SET NULL
CREATE TABLE build_results (
    id BIGSERIAL PRIMARY KEY,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    kiln_commit_id BIGINT REFERENCES kiln_commits(id),
    project_commit TEXT NOT NULL DEFAULT '',
    worker_id BIGINT REFERENCES workers(id) ON DELETE SET NULL,
    compiler TEXT NOT NULL DEFAULT '',
    compiler_version TEXT NOT NULL DEFAULT '',
    status build_status NOT NULL,
    test_status build_status,
    test_duration_seconds INT,
    cmake_fallback_status build_status,
    cmake_version TEXT,
    duration_seconds INT NOT NULL DEFAULT 0,
    cmake_duration_seconds INT,
    log_path TEXT,
    cmake_log_path TEXT,
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Migration 3: worker FK ON DELETE CASCADE
CREATE TABLE active_jobs (
    id BIGSERIAL PRIMARY KEY,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    kiln_commit_id BIGINT REFERENCES kiln_commits(id),
    worker_id BIGINT NOT NULL REFERENCES workers(id) ON DELETE CASCADE,
    assigned_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    heartbeat_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE config (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT INTO config (key, value) VALUES ('current_kiln_hash', '');

-- Migration 3: admin authentication
CREATE TABLE admins (
    id BIGSERIAL PRIMARY KEY,
    username TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE invite_tokens (
    id BIGSERIAL PRIMARY KEY,
    token TEXT UNIQUE NOT NULL,
    created_by BIGINT NOT NULL REFERENCES admins(id),
    used_by BIGINT REFERENCES admins(id),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    used_at TIMESTAMPTZ
);

CREATE INDEX idx_build_results_project ON build_results(project_id, finished_at DESC);
CREATE INDEX idx_build_results_status ON build_results(project_id, status);
CREATE INDEX idx_active_jobs_project ON active_jobs(project_id);
CREATE INDEX idx_active_jobs_heartbeat ON active_jobs(heartbeat_at);
CREATE INDEX idx_workers_token ON workers(auth_token);
