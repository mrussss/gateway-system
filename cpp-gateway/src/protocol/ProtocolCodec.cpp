#include "protocol/ProtocolCodec.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <endian.h>

namespace
{
    constexpr uint32_t FIXED_BODY_SIZE = 1 + 1 + 8;
    constexpr uint32_t MAX_BODY_SIZE = MAX_PAYLOAD_SIZE + FIXED_BODY_SIZE;
}

std::string ProtocolCodec::encode(const Response &response)
{

    std::string result;

    // body_length = 1 (version) + 1 (type) + 8 (request_id) + payload.size()
    uint32_t body_length = 1 + 1 + 8 + response.payload.size();

    // convert to network byte order (big-endian)
    uint32_t net_body_len = htonl(body_length);
    uint64_t net_req_id = htobe64(response.request_id);

    uint8_t version = response.version;
    uint8_t msg_type = static_cast<uint8_t>(response.type);

    // append in order: body_len, version, type, request_id, payload
    result.append(reinterpret_cast<const char *>(&net_body_len), sizeof(net_body_len));
    result.append(reinterpret_cast<const char *>(&version), sizeof(version));
    result.append(reinterpret_cast<const char *>(&msg_type), sizeof(msg_type));
    result.append(reinterpret_cast<const char *>(&net_req_id), sizeof(net_req_id));

    result.append(response.payload);

    return result;
}

DecodeStatus ProtocolCodec::decode(std::string &input_buffer, int fd, std::vector<Request> &out_requests, uint64_t conn_id)
{
    size_t read_index = 0;
    bool need_more_data = false;
    while (true)
    {

        size_t remaining = input_buffer.size() - read_index;
        // not enough for 4-byte header → need more data
        if (remaining < 4)
        {
            need_more_data = true;
            break;
        }
        // extract body_length and convert from network to host byte order
        uint32_t network_body_len = 0;
        std::memcpy(&network_body_len, input_buffer.data() + read_index, sizeof(uint32_t));
        uint32_t host_body_length = ntohl(network_body_len);

        // validate body_length range (OOM / invalid-protocol guard)
        if (host_body_length < FIXED_BODY_SIZE || host_body_length > MAX_BODY_SIZE)
        {
            return DecodeStatus::INVALID_LENGTH;
        }
        // incomplete body → need more data
        if (remaining < (4 + host_body_length))
        {
            need_more_data = true;
            break;
        }

        // buffer contains at least one complete packet — decode it
        // layout: [body_len(4)][version(1)][type(1)][request_id(8)][payload(N)]
        Request req;
        req.version = static_cast<uint8_t>(input_buffer[read_index + 4]);
        req.type = static_cast<MessageType>(input_buffer[read_index + 5]);
        uint64_t request_id_net;
        std::memcpy(&request_id_net, input_buffer.data() + read_index + 6, 8);
        req.request_id = be64toh(request_id_net);
        size_t payload_length = host_body_length - FIXED_BODY_SIZE;
        req.payload = std::string(input_buffer.data() + read_index + 14, payload_length);
        req.fd = fd;
        req.conn_id = conn_id;
        out_requests.push_back(req);

        read_index += (4 + host_body_length);
    }
    if (read_index > 0)
    {
        input_buffer.erase(0, read_index);
    }
    return need_more_data ? DecodeStatus::NEED_MORE_DATA : DecodeStatus::OK;
}