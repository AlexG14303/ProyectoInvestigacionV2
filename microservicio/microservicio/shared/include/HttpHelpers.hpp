#pragma once
#include <string>
#include "httplib.h"
#include "json.hpp"

// ─── Respuestas HTTP estándar ────────────────────────────────────────────────

inline void jsonOk(httplib::Response& res, const nlohmann::json& data) {
    res.status = 200;
    res.set_content(data.dump(), "application/json");
}

inline void jsonCreated(httplib::Response& res, const nlohmann::json& data) {
    res.status = 201;
    res.set_content(data.dump(), "application/json");
}

inline void jsonError(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    nlohmann::json err = {{"error", message}};
    res.set_content(err.dump(), "application/json");
}

// ─── Parseo del cuerpo de la petición ───────────────────────────────────────

inline nlohmann::json parseBody(const httplib::Request& req) {
    return nlohmann::json::parse(req.body);
}
