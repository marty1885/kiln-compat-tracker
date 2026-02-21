#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace kiln {

class AdminApi : public drogon::HttpController<AdminApi> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminApi::status, "/api/v1/auth/status", drogon::Get);
    ADD_METHOD_TO(AdminApi::registerAdmin, "/api/v1/auth/register", drogon::Post);
    ADD_METHOD_TO(AdminApi::login, "/api/v1/auth/login", drogon::Post);
    ADD_METHOD_TO(AdminApi::logout, "/api/v1/auth/logout", drogon::Post);
    ADD_METHOD_TO(AdminApi::invite, "/api/v1/auth/invite", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> registerAdmin(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> login(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> logout(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> invite(drogon::HttpRequestPtr req);
};

} // namespace kiln
