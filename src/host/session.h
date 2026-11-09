// One connected agent: the handshake, then watching its sources, recompiling,
// linking and sending patches.
#pragma once

#include "common/messages.h"
#include "host/compile_db.h"
#include "host/deps.h"
#include "host/image/dwarf_types.h"
#include "host/image/elf_image.h"
#include "host/link/symbol_index.h"
#include "host/object/elf_object.h"
#include "host/platform.h"
#include "host/registry.h"
#include "host/report.h"
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
    bool auto_reload = true;   // patch saved files right away (else only when a controller asks)
};

// Bumped (e.g. by SIGUSR1) to ask every connected program to hot-restart.
extern std::atomic<int> g_restart_requests;

class Session {
public:
    // Events go to `events`; the session registers itself in `registry` for controllers.
    Session(std::unique_ptr<Stream> stream, HostOptions options, EventLog& events, Registry& registry);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    struct Outcome {
        bool hello = false;     // the agent completed the handshake
        bool layout_ok = false; // its patch region is usable
    };
    // Serves the connection until the agent disconnects. `hello` is the payload of
    // the first message, which was a Hello.
    Outcome run(const std::vector<uint8_t>& hello);

private:
    bool send(proto::MsgType type, const proto::Writer& w);
    bool recv(proto::Header& h, std::vector<uint8_t>& payload);

    // A loaded module (the executable or a shared library) built from sources in
    // compile_commands.json, and everything needed to patch it.
    struct ModuleState {
        size_t index = 0; // into hello_.modules
        std::string name;
        std::unique_ptr<image::ModuleImage> image;
        link::SymbolIndex symbols;
        std::set<std::filesystem::path> units; // sources it was built from
        uint64_t region_base = 0, cursor = 0, end = 0; // its patch region
        std::unordered_map<std::string, image::TypeLayout> layouts, pending_layouts;
        bool layouts_ok = false;
    };

    void start_hot_reload();
    void setup_modules();
    ModuleState* track_module(size_t index);
    void apply_module_changes();
    void resume_or_reset();
    void save_state();
    std::optional<proto::RegionRecord> reserve_region(uint32_t module_index, uint64_t size);
    void bind_interposed_variables(ModuleState& module);
    void watch_loop();
    void watch_headers();
    // Content differs from the last time we looked (and remembers the new content).
    bool changed(const std::filesystem::path& file);
    bool differs(const std::filesystem::path& file) const; // same, without remembering
    void remember(const std::filesystem::path& file);
    // Patches the changed ones among `files`. nullptr if there was nothing to patch.
    std::shared_ptr<PatchReport> handle_changes(const std::vector<std::filesystem::path>& files, const char* trigger);
    void warn_layout_changes(ModuleState& module, const link::LinkResult& linked,
                             const std::vector<const object::ElfObject*>& objects, PatchReport& report);
    struct RestartResult {
        bool restarting = false; // the Restart message went out
        std::string outcome;     // restarting | build_failed | disconnected
        std::string error, output;
    };
    // Rebuilds (if `rebuild` and there is a build command), then restarts.
    RestartResult hot_restart(const std::string& reason, bool rebuild = true);
    // Controllers' requests and auto-reload being switched.
    void serve_requests();
    void defer(const std::vector<std::filesystem::path>& files); // auto-reload off: remember for later
    std::vector<std::filesystem::path> rescan();                // tracked files whose mtime or size changed
    bool relevant(const std::filesystem::path& file) const;     // part of this program's build
    void publish(const std::shared_ptr<PatchReport>& report);
    void sync_info();                                           // pending/failed files into the snapshot
    std::vector<uint64_t> resolve_external(const std::vector<std::string>& names);
    std::optional<proto::PatchResult> wait_for_result(uint32_t id, const std::shared_ptr<PatchReport>& report);
    // Prints a line and logs it as an event of `kind` (no event if nullptr).
    void say(const std::string& line, const char* kind = "log") const;
    void unavailable(const std::string& why);

    std::unique_ptr<Stream> stream_;
    HostOptions options_;
    EventLog& events_;
    Registry& registry_;
    std::shared_ptr<SessionHandle> handle_;
    std::mutex send_mutex_;
    proto::Hello hello_;
    std::string exe_name_;

    // Hot reload state, owned by the watcher thread once started.
    std::optional<CompileDb> db_;
    std::filesystem::file_time_type db_time_{};
    void reload_compile_db_if_changed();
    // [0] is the executable. Shared, so the scan thread can work on a snapshot while
    // the watcher thread adds and drops modules (dlopen/dlclose).
    std::vector<std::shared_ptr<ModuleState>> modules_;
    std::set<std::filesystem::path> units_;              // sources of all of them
    struct FileState {
        size_t hash = 0;
        std::filesystem::file_time_type mtime{};
        uintmax_t size = 0;
    };
    std::unordered_map<std::string, FileState> files_; // every source and header looked at
    std::set<std::filesystem::path> pending_;          // changed, not patched yet (auto-reload off)
    std::set<std::filesystem::path> failed_files_;     // their newest version failed to patch
    std::vector<std::shared_ptr<SessionRequest>> current_; // reload requests being served
    std::shared_ptr<const PatchReport> last_failure_;
    bool auto_reload_seen_ = true;
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
    std::map<uint32_t, std::vector<uint64_t>> frames_; // ActiveFrames, by patch id
    std::vector<proto::ModulesChanged> module_changes_; // for the watcher thread
    std::atomic<bool> has_module_changes_{false};
    bool disconnected_ = false;
};

} // namespace hotpp::host
