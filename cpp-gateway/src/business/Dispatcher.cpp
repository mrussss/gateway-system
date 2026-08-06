#include "business/Dispatcher.hpp"
#include "business/Handlers.hpp"
#include "business/StatsManager.hpp"
#include "protocol/MessageType.hpp"

namespace business
{
    Response Dispatcher::dispatch(const Request &request)
    {
        StatsManager::getInstance().incrementRequests();
        switch (request.type)
        {
        case MessageType::AUTH:
            StatsManager::getInstance().incrementErrors();
            return makeErrorResponse(request, 500, "AUTH reached normal worker");
        case MessageType::PING:
            return handlePing(request);
        case MessageType::ECHO:
            return handleEcho(request);
        case MessageType::LOG_PUSH:
            return handleLogPush(request);
        case MessageType::STATS:
            return handleStats(request);

        default:
            return makeErrorResponse(request);
        }
    }

}
