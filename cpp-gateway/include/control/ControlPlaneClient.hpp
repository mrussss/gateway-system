#pragma once

#include <string>

class ControlPlaneClient
{
public:
    ControlPlaneClient(std::string host, int port, int timeout_ms);

    bool checkAuth(const std::string &client_id, const std::string &token) const;

private:
    int connectWithTimeout() const;
    bool sendAll(int fd, const std::string &data) const;
    std::string readResponse(int fd) const;

    std::string host_;
    int port_;
    int timeout_ms_;
};
