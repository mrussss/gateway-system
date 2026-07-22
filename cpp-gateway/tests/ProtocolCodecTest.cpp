#include "TestHarness.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <string>
#include <vector>

#include "protocol/ProtocolCodec.hpp"

namespace
{
std::string makePacket(uint8_t version, MessageType type, uint64_t request_id,
                       const std::string &payload)
{
    Response response{};
    response.version = version;
    response.type = type;
    response.request_id = request_id;
    response.payload = payload;
    return ProtocolCodec::encode(response);
}

void checkRequest(const Request &request, int fd, uint64_t conn_id, uint8_t version,
                  MessageType type, uint64_t request_id, const std::string &payload)
{
    CHECK_EQ(request.fd, fd);
    CHECK_EQ(request.conn_id, conn_id);
    CHECK_EQ(request.version, version);
    CHECK(request.type == type);
    CHECK_EQ(request.request_id, request_id);
    CHECK_EQ(request.payload, payload);
}
}

int main()
{
    return runTests({
        {"single complete packet and round trip", []
         {
             std::string input = makePacket(1, MessageType::ECHO, 42, "hello");
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 7, requests, 99) == DecodeStatus::OK);
             CHECK(input.empty());
             CHECK_EQ(requests.size(), size_t{1});
             checkRequest(requests[0], 7, 99, 1, MessageType::ECHO, 42, "hello");
         }},
        {"half packet is retained", []
         {
             const std::string packet = makePacket(1, MessageType::PING, 1, "abc");
             std::string input = packet.substr(0, 8);
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 1, requests, 2) == DecodeStatus::NEED_MORE_DATA);
             CHECK(requests.empty());
             CHECK_EQ(input.size(), size_t{8});
             input.append(packet.substr(8));
             CHECK(ProtocolCodec::decode(input, 1, requests, 2) == DecodeStatus::OK);
             CHECK_EQ(requests.size(), size_t{1});
         }},
        {"sticky packets decode in order", []
         {
             std::string input = makePacket(1, MessageType::PING, 10, "") +
                                 makePacket(1, MessageType::ECHO, 11, "second");
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 3, requests, 4) == DecodeStatus::OK);
             CHECK_EQ(requests.size(), size_t{2});
             CHECK_EQ(requests[0].request_id, uint64_t{10});
             CHECK_EQ(requests[1].request_id, uint64_t{11});
             CHECK_EQ(requests[1].payload, std::string("second"));
         }},
        {"empty payload", []
         {
             std::string input = makePacket(1, MessageType::PING, 123, "");
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 8, requests, 9) == DecodeStatus::OK);
             CHECK_EQ(requests.size(), size_t{1});
             CHECK(requests[0].payload.empty());
         }},
        {"maximum legal payload", []
         {
             std::string payload(MAX_PAYLOAD_SIZE, 'x');
             std::string input = makePacket(1, MessageType::ECHO, 5, payload);
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 1, requests, 1) == DecodeStatus::OK);
             CHECK_EQ(requests.size(), size_t{1});
             CHECK_EQ(requests[0].payload.size(), static_cast<size_t>(MAX_PAYLOAD_SIZE));
         }},
        {"body length below fixed header is rejected", []
         {
             uint32_t invalid = htonl(9);
             std::string input(reinterpret_cast<const char *>(&invalid), sizeof(invalid));
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 1, requests, 1) == DecodeStatus::INVALID_LENGTH);
         }},
        {"oversized body length is rejected", []
         {
             uint32_t invalid = htonl(MAX_PAYLOAD_SIZE + 11);
             std::string input(reinterpret_cast<const char *>(&invalid), sizeof(invalid));
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 1, requests, 1) == DecodeStatus::INVALID_LENGTH);
         }},
        {"complete packets are consumed and partial tail remains", []
         {
             const std::string tail = makePacket(1, MessageType::ECHO, 2, "tail");
             std::string input = makePacket(1, MessageType::PING, 1, "") + tail.substr(0, 6);
             std::vector<Request> requests;
             CHECK(ProtocolCodec::decode(input, 1, requests, 1) == DecodeStatus::NEED_MORE_DATA);
             CHECK_EQ(requests.size(), size_t{1});
             CHECK_EQ(input, tail.substr(0, 6));
         }},
    });
}
