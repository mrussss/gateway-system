#include "control/ControlPlaneClient.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "common/Logger.hpp"
#include "nlohmann/json.hpp"

ControlPlaneClient::ControlPlaneClient(std::string host, int port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms)
{
}

bool ControlPlaneClient::checkAuth(const std::string &client_id, const std::string &token) const
{
    nlohmann::json payload = {
        {"client_id", client_id},
        {"token", token},
    };

    std::string response_body;
    if (!postJson("/auth/check", payload.dump(), response_body))
    {
        LOG_ERROR("%s", "control plane auth request failed, default reject");
        return false;
    }

    try
    {
        auto json = nlohmann::json::parse(response_body);
        bool allowed = json.value("allowed", false);
        std::string reason = json.value("reason", "");
        LOG_INFO("control plane auth result: client_id=%s allowed=%d reason=%s",
                 client_id.c_str(), allowed ? 1 : 0, reason.c_str());
        return allowed;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("control plane auth response parse failed: %s", e.what());
        return false;
    }
}

bool ControlPlaneClient::reportMetrics(const GatewayMetrics &metrics) const
{
    nlohmann::json payload = {
        {"gateway_id", metrics.gateway_id},
        {"active_connections", metrics.active_connections},
        {"total_messages", metrics.total_messages},
        {"bytes_in", metrics.bytes_in},
        {"bytes_out", metrics.bytes_out},
        {"error_count", metrics.error_count},
        {"timestamp", metrics.timestamp},
    };

    std::string response_body;
    if (!postJson("/metrics/report", payload.dump(), response_body))
    {
        LOG_ERROR("%s", "control plane metrics report failed");
        return false;
    }

    LOG_INFO("metrics reported: gateway_id=%s active_connections=%llu total_messages=%llu",
             metrics.gateway_id.c_str(),
             static_cast<unsigned long long>(metrics.active_connections),
             static_cast<unsigned long long>(metrics.total_messages));
    return true;
}

bool ControlPlaneClient::reportClients(const std::string &gateway_id, const std::vector<ClientReport> &clients) const
{
    nlohmann::json client_items = nlohmann::json::array();
    for (const auto &client : clients)
    {
        client_items.push_back({
            {"client_id", client.client_id},
            {"remote_addr", client.remote_addr},
            {"connected_at", client.connected_at},
        });
    }

    nlohmann::json payload = {
        {"gateway_id", gateway_id},
        {"clients", client_items},
    };

    std::string response_body;
    if (!postJson("/clients/report", payload.dump(), response_body))
    {
        LOG_ERROR("%s", "control plane clients report failed");
        return false;
    }

    LOG_INFO("clients reported: gateway_id=%s count=%llu",
             gateway_id.c_str(),
             static_cast<unsigned long long>(clients.size()));
    return true;
}

bool ControlPlaneClient::postJson(const std::string &path, const std::string &body, std::string &response_body) const
{
    int fd = connectWithTimeout();
    if (fd == -1)
    {
        LOG_ERROR("control plane connect failed: path=%s", path.c_str());
        return false;
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host_ << ":" << port_ << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    if (!sendAll(fd, req.str()))
    {
        LOG_ERROR("control plane request send failed: path=%s", path.c_str());
        close(fd);
        return false;
    }

    std::string response = readResponse(fd);
    close(fd);

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.find("HTTP/1.1 200") != 0)
    {
        LOG_ERROR("control plane returned invalid HTTP response: path=%s", path.c_str());
        return false;
    }

    response_body = response.substr(header_end + 4);
    return true;
}

int ControlPlaneClient::connectWithTimeout() const
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *result = nullptr;
    std::string port = std::to_string(port_);
    int gai_ret = getaddrinfo(host_.c_str(), port.c_str(), &hints, &result);
    if (gai_ret != 0)
    {
        LOG_ERROR("control plane resolve failed: host=%s port=%d error=%s",
                  host_.c_str(), port_, gai_strerror(gai_ret));
        return -1;
    }

    for (addrinfo *rp = result; rp != nullptr; rp = rp->ai_next)
    {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)
        {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            close(fd);
            continue;
        }

        int ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == -1 && errno != EINPROGRESS)
        {
            close(fd);
            continue;
        }

        if (ret == -1)
        {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);

            timeval timeout{};
            timeout.tv_sec = timeout_ms_ / 1000;
            timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
            ret = select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
            if (ret <= 0)
            {
                close(fd);
                continue;
            }

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == -1 || so_error != 0)
            {
                close(fd);
                continue;
            }
        }

        timeval io_timeout{};
        io_timeout.tv_sec = timeout_ms_ / 1000;
        io_timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

        if (fcntl(fd, F_SETFL, flags) == -1)
        {
            close(fd);
            continue;
        }

        freeaddrinfo(result);
        return fd;
    }

    freeaddrinfo(result);
    return -1;
}

bool ControlPlaneClient::sendAll(int fd, const std::string &data) const
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string ControlPlaneClient::readResponse(int fd) const
{
    std::string response;
    char buf[4096];
    while (true)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            response.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0)
        {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        break;
    }
    return response;
}
