#include "server/migrate.h"
#include "server/config.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <iostream>

int main(int argc, char *argv[]) {
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
    std::cout << "Kiln Compat Tracker starting on http://localhost:7621\n";
    app.run();
    return 0;
}
