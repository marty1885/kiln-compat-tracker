#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace kiln {

class ProjectApi : public drogon::HttpController<ProjectApi> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ProjectApi::list, "/api/v1/projects", drogon::Get);
    ADD_METHOD_TO(ProjectApi::create, "/api/v1/projects", drogon::Post);
    ADD_METHOD_TO(ProjectApi::update, "/api/v1/projects/{id}", drogon::Put);
    ADD_METHOD_TO(ProjectApi::remove, "/api/v1/projects/{id}", drogon::Delete);
    ADD_METHOD_TO(ProjectApi::toggleEnable, "/api/v1/projects/{id}/enable", drogon::Patch);
    ADD_METHOD_TO(ProjectApi::trigger, "/api/v1/projects/{id}/trigger", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req, int64_t id);
    drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req, int64_t id);
    drogon::Task<drogon::HttpResponsePtr> toggleEnable(drogon::HttpRequestPtr req, int64_t id);
    drogon::Task<drogon::HttpResponsePtr> trigger(drogon::HttpRequestPtr req, int64_t id);
};

} // namespace kiln
