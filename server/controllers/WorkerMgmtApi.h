#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace kiln {

class WorkerMgmtApi : public drogon::HttpController<WorkerMgmtApi> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(WorkerMgmtApi::list, "/api/v1/admin/workers", drogon::Get);
    ADD_METHOD_TO(WorkerMgmtApi::create, "/api/v1/admin/workers", drogon::Post);
    ADD_METHOD_TO(WorkerMgmtApi::remove, "/api/v1/admin/workers/{id}", drogon::Delete);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req, int64_t id);
};

} // namespace kiln
