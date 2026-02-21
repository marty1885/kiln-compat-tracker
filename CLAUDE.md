# Kiln Compat Tracker

Continuous compatibility testing for Kiln (a CMake-compatible build system). Coordinator server + distributed workers.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/kiln-compat-tracker-server  # runs on :7621, serve from repo root
./build/kiln-compat-tracker-worker worker/config.yaml  # connect a worker
```

Dependencies (system-installed): Drogon, Glaze, yaml-cpp.

## Database

PostgreSQL. User and DB are both `kiln-compat-tracker`, no password, localhost.

**Migrations are automatic.** The server applies pending migrations on startup via `server/migrate.h`. No manual `psql -f schema.sql` needed after initial DB creation.

For a fresh database, just create the DB and user — the server handles the rest:
```bash
sudo -u postgres createuser kiln-compat-tracker
sudo -u postgres createdb -O kiln-compat-tracker kiln-compat-tracker
```

### Schema change policy

- `sql/schema.sql` is the reference schema for documentation. Keep it in sync with migrations.
- All schema changes go through `server/migrate.h` as numbered migrations.
- Append new migrations to the `migrations()` vector — never modify existing ones.
- Each migration is a version number + list of SQL statements, applied in a transaction.
- The `schema_version` table tracks what's been applied.

## API

All endpoints are versioned under `/api/v1/`.

## Code Conventions

- C++20, targeting C++23 idioms. Handle errors gracefully — no `(void)` casts.
- Drogon controllers use **coroutines** (`Task<HttpResponsePtr>`, `co_await db->execSqlCoro(...)`). No callbacks.
- For SqlBinder with nullable params: `co_await drogon::orm::internal::SqlAwaiter(std::move(binder))`.
- All Glaze-serializable structs go in `common/protocol.h` at namespace scope (Glaze cannot reflect local types).
- `json_response<T>()` and `error_response()` helpers in `server/json_response.h`.
- Frontend: plain HTML/CSS/JS. Pull JS libs from CDN. No web build system.
- `find_package(Drogon CONFIG REQUIRED)` — the CONFIG keyword is required.


## Coding Guidelines

- State assumptions before implementing
- Produce code you wouldn't want to debug at 3am
- Don't handle only the happy path
- Don't solve problems you weren't asked to solve
- Don't import complexity you don't need

