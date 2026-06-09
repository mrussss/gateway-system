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