#include "TestHarness.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "control/ControlPlaneClient.hpp"

namespace
{
void sendAll(int fd, const std::string &data)
{
    size_t offset = 0;
    while (offset < data.size())
    {
        const ssize_t sent = send(fd, data.data() + offset, data.size() - offset,
                                  MSG_NOSIGNAL);
        if (sent <= 0)
        {
            throw std::runtime_error("fake server send failed");
        }
        offset += static_cast<size_t>(sent);
    }
}

std::string response(std::string body, std::string headers = {}, int status = 200)
{
    return "HTTP/1.1 " + std::to_string(status) + " Test\r\n" + headers +
           "Content-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

class FakeHttpServer
{
public:
    explicit FakeHttpServer(std::function<void(int)> handler)
    {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ == -1)
        {
            throw std::runtime_error("fake server socket failed");
        }
        int enabled = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == -1 ||
            listen(listen_fd_, 1) == -1)
        {
            close(listen_fd_);
            throw std::runtime_error("fake server bind/listen failed");
        }
        socklen_t size = sizeof(address);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &size) == -1)
        {
            close(listen_fd_);
            throw std::runtime_error("fake server getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this, handler = std::move(handler)]
        {
            const int client = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client == -1)
            {
                return;
            }
            char input[4096];
            (void)recv(client, input, sizeof(input), 0);
            handler(client);
            close(client);
        });
    }

    ~FakeHttpServer()
    {
        if (thread_.joinable())
        {
            thread_.join();
        }
        close(listen_fd_);
    }

    int port() const noexcept { return port_; }

private:
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

AuthResult authAgainst(const std::function<void(int)> &handler, int timeout_ms = 500)
{
    FakeHttpServer server(handler);
    ControlPlaneClient client("127.0.0.1", server.port(), timeout_ms, "gateway-secret");
    return client.checkAuth("client-1", "secret");
}

void ignoreSignal(int) {}
} // namespace

int main()
{
    return runTests({
        {"allowed response completes at Content-Length without waiting for EOF", []
         {
             FakeHttpServer server([](int fd)
             {
                 sendAll(fd, response(R"({"allowed":true,"code":"OK"})"));
                 std::this_thread::sleep_for(std::chrono::milliseconds(250));
             });
             ControlPlaneClient client("127.0.0.1", server.port(), 1000,
                                       "gateway-secret");
             const auto started = std::chrono::steady_clock::now();
             const AuthResult result = client.checkAuth("client-1", "secret");
             const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started);
             CHECK(result.outcome == AuthOutcome::Allowed);
             CHECK(elapsed.count() < 200);
         }},
        {"credential denial is distinct from unavailability", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, response(R"({"allowed":false,"code":"INVALID_CREDENTIALS"})"));
             });
             CHECK(result.outcome == AuthOutcome::Denied);
             CHECK_EQ(result.reason_code, std::string("INVALID_CREDENTIALS"));
             CHECK(result.http_error == HttpError::None);
         }},
        {"invalid auth JSON fails closed", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, response("not-json"));
             });
             CHECK(result.outcome == AuthOutcome::Unavailable);
             CHECK(result.http_error == HttpError::InvalidJson);
         }},
        {"missing allowed field fails closed", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, response(R"({"code":"OK"})"));
             });
             CHECK(result.outcome == AuthOutcome::Unavailable);
             CHECK(result.http_error == HttpError::InvalidJson);
         }},
        {"non-2xx response is unavailable", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, response(R"({"code":"UNAVAILABLE"})", {}, 503));
             });
             CHECK(result.outcome == AuthOutcome::Unavailable);
             CHECK(result.http_error == HttpError::HttpStatusError);
             CHECK_EQ(result.http_status, 503);
         }},
        {"premature EOF is rejected", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
             });
             CHECK(result.http_error == HttpError::PrematureEof);
         }},
        {"duplicate Content-Length is rejected", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\n{}");
             });
             CHECK(result.http_error == HttpError::DuplicateContentLength);
         }},
        {"Transfer-Encoding is explicitly rejected", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
             });
             CHECK(result.http_error == HttpError::UnsupportedTransferEncoding);
         }},
        {"Transfer-Encoding plus Content-Length is malformed", []
         {
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n");
             });
             CHECK(result.http_error == HttpError::MalformedResponse);
         }},
        {"missing length and unsupported encodings fail explicitly", []
         {
             const AuthResult missing = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{}");
             });
             CHECK(missing.http_error == HttpError::MissingContentLength);

             const AuthResult encoded = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 2\r\n\r\n{}");
             });
             CHECK(encoded.http_error == HttpError::MalformedResponse);
         }},
        {"case-insensitive Content-Length is accepted and extra body is rejected", []
         {
             const AuthResult valid = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: 16\r\n\r\n{\"allowed\":true}");
             });
             CHECK(valid.outcome == AuthOutcome::Allowed);

             const AuthResult extra = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}extra");
             });
             CHECK(extra.http_error == HttpError::MalformedResponse);
         }},
        {"header and declared body limits are independent", []
         {
             const AuthResult header = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nX-Large: " +
                             std::string(ControlPlaneClient::MAX_HTTP_HEADER_BYTES, 'x'));
             });
             CHECK(header.http_error == HttpError::HeaderTooLarge);

             const AuthResult body = authAgainst([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: " +
                             std::to_string(ControlPlaneClient::MAX_HTTP_BODY_BYTES + 1) +
                             "\r\n\r\n");
             });
             CHECK(body.http_error == HttpError::BodyTooLarge);
         }},
        {"slow trickle cannot reset the shared deadline", []
         {
             FakeHttpServer server([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{");
                 std::this_thread::sleep_for(std::chrono::milliseconds(250));
                 (void)send(fd, "}", 1, MSG_NOSIGNAL);
             });
             ControlPlaneClient client("127.0.0.1", server.port(), 100,
                                       "gateway-secret");
             const auto started = std::chrono::steady_clock::now();
             const AuthResult result = client.checkAuth("client-1", "secret");
             const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started);
             CHECK(result.http_error == HttpError::DeadlineExceeded);
             CHECK(elapsed.count() < 220);
         }},
        {"caller lifecycle deadline caps a new AUTH request", []
         {
             FakeHttpServer server([](int fd)
             {
                 std::this_thread::sleep_for(std::chrono::milliseconds(250));
                 const std::string late_response =
                     "HTTP/1.1 200 OK\r\nContent-Length: 16\r\n\r\n{\"allowed\":true}";
                 (void)send(fd, late_response.data(), late_response.size(), MSG_NOSIGNAL);
             });
             ControlPlaneClient client("127.0.0.1", server.port(), 1000,
                                       "gateway-secret");
             const auto started = std::chrono::steady_clock::now();
             const AuthResult result = client.checkAuth(
                 "client-1", "secret", started + std::chrono::milliseconds(80));
             const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started);
             CHECK(result.http_error == HttpError::DeadlineExceeded);
             CHECK(elapsed.count() < 200);
         }},
        {"EINTR retries preserve the original deadline", []
         {
             struct sigaction action{};
             struct sigaction previous{};
             action.sa_handler = ignoreSignal;
             sigemptyset(&action.sa_mask);
             CHECK_EQ(sigaction(SIGUSR1, &action, &previous), 0);

             FakeHttpServer server([](int fd)
             {
                 std::this_thread::sleep_for(std::chrono::milliseconds(250));
                 (void)send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 38,
                            MSG_NOSIGNAL);
             });
             ControlPlaneClient client("127.0.0.1", server.port(), 100);
             const pthread_t target = pthread_self();
             std::atomic<bool> finished{false};
             std::thread interrupter([&]
             {
                 while (!finished.load())
                 {
                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                     pthread_kill(target, SIGUSR1);
                 }
             });
             const auto started = std::chrono::steady_clock::now();
             const AuthResult result = client.checkAuth("client-1", "secret");
             const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started);
             finished = true;
             interrupter.join();
             CHECK_EQ(sigaction(SIGUSR1, &previous, nullptr), 0);
             CHECK(result.http_error == HttpError::DeadlineExceeded);
             CHECK(elapsed.count() < 220);
         }},
        {"header injection is rejected at construction", []
         {
             bool host_rejected = false;
             bool token_rejected = false;
             try { ControlPlaneClient client("host\r\ninjected", 80, 100); }
             catch (const std::invalid_argument &) { host_rejected = true; }
             try { ControlPlaneClient client("localhost", 80, 100, "x\nheader"); }
             catch (const std::invalid_argument &) { token_rejected = true; }
             CHECK(host_rejected);
             CHECK(token_rejected);
         }},
        {"HTTP failures are counted by low-cardinality category", []
         {
             FakeHttpServer server([](int fd)
             {
                 sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}");
             });
             ControlPlaneClient client("127.0.0.1", server.port(), 500);
             const AuthResult result = client.checkAuth("client-1", "secret");
             const ControlPlaneMetricsSnapshot metrics = client.metricsSnapshot();
             CHECK(result.http_error == HttpError::DuplicateContentLength);
             CHECK_EQ(metrics.requests_auth, uint64_t{1});
             CHECK_EQ(metrics.errors_protocol, uint64_t{1});
         }},
        {"poll supports descriptors above FD_SETSIZE", []
         {
             std::vector<int> descriptors;
             while (descriptors.empty() || descriptors.back() <= 1100)
             {
                 const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                 if (fd == -1)
                 {
                     throw std::runtime_error("unable to allocate high descriptor");
                 }
                 descriptors.push_back(fd);
             }
             const AuthResult result = authAgainst([](int fd)
             {
                 sendAll(fd, response(R"({"allowed":true})"));
             });
             for (const int fd : descriptors) { close(fd); }
             CHECK(result.outcome == AuthOutcome::Allowed);
         }},
    });
}
