#pragma once
#include <string>
#include "control/ControlPlaneClient.hpp"
#include "protocol/Request.hpp"
#include "protocol/Response.hpp"

namespace business
{
    Response handleAuth(const Request &request, const ControlPlaneClient &control_plane);
    Response handlePing(const Request &request);
    Response handleEcho(const Request &request);
    Response handleLogPush(const Request &request);
    Response handleStats(const Request &request);
    Response makeErrorResponse(const Request &request);
    Response makeErrorResponse(const Request &request, int status, const std::string &message);
}
