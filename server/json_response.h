#pragma once

#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>
#include <string>

namespace kiln {

template <typename T>
drogon::HttpResponsePtr json_response(T &&value,
                                      drogon::HttpStatusCode code = drogon::k200OK) {
    std::string json;
    if (auto err = glz::write_json(std::forward<T>(value), json); err) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setBody("JSON serialization error");
        return resp;
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(std::move(json));
    return resp;
}

inline drogon::HttpResponsePtr error_response(drogon::HttpStatusCode code,
                                               std::string_view msg = "") {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    if (!msg.empty()) resp->setBody(std::string(msg));
    return resp;
}

} // namespace kiln
