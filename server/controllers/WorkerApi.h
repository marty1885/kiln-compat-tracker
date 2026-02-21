#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace kiln {

class WorkerApi : public drogon::HttpController<WorkerApi> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(WorkerApi::heartbeat, "/api/v1/worker/heartbeat", drogon::Post);
    ADD_METHOD_TO(WorkerApi::poll, "/api/v1/worker/poll", drogon::Post);
    ADD_METHOD_TO(WorkerApi::jobHeartbeat, "/api/v1/worker/job-heartbeat", drogon::Patch);
    ADD_METHOD_TO(WorkerApi::result, "/api/v1/worker/result", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> heartbeat(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> poll(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> jobHeartbeat(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> result(drogon::HttpRequestPtr req);
};

} // namespace kiln
