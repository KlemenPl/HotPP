// The control channel between a host and its controllers (hotpp mcp): length-
// prefixed JSON messages on the host's socket. See docs/DESIGN.md §3.4.
//
//   controller -> host   ControlHello {"control_version": 1, "client": "..."}
//   host -> controller   Control      {"hello": {...host...}, "control_version": 1}
//                                     or {"error": {"message": ...}} and the connection closes
//   controller -> host   Control      {"id": n, "method": "...", "params": {...}}
//   host -> controller   Control      {"id": n, "result": {...}} | {"id": n, "error": {"message": ...}}
//                                     | {"id": n, "progress": "text"} (any number, before the result)
#pragma once

#include "common/protocol.h"
#include "host/platform.h"
#include "host/util/json.h"

#include <cstdint>
#include <mutex>

namespace hotpp::host {

constexpr uint32_t kControlVersion = 1;
constexpr uint32_t kMaxControlRequest = 1u << 20; // controller -> host
constexpr uint32_t kMaxControlReply = 64u << 20;  // host -> controller
constexpr int kMaxControllers = 16;               // connections per host
constexpr int kMaxRequestsInFlight = 8;           // per connection

bool send_json(Stream& stream, proto::MsgType type, const json::Value& value);

enum class RecvStatus { Ok, Closed, Malformed };
// Reads one message. Malformed: a frame that isn't JSON (the stream stays usable);
// a bad header or an oversized frame counts as Closed.
RecvStatus recv_json(Stream& stream, proto::MsgType& type, json::Value& value, uint32_t max_size);
// The payload of an already read header.
RecvStatus recv_json_payload(Stream& stream, const proto::Header& header, json::Value& value, uint32_t max_size);

} // namespace hotpp::host
