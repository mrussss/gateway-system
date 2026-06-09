#include "net/TcpServer.hpp"
#include "protocol/MessageType.hpp"
#include "protocol/Request.hpp"

int main()
{
    TcpServer server(8080);

    Request req;
    req.fd = 5;

    server.start();
    return 0;
}