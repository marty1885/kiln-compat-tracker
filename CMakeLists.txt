cmake_minimum_required(VERSION 3.20)
project(kiln-compat-tracker CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SERVER "Build tracker server" ON)

find_package(Drogon CONFIG REQUIRED)
find_package(glaze REQUIRED)
find_package(yaml-cpp REQUIRED)

if(BUILD_SERVER)
    find_package(Botan REQUIRED)
    # --- Server ---
    add_executable(kiln-compat-tracker-server
        server/main.cpp
        server/controllers/WorkerApi.cpp
        server/controllers/ProjectApi.cpp
        server/controllers/DashboardApi.cpp
        server/controllers/AdminApi.cpp
        server/controllers/WorkerMgmtApi.cpp
    )

    target_include_directories(kiln-compat-tracker-server PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(kiln-compat-tracker-server PRIVATE
        Drogon::Drogon
        glaze::glaze
        yaml-cpp::yaml-cpp
        Botan::Botan
    )
endif()

# --- Worker ---
add_executable(kiln-compat-tracker-worker
    worker/main.cpp
    worker/platform.cpp
    worker/builder.cpp
)

target_include_directories(kiln-compat-tracker-worker PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(kiln-compat-tracker-worker PRIVATE
    Drogon::Drogon
    glaze::glaze
    yaml-cpp::yaml-cpp
)
