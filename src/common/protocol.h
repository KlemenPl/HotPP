// HotPP host <-> agent wire format: framing and a bounds-checked reader/writer.
// Shared by the agent and the host; uses no exceptions.
//
// Every message is a 12-byte header followed by `size` payload bytes:
//   u32 magic ("HOTP")  u16 version  u16 type  u32 size
// All integers are little-endian. Strings are u32 length + bytes (UTF-8, no NUL).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace hotpp::proto {

constexpr uint32_t kMagic = 0x50544F48; // "HOTP"
constexpr uint16_t kVersion = 2;
constexpr size_t kHeaderSize = 12;
constexpr uint32_t kMaxPayload = 1u << 30;

enum class MsgType : uint16_t {
    Hello = 1,          // agent -> host
    HelloAck = 2,       // host -> agent
    Bye = 3,            // either way: orderly disconnect
    Log = 4,            // either way: text to show the user
    Patch = 5,          // host -> agent: code to map and functions to redirect
    PatchResult = 6,    // agent -> host
    ResolveRequest = 7, // host -> agent: look up symbols exported by loaded libraries
    ResolveReply = 8,   // agent -> host
};

// Memory protection bits used in Patch segments.
constexpr uint8_t kProtRead = 1;
constexpr uint8_t kProtWrite = 2;
constexpr uint8_t kProtExec = 4;

enum class Platform : uint8_t { Unknown = 0, Linux = 1, Windows = 2 };
enum class Arch : uint8_t { Unknown = 0, X64 = 1 };
enum class LogLevel : uint8_t { Info = 0, Warning = 1, Error = 2 };

struct Header {
    uint32_t magic = 0;
    uint16_t version = 0;
    MsgType type{};
    uint32_t size = 0;
};

inline void store_le(uint8_t* p, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * i));
}

inline uint64_t load_le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

inline void encode_header(uint8_t out[kHeaderSize], MsgType type, uint32_t size) {
    store_le(out + 0, kMagic, 4);
    store_le(out + 4, kVersion, 2);
    store_le(out + 6, uint16_t(type), 2);
    store_le(out + 8, size, 4);
}

// False on a bad magic or oversized payload. The version is returned as-is so the
// caller can report a mismatch.
inline bool decode_header(const uint8_t in[kHeaderSize], Header& h) {
    h.magic = uint32_t(load_le(in + 0, 4));
    h.version = uint16_t(load_le(in + 4, 2));
    h.type = MsgType(load_le(in + 6, 2));
    h.size = uint32_t(load_le(in + 8, 4));
    return h.magic == kMagic && h.size <= kMaxPayload;
}

class Writer {
public:
    Writer() = default;
    ~Writer() { std::free(data_); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void u8(uint8_t v) { put_le(v, 1); }
    void u16(uint16_t v) { put_le(v, 2); }
    void u32(uint32_t v) { put_le(v, 4); }
    void u64(uint64_t v) { put_le(v, 8); }
    void str(const char* s, size_t n) {
        u32(uint32_t(n));
        put(s, n);
    }
    void str(const char* s) { str(s, s ? std::strlen(s) : 0); }
    void raw(const void* p, size_t n) { put(p, n); }

    bool ok() const { return ok_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    void put_le(uint64_t v, int n) {
        uint8_t b[8];
        store_le(b, v, n);
        put(b, size_t(n));
    }
    void put(const void* p, size_t n) {
        if (!ok_ || n == 0) return;
        if (size_ + n > cap_) {
            size_t cap = cap_ ? cap_ : 256;
            while (cap < size_ + n) cap *= 2;
            auto* grown = static_cast<uint8_t*>(std::realloc(data_, cap));
            if (!grown) {
                ok_ = false;
                return;
            }
            data_ = grown;
            cap_ = cap;
        }
        std::memcpy(data_ + size_, p, n);
        size_ += n;
    }

    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
    bool ok_ = true;
};

struct StrView {
    const char* data = "";
    uint32_t size = 0;
};

// Reads past the end yield zeros and clear ok(); check ok() once at the end.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint8_t u8() { return uint8_t(get(1)); }
    uint16_t u16() { return uint16_t(get(2)); }
    uint32_t u32() { return uint32_t(get(4)); }
    uint64_t u64() { return get(8); }
    StrView str() {
        uint32_t n = u32();
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return {};
        }
        StrView s{reinterpret_cast<const char*>(data_ + pos_), n};
        pos_ += n;
        return s;
    }
    // Pointer to the next n bytes, or nullptr (and !ok()) if fewer remain.
    const uint8_t* raw(size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }

    bool ok() const { return ok_; }
    bool at_end() const { return pos_ == size_; }
    size_t remaining() const { return size_ - pos_; }

private:
    uint64_t get(int n) {
        if (!ok_ || size_t(n) > remaining()) {
            ok_ = false;
            return 0;
        }
        uint64_t v = load_le(data_ + pos_, n);
        pos_ += size_t(n);
        return v;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace hotpp::proto
