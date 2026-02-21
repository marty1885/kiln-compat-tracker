#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace kiln {

class DashboardApi : public drogon::HttpController<DashboardApi> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DashboardApi::matrix, "/api/v1/dashboard/matrix", drogon::Get);
    ADD_METHOD_TO(DashboardApi::workers, "/api/v1/dashboard/workers", drogon::Get);
    ADD_METHOD_TO(DashboardApi::projectHistory, "/api/v1/dashboard/project/{id}/history", drogon::Get);
    ADD_METHOD_TO(DashboardApi::buildLog, "/api/v1/dashboard/build/{id}/log", drogon::Get);
    ADD_METHOD_TO(DashboardApi::trend, "/api/v1/dashboard/trend", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> matrix(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> workers(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> projectHistory(drogon::HttpRequestPtr req, int64_t id);
    drogon::Task<drogon::HttpResponsePtr> buildLog(drogon::HttpRequestPtr req, int64_t id);
    drogon::Task<drogon::HttpResponsePtr> trend(drogon::HttpRequestPtr req);
};

} // namespace kiln
