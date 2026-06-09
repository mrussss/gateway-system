#include "net/TcpServer.hpp"
#include "protocol/MessageType.hpp"
#include "protocol/Request.hpp"

int main()
{
    TcpServer server(9000);

    Request req;
    req.fd = 5;

    server.start();
    return 0;
}
