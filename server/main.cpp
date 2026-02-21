#include "server/migrate.h"
#include <drogon/drogon.h>
#include <iostream>

int main() {
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
    app.addListener("0.0.0.0", 8080);
    std::cout << "Kiln Compat Tracker starting on http://localhost:8080\n";
    app.run();
    return 0;
}
