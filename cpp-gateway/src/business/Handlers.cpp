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

    AuthHandlingResult handleAuth(const Request &request,
                                  const ControlPlaneClient &control_plane,
                                  std::optional<ControlPlaneClient::Deadline> not_after)
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
                resp.status_code = 400;
                resp.payload = R"({"allowed":false,"code":"INVALID_REQUEST"})";
                resp.close_connection = true;
                return {std::move(resp), AuthOutcome::Denied};
            }

            std::string client_id = payload["client_id"];
            std::string token = payload["token"];
            const AuthResult result = control_plane.checkAuth(client_id, token, not_after);
            if (result.outcome != AuthOutcome::Allowed)
            {
                StatsManager::getInstance().incrementErrors();
                resp.status_code = result.outcome == AuthOutcome::Denied ? 401 : 503;
                nlohmann::json response_payload = {
                    {"allowed", false},
                    {"code", result.outcome == AuthOutcome::Denied
                                 ? (result.reason_code.empty() ? "INVALID_CREDENTIALS"
                                                               : result.reason_code)
                                 : "AUTH_UNAVAILABLE"},
                };
                resp.payload = response_payload.dump();
                resp.close_connection = true;
                return {std::move(resp), result.outcome};
            }

            resp.status_code = 0;
            resp.payload = R"({"allowed":true,"code":"OK"})";
            resp.client_id_to_authenticate = client_id;
            return {std::move(resp), AuthOutcome::Allowed};
        }
        catch (const std::exception &)
        {
            StatsManager::getInstance().incrementErrors();
            resp.status_code = 400;
            resp.payload = R"({"allowed":false,"code":"INVALID_REQUEST"})";
            resp.close_connection = true;
            return {std::move(resp), AuthOutcome::Denied};
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
        uint64_t request_queue_capacity = instance_->getRequestQueueCapacity();
        uint64_t response_queue_capacity = instance_->getResponseQueueCapacity();
        uint64_t request_queue_peak = instance_->getRequestQueuePeakSize();
        uint64_t response_queue_peak = instance_->getResponseQueuePeakSize();
        uint64_t request_queue_rejected = StatsManager::getInstance().getRequestQueueRejected();
        uint64_t response_queue_rejected = StatsManager::getInstance().getResponseQueueRejected();
        const StatsSnapshot snapshot = StatsManager::getInstance().snapshot();
        const uint64_t auth_queue_backlog = instance_->getAuthQueueSize();
        const uint64_t auth_queue_capacity = instance_->getAuthQueueCapacity();
        const uint64_t auth_queue_peak = instance_->getAuthQueuePeakSize();

        resp.payload = nlohmann::json{
            {"total_requests", requests},
            {"total_logs", logMessages},
            {"total_errors", errors},
            {"total_recv_bytes", recv_bytes},
            {"total_sent_bytes", sent_bytes},
            {"active_connections", active_connections},
            {"total_request_queue_backlog", request_queue_backlog},
            {"total_response_queue_backlog", response_queue_backlog},
            {"request_queue_capacity", request_queue_capacity},
            {"response_queue_capacity", response_queue_capacity},
            {"request_queue_peak", request_queue_peak},
            {"response_queue_peak", response_queue_peak},
            {"request_queue_rejected", request_queue_rejected},
            {"response_queue_rejected", response_queue_rejected},
            {"auth_queue_capacity", auth_queue_capacity},
            {"auth_queue_backlog", auth_queue_backlog},
            {"auth_queue_peak", auth_queue_peak},
            {"auth_queue_rejected", snapshot.auth_queue_rejected},
            {"auth_in_flight", instance_->getAuthInFlight()},
            {"auth_tasks_cancelled_before_start",
             snapshot.auth_tasks_cancelled_before_start},
            {"auth_allowed", snapshot.auth_allowed},
            {"auth_denied", snapshot.auth_denied},
            {"auth_unavailable", snapshot.auth_unavailable},
            {"auth_duration_count", snapshot.auth_duration_count},
            {"auth_duration_total_us", snapshot.auth_duration_total_us},
            {"response_queue_rejected_normal",
             snapshot.response_queue_rejected_normal},
            {"response_queue_rejected_auth", snapshot.response_queue_rejected_auth},
        }.dump();
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
