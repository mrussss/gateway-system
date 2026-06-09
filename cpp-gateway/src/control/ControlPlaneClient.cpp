#include "control/ControlPlaneClient.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
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
    int fd = connectWithTimeout();
    if (fd == -1)
    {
        LOG_ERROR("%s", "control plane auth connect failed, default reject");
        return false;
    }

    nlohmann::json payload = {
        {"client_id", client_id},
        {"token", token},
    };
    std::string body = payload.dump();

    std::ostringstream req;
    req << "POST /auth/check HTTP/1.1\r\n"
        << "Host: " << host_ << ":" << port_ << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    bool allowed = false;
    if (!sendAll(fd, req.str()))
    {
        LOG_ERROR("%s", "control plane auth request send failed, default reject");
        close(fd);
        return false;
    }

    std::string response = readResponse(fd);
    close(fd);

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.find("HTTP/1.1 200") != 0)
    {
        LOG_ERROR("%s", "control plane auth returned invalid HTTP response, default reject");
        return false;
    }

    try
    {
        std::string response_body = response.substr(header_end + 4);
        auto json = nlohmann::json::parse(response_body);
        allowed = json.value("allowed", false);
        std::string reason = json.value("reason", "");
        LOG_INFO("control plane auth result: client_id=%s allowed=%d reason=%s",
                 client_id.c_str(), allowed ? 1 : 0, reason.c_str());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("control plane auth response parse failed: %s", e.what());
        return false;
    }

    return allowed;
}

int ControlPlaneClient::connectWithTimeout() const
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1)
    {
        close(fd);
        return -1;
    }

    int ret = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == -1 && errno != EINPROGRESS)
    {
        close(fd);
        return -1;
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
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == -1 || so_error != 0)
        {
            close(fd);
            return -1;
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
        return -1;
    }

    return fd;
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
