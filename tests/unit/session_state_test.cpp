#include "host/session_state.h"
#include "host/platform.h"
#include "unit/test.h"

#include <filesystem>
#include <fstream>


using namespace hotpp;

HOTPP_TEST(session_state_round_trips) {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / ("hotpp-state-" + std::to_string(hotpp::host::current_pid())) / "state";
    host::SessionState st;
    st.pid = 1234;
    st.next_patch = 7;
    st.modules.push_back({0, "/bin/app", {1, 2, 3}, 0x10000000, 0x10004000});
    link::IndexDelta d;
    link::FunctionVersion v{0x10001005, link::SiteLayout{}};
    d.versions.push_back({{false, "", "_Z5valuev"}, v});
    link::IndexEntry e;
    e.kind = link::SymbolKind::Data;
    e.address = 0x10003000;
    e.size = 4;
    e.writable = true;
    d.symbols.push_back({{true, "game.cpp", "_ZL5added"}, e});
    st.deltas.emplace_back(0, d);
    CHECK(host::save_session_state(file, st));

    const auto back = host::load_session_state(file);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->pid == 1234 && back->next_patch == 7);
        CHECK(back->modules.size() == 1 && back->modules[0].cursor == 0x10004000);
        CHECK(back->modules[0].build_id == std::vector<uint8_t>({1, 2, 3}));
        CHECK(back->deltas.size() == 1);
        const auto& bd = back->deltas[0].second;
        CHECK(bd.versions.size() == 1 && bd.versions[0].version.entry == 0x10001005 && bd.versions[0].version.site);
        CHECK(bd.symbols.size() == 1 && bd.symbols[0].key.local && bd.symbols[0].entry.address == 0x10003000);
    }
    std::ofstream(file, std::ios::trunc) << "garbage";
    CHECK(!host::load_session_state(file)); // corrupt files are rejected, not trusted
    fs::remove_all(file.parent_path());
}
