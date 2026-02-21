cmake_minimum_required(VERSION 3.20)
project(kiln-compat-tracker CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Drogon CONFIG REQUIRED)
find_package(glaze REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Botan REQUIRED)

# --- Server ---
add_executable(kiln-server
    server/main.cpp
    server/controllers/WorkerApi.cpp
    server/controllers/ProjectApi.cpp
    server/controllers/DashboardApi.cpp
    server/controllers/AdminApi.cpp
    server/controllers/WorkerMgmtApi.cpp
)

target_include_directories(kiln-server PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(kiln-server PRIVATE
    Drogon::Drogon
    glaze::glaze
    yaml-cpp::yaml-cpp
    Botan::Botan
)

# --- Worker ---
add_executable(kiln-worker
    worker/main.cpp
    worker/platform.cpp
    worker/builder.cpp
)

target_include_directories(kiln-worker PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(kiln-worker PRIVATE
    Drogon::Drogon
    glaze::glaze
    yaml-cpp::yaml-cpp
)
