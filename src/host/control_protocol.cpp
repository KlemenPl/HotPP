#include "host/control_protocol.h"

#include <vector>

namespace hotpp::host {

bool send_json(Stream& stream, proto::MsgType type, const json::Value& value) {
    const std::string text = value.dump();
    if (text.size() > kMaxControlReply) return false;
    uint8_t header[proto::kHeaderSize];
    proto::encode_header(header, type, uint32_t(text.size()));
    return stream.send(header, sizeof header) && (text.empty() || stream.send(text.data(), text.size()));
}

RecvStatus recv_json_payload(Stream& stream, const proto::Header& h, json::Value& value, uint32_t max_size) {
    if (h.size > max_size) return RecvStatus::Closed;
    std::string payload(h.size, '\0');
    if (h.size && !stream.recv(payload.data(), h.size)) return RecvStatus::Closed;
    try {
        value = json::Value::parse(payload);
    } catch (const json::JsonError&) {
        return RecvStatus::Malformed;
    }
    return RecvStatus::Ok;
}

RecvStatus recv_json(Stream& stream, proto::MsgType& type, json::Value& value, uint32_t max_size) {
    uint8_t header[proto::kHeaderSize];
    if (!stream.recv(header, sizeof header)) return RecvStatus::Closed;
    proto::Header h;
    if (!proto::decode_header(header, h)) return RecvStatus::Closed;
    type = h.type;
    return recv_json_payload(stream, h, value, max_size);
}

} // namespace hotpp::host
