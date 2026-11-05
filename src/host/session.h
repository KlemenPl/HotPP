// One connected agent: the handshake, then watching its sources, recompiling,
// linking and sending patches.
#pragma once

#include "common/messages.h"
#include "host/compile_db.h"
#include "host/deps.h"
#include "host/image/dwarf_types.h"
#include "host/image/elf_image.h"
#include "host/link/symbol_index.h"
#include "host/platform.h"
#include "host/watcher.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace hotpp::link {
struct LinkResult;
}

namespace hotpp::host {

struct HostOptions {
    bool verbose = false;      // print every binding decision of every patch
    std::string build_command; // rebuilds the program for a hot restart (run with /bin/sh -c)
    bool auto_restart = false; // hot-restart when a change can't be patched
};

// Bumped (e.g. by SIGUSR1) to ask every connected program to hot-restart.
extern std::atomic<int> g_restart_requests;

class Session {
public:
    Session(std::unique_ptr<Stream> stream, HostOptions options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    struct Outcome {
        bool hello = false;     // the agent completed the handshake
        bool layout_ok = false; // its patch region is usable
    };
    // Serves the connection until the agent disconnects.
    Outcome run();

private:
    bool send(proto::MsgType type, const proto::Writer& w);
    bool recv(proto::Header& h, std::vector<uint8_t>& payload);

    // A loaded module (the executable or a shared library) built from sources in
    // compile_commands.json, and everything needed to patch it.
    struct ModuleState {
        size_t index = 0; // into hello_.modules
        std::string name;
        std::unique_ptr<image::ElfImage> image;
        link::SymbolIndex symbols;
        std::set<std::filesystem::path> units; // sources it was built from
        uint64_t region_base = 0, cursor = 0, end = 0; // its patch region
        std::unordered_map<std::string, image::TypeLayout> layouts, pending_layouts;
        bool layouts_ok = false;
    };

    void start_hot_reload();
    void setup_modules();
    void resume_or_reset();
    void save_state();
    std::optional<proto::RegionRecord> reserve_region(uint32_t module_index, uint64_t size);
    void bind_interposed_variables(ModuleState& module);
    void watch_loop();
    void watch_headers();
    bool changed(const std::filesystem::path& file); // content differs from the last time we looked
    void handle_changes(const std::vector<std::filesystem::path>& files);
    void warn_layout_changes(ModuleState& module, const link::LinkResult& linked);
    void hot_restart(const std::string& reason);
    std::vector<uint64_t> resolve_external(const std::vector<std::string>& names);
    std::optional<proto::PatchResult> wait_for_result(uint32_t id);
    void say(const std::string& line) const;

    std::unique_ptr<Stream> stream_;
    HostOptions options_;
    std::mutex send_mutex_;
    proto::Hello hello_;
    std::string exe_name_;

    // Hot reload state, owned by the watcher thread once started.
    std::optional<CompileDb> db_;
    std::filesystem::file_time_type db_time_{};
    void reload_compile_db_if_changed();
    std::vector<std::unique_ptr<ModuleState>> modules_; // [0] is the executable
    std::set<std::filesystem::path> units_;              // sources of all of them
    std::unordered_map<std::string, size_t> content_hash_;
    std::unique_ptr<FileWatcher> watcher_;
    std::set<std::filesystem::path> watched_dirs_;
    DependencyGraph graph_;
    std::thread scan_thread_;
    std::mutex scan_mutex_;
    std::condition_variable scan_cv_;
    std::atomic<bool> scan_done_{false};
    std::atomic<bool> layouts_ready_{false}; // struct layouts read (after the dependency scan)
    std::filesystem::path patch_dir_;
    std::vector<std::pair<uint32_t, link::IndexDelta>> applied_; // every applied delta, for the state file
    uint32_t next_patch_ = 1;
    uint32_t next_request_ = 1;
    std::thread watch_thread_;
    std::atomic<bool> stop_{false};

    // Replies from the agent, handed from the connection thread to the watcher thread.
    std::mutex reply_mutex_;
    std::condition_variable reply_cv_;
    std::map<uint32_t, proto::PatchResult> results_;
    std::map<uint32_t, proto::ResolveReply> resolved_;
    std::map<uint32_t, proto::ReserveReply> reserved_;
    bool disconnected_ = false;
};

} // namespace hotpp::host
