# Kiln Compat Worker

A distributed build worker for the Kiln Compat Tracker. Workers poll the coordinator server for build jobs, run them, and report results back.

## Runtime Dependencies

The worker process itself needs:

- **git** — for cloning/fetching the kiln source and project repos
- **cmake** and **ninja** — for building kiln itself and running the cmake fallback
- A **C++ compiler** (GCC or Clang) — detected automatically at startup
- SSH access to `git@github.com:marty1885/kiln.git` — the worker clones kiln over SSH, so the user running the worker needs a GitHub SSH key configured

## Configuration

The worker is configured with a YAML file. By default it looks for `worker/config.yaml` relative to the working directory, but you can pass any path as the first argument:

```
kiln-worker /etc/kiln-worker/config.yaml
```

### Config file reference

```yaml
# Required

# Base URL of the coordinator server (no trailing slash)
server_url: http://my-server:7621

# Bearer token issued by the server when the worker was registered (Admin → Workers → Add Worker)
auth_token: <token from server>

# Display name for this worker, must match what was registered on the server
worker_name: my-builder-01


# Optional

# Maximum resource tier this worker can handle.
# Projects are scheduled only to workers that meet or exceed their tier requirement.
# Valid values: small, medium, large  (default: small)
resource_tier_max: small

# Maximum dependency level this worker can handle.
# Projects are scheduled only to workers that meet or exceed their dep level.
# Valid values: base, moderate, full  (default: base)
dep_level_max: base

# Root directory for the worker's persistent cache.
# Kiln source is kept at {workspace_dir}/kiln/
# Project repos are kept at {workspace_dir}/projects/
# Should survive reboots — avoid /tmp for production use.
# default: /tmp/kiln-ci
workspace_dir: /var/lib/kiln-worker/workspace

# How long to wait (in seconds) between poll cycles when there is no work.
# Also controls the heartbeat rate — heartbeats are sent at most once per interval.
# default: 30
poll_interval_seconds: 30
```

### Getting an auth token

1. Log in to the tracker dashboard
2. Go to **Admin -> Workers -> Add Worker**
3. Enter the worker name and select a resource tier
4. Copy the token shown — it is only displayed once

The token goes in the `auth_token` field of the config file.

## Running manually

A config file path is required — the worker has no default and will refuse to start without one.

```bash
# Build the project first (from repo root):
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

./build/kiln-worker /etc/kiln-worker/config.yaml
```

## Running as a systemd service

See [`kiln-worker.service`](kiln-worker.service) in this directory for a ready-to-use unit file.

### Quick setup

```bash
# 1. Create a dedicated user and workspace
sudo useradd -r -m -d /var/lib/kiln-worker -s /sbin/nologin kiln-worker
sudo mkdir -p /var/lib/kiln-worker/workspace
sudo chown kiln-worker:kiln-worker /var/lib/kiln-worker/workspace

# 2. Set up an SSH key for the kiln-worker user so it can clone kiln over SSH
sudo -u kiln-worker ssh-keygen -t ed25519 -C "kiln-worker@$(hostname)" -f /var/lib/kiln-worker/.ssh/id_ed25519 -N ""
# Add the public key to your GitHub account (or the kiln repo's deploy keys):
sudo cat /var/lib/kiln-worker/.ssh/id_ed25519.pub

# 3. Install the binary
sudo install -m 755 build/kiln-worker /usr/local/bin/kiln-worker

# 4. Create the config file
sudo mkdir -p /etc/kiln-worker
sudo cp worker/config.yaml /etc/kiln-worker/config.yaml
sudo $EDITOR /etc/kiln-worker/config.yaml   # fill in server_url, auth_token, worker_name
sudo chown root:kiln-worker /etc/kiln-worker/config.yaml
sudo chmod 640 /etc/kiln-worker/config.yaml  # keep token readable only by the service user

# 5. Install and start the service
sudo cp worker/kiln-worker.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now kiln-worker

# Check status / logs
sudo systemctl status kiln-worker
sudo journalctl -u kiln-worker -f
```

## Workspace layout

After the first run, the workspace will contain:

```
{workspace_dir}/
  kiln/               # kiln source + built binary (kiln/build/kiln)
  projects/
    <project-name>/   # one git clone per tracked project
    ...
```

The worker keeps these clones around between jobs and does `git fetch` rather than re-cloning each time, so the workspace directory should be on persistent storage.
