#include "common/messages.h"
#include "unit/test.h"

using namespace hotpp::proto;

namespace {

Hello sample_hello() {
    Hello h;
    h.pid = 4242;
    h.platform = Platform::Linux;
    h.arch = Arch::X64;
    h.exe = "/usr/bin/app";
    h.compile_commands = "/build/compile_commands.json";
    h.modules.push_back({0x555500000000, 0x555500000000, 0x5000, "/usr/bin/app"});
    h.modules.push_back({0x7f0000000000, 0x7f0000001000, 0x200000, "/lib/libc.so.6"});
    h.regions.push_back({0x5554e0000000, 512ull << 20, 0});
    return h;
}

} // namespace

HOTPP_TEST(header_round_trip) {
    uint8_t buf[kHeaderSize];
    encode_header(buf, MsgType::HelloAck, 1234);
    Header h;
    CHECK(decode_header(buf, h));
    CHECK(h.magic == kMagic);
    CHECK(h.version == kVersion);
    CHECK(h.type == MsgType::HelloAck);
    CHECK(h.size == 1234);
}

HOTPP_TEST(header_rejects_bad_magic_and_huge_size) {
    uint8_t buf[kHeaderSize];
    encode_header(buf, MsgType::Hello, 16);
    buf[0] ^= 0xFF;
    Header h;
    CHECK(!decode_header(buf, h));
    encode_header(buf, MsgType::Hello, kMaxPayload + 1);
    CHECK(!decode_header(buf, h));
}

HOTPP_TEST(hello_round_trip) {
    const Hello in = sample_hello();
    Writer w;
    encode(w, in);
    CHECK(w.ok());
    Reader r(w.data(), w.size());
    Hello out;
    CHECK(decode(r, out));
    CHECK(out.pid == in.pid);
    CHECK(out.platform == in.platform);
    CHECK(out.arch == in.arch);
    CHECK(out.exe == in.exe);
    CHECK(out.compile_commands == in.compile_commands);
    CHECK(out.modules.size() == 2);
    CHECK(out.modules[1].base == in.modules[1].base);
    CHECK(out.modules[1].start == in.modules[1].start);
    CHECK(out.modules[1].size == in.modules[1].size);
    CHECK(out.modules[1].path == in.modules[1].path);
    CHECK(out.regions.size() == 1);
    CHECK(out.regions[0].base == in.regions[0].base);
    CHECK(out.regions[0].size == in.regions[0].size);
}

HOTPP_TEST(hello_rejects_truncated_and_trailing_bytes) {
    Writer w;
    encode(w, sample_hello());
    for (size_t cut : {size_t(0), size_t(3), w.size() / 2, w.size() - 1}) {
        Reader r(w.data(), cut);
        Hello out;
        CHECK(!decode(r, out));
    }
    Writer longer;
    encode(longer, sample_hello());
    longer.u8(0);
    Reader r(longer.data(), longer.size());
    Hello out;
    CHECK(!decode(r, out));
}

HOTPP_TEST(hello_rejects_absurd_module_count) {
    Writer w;
    w.u32(1);
    w.u8(1);
    w.u8(1);
    w.str("");
    w.str("");
    w.u32(0xFFFFFFFF); // claims 4 billion modules with no data behind it
    Reader r(w.data(), w.size());
    Hello out;
    CHECK(!decode(r, out));
}
