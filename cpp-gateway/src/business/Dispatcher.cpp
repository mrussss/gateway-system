#include "business/Dispatcher.hpp"
#include "business/Handlers.hpp"
#include "business/StatsManager.hpp"
#include "protocol/MessageType.hpp"

namespace business
{
    Dispatcher::Dispatcher(const ControlPlaneClient &control_plane)
        : control_plane_(control_plane)
    {
    }

    Response Dispatcher::dispatch(const Request &request)
    {
        StatsManager::getInstance().incrementRequests();
        switch (request.type)
        {
        case MessageType::AUTH:
            return handleAuth(request, control_plane_);
            break;
        case MessageType::PING:
            return handlePing(request);
            break;
        case MessageType::ECHO:
            return handleEcho(request);
            break;
        case MessageType::LOG_PUSH:
            return handleLogPush(request);
            break;
        case MessageType::STATS:
            return handleStats(request);
            break;

        default:
            return makeErrorResponse(request);
            break;
        }
    }

}
