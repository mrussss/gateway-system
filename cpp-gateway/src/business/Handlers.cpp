#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "business/Handlers.hpp"
#include "protocol/MessageType.hpp"
#include "business/StatsManager.hpp"
#include "business/LogStorage.hpp"
#include "net/TcpServer.hpp"
#include "nlohmann/json.hpp"

namespace business
{

    Response handleAuth(const Request &request, const ControlPlaneClient &control_plane)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::AUTH_RESP;

        try
        {
            auto payload = nlohmann::json::parse(request.payload);
            if (!payload.is_object() ||
                !payload.contains("client_id") ||
                !payload.contains("token") ||
                !payload["client_id"].is_string() ||
                !payload["token"].is_string())
            {
                StatsManager::getInstance().incrementErrors();
                resp.close_connection = true;
                resp.skip_write = true;
                return resp;
            }

            std::string client_id = payload["client_id"];
            std::string token = payload["token"];
            if (!control_plane.checkAuth(client_id, token))
            {
                StatsManager::getInstance().incrementErrors();
                resp.close_connection = true;
                resp.skip_write = true;
                return resp;
            }

            resp.status_code = 0;
            resp.payload = R"({"allowed":true,"reason":"ok"})";
            resp.mark_authenticated = true;
            resp.authenticated_client_id = client_id;
            return resp;
        }
        catch (const std::exception &)
        {
            StatsManager::getInstance().incrementErrors();
            resp.close_connection = true;
            resp.skip_write = true;
            return resp;
        }
    }

    Response handlePing(const Request &request)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::PONG;
        resp.payload = R"({"status":0,"message":"pong"})";
        return resp;
    }
    Response handleEcho(const Request &request)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::ECHO_RESP;
        resp.payload = request.payload;
        return resp;
    }

    Response handleLogPush(const Request &request)
    {
        if (request.payload.empty())
        {
            StatsManager::getInstance().incrementErrors();
            Response resp;
            resp.fd = request.fd;
            resp.conn_id = request.conn_id;
            resp.version = request.version;
            resp.request_id = request.request_id;
            resp.type = MessageType::ERROR_RESP;
            resp.payload = R"({"status":400,"message":"payload is empty"})";
            return resp;
        }
        if (request.payload.size() > 4096)
        {
            StatsManager::getInstance().incrementErrors();
            Response resp;
            resp.fd = request.fd;
            resp.conn_id = request.conn_id;
            resp.version = request.version;
            resp.request_id = request.request_id;
            resp.type = MessageType::ERROR_RESP;
            resp.payload = R"({"status":400,"message":"payload too large"})";
            return resp;
        }
        try
        {
            auto j = nlohmann::json::parse(request.payload);

            if (!j.is_object() ||
                !j.contains("level") ||
                !j.contains("service") ||
                !j.contains("message") ||
                !j["level"].is_string() ||
                !j["service"].is_string() ||
                !j["message"].is_string())
            {
                StatsManager::getInstance().incrementErrors();
                return makeErrorResponse(request, 400, "invalid log format");
            }
        }
        catch (const nlohmann::json::parse_error &)
        {
            StatsManager::getInstance().incrementErrors();
            return makeErrorResponse(request, 400, "invalid json");
        }

        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;

        std::time_t now = std::time(nullptr);
        struct tm time_info;
        localtime_r(&now, &time_info);
        std::ostringstream oss;
        oss << std::put_time(&time_info, "[%Y-%m-%d %H:%M:%S]")
            << " fd=" << request.fd
            << " request_id=" << request.request_id
            << " payload=" << request.payload;

        bool is_written = LogStorage::getInstance().append(oss.str());
        if (is_written)
        {
            StatsManager::getInstance().incrementLogMessages();
            resp.type = MessageType::LOG_ACK;
            resp.payload = R"({"status":"success"})";
            return resp;
        }
        else
        {
            StatsManager::getInstance().incrementErrors();
            resp.type = MessageType::ERROR_RESP;
            resp.payload = R"({"status":500, "message":"log write failed."})";
            return resp;
        }
    }
    Response handleStats(const Request &request)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::STATS_RESP;

        uint64_t requests = StatsManager::getInstance().getTotalRequests();
        uint64_t logMessages = StatsManager::getInstance().getTotalLogMessages();
        uint64_t errors = StatsManager::getInstance().getTotalErrors();
        uint64_t recv_bytes = StatsManager::getInstance().getReadBytes();
        uint64_t sent_bytes = StatsManager::getInstance().getWriteBytes();
        uint64_t active_connections = StatsManager::getInstance().getConnections();

        TcpServer *instance_ = TcpServer::getInstance();
        uint64_t request_queue_backlog = instance_->getRequestQueueSize();
        uint64_t response_queue_backlog = instance_->getResponseQueueSize();

        std::string json = "{\"total_requests\": ";
        json += std::to_string(requests);
        json += ", \"total_logs\": ";
        json += std::to_string(logMessages);
        json += ", \"total_errors\": ";
        json += std::to_string(errors);
        json += ", \"total_recv_bytes\": ";
        json += std::to_string(recv_bytes);
        json += ", \"total_sent_bytes\": ";
        json += std::to_string(sent_bytes);
        json += ", \"active_connections\": ";
        json += std::to_string(active_connections);
        json += ", \"total_request_queue_backlog\": ";
        json += std::to_string(request_queue_backlog);
        json += ", \"total_response_queue_backlog\": ";
        json += std::to_string(response_queue_backlog);
        json += "}";
        resp.payload = json;
        return resp;
    }
    Response makeErrorResponse(const Request &request)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::ERROR_RESP;
        resp.payload = R"({"status":400,"message":"unknown type"})";
        return resp;
    }

    Response makeErrorResponse(const Request &request, int status, const std::string &message)
    {
        Response resp;
        resp.fd = request.fd;
        resp.conn_id = request.conn_id;
        resp.version = request.version;
        resp.request_id = request.request_id;
        resp.type = MessageType::ERROR_RESP;

        nlohmann::json j;
        j["status"] = status;
        j["message"] = message;
        resp.payload = j.dump();
        return resp;
    }

}
