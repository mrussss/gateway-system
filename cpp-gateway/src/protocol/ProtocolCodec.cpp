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

    // body_length 只计算消息体：version、type、request_id 和 payload。
    uint32_t body_length = 1 + 1 + 8 + response.payload.size();

    // 多字节长度和 request_id 在线路上使用网络字节序（大端）。
    uint32_t net_body_len = htonl(body_length);
    uint64_t net_req_id = htobe64(response.request_id);

    uint8_t version = response.version;
    uint8_t msg_type = static_cast<uint8_t>(response.type);

    // 编码顺序与协议布局一致：长度、版本、类型、请求 ID、payload。
    result.append(reinterpret_cast<const char *>(&net_body_len), sizeof(net_body_len));
    result.append(reinterpret_cast<const char *>(&version), sizeof(version));
    result.append(reinterpret_cast<const char *>(&msg_type), sizeof(msg_type));
    result.append(reinterpret_cast<const char *>(&net_req_id), sizeof(net_req_id));

    result.append(response.payload);

    return result;
}

DecodeStatus ProtocolCodec::decode(std::string &input_buffer, int fd, std::vector<Request> &out_requests,
                                   uint64_t conn_id, uint32_t max_body_size)
{
    size_t read_index = 0;
    bool need_more_data = false;
    while (true)
    {

        size_t remaining = input_buffer.size() - read_index;
        if (remaining == 0)
        {
            break;
        }
        // 长度前缀尚未收全；保留尾部数据，等下一次读取。
        if (remaining < 4)
        {
            need_more_data = true;
            break;
        }
        // 先将网络序长度转为主机序，再进行范围检查。
        uint32_t network_body_len = 0;
        std::memcpy(&network_body_len, input_buffer.data() + read_index, sizeof(uint32_t));
        uint32_t host_body_length = ntohl(network_body_len);

        // 在等待消息体前拒绝非法长度，避免为恶意声明分配过量内存。
        if (host_body_length < FIXED_BODY_SIZE || host_body_length > MAX_BODY_SIZE ||
            host_body_length > max_body_size)
        {
            return DecodeStatus::INVALID_LENGTH;
        }
        // 合法帧尚未收全；从本帧起保留数据供下一次读取。
        if (remaining < (4 + host_body_length))
        {
            need_more_data = true;
            break;
        }

        // 已有完整帧；按 [长度(4)][版本(1)][类型(1)][请求 ID(8)][payload] 解码。
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
