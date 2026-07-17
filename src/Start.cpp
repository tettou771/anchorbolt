// =============================================================================
// anchorbolt start — kiosk mode: the venue-side supervisor
//
// Spawns the app as a child process with the ops environment injected
// (TRUSSC_MCP / TRUSSC_MCP_PORT / TRUSSC_LOG_FILE), then watches it two ways:
//   - process exit  (waitpid)            -> restart
//   - hang          (tc_get_health silence) -> SIGTERM/SIGKILL -> restart
// The app needs zero code changes; everything rides the standard MCP tools.
// =============================================================================

#include "Start.h"
#include "Sink.h"

#include <TrussC.h>
#include <impl/httplib.h>   // localhost MCP calls only (plain http)
#include <tcxCurl.h>        // fleet-server push (needs https over the tunnel)
#include <tcWebSocketClient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/file.h>
#include <fcntl.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

namespace {

struct StartOptions {
    string runPath;              // app binary (positional, required)
    vector<string> appArgs;      // passed to the app verbatim (after --)
    string cwd;                  // working dir (default: the binary's directory)
    string projectDir;           // derived: TrussC project root (git/trusscli home)
    string logDir;               // empty = platform default (~/Library/Logs/anchorbolt/<id> etc.)
    int    logKeepDays = 30;     // prune logs older than this (0 = keep forever)
    int    port        = 47777;  // TRUSSC_MCP_PORT
    bool   portExplicit = false; // --port / config given -> exact, no scanning
    int    watchdogTimeoutSec = 10;  // unresponsive this long -> restart (0 = process-exit only)
    int    graceSec    = 15;     // boot grace before the watchdog arms
    string serverUrl;            // fleet server base URL (empty = no push)
    string appId;                // id on the fleet server (default: binary name)
    string token;                // agent token (--token > ANCHORBOLT_TOKEN > tokenFile > pair file)
    string pairCode;             // --pair <code>: redeem for id+token, then persist
    string configPath;           // config file that was loaded (empty = none)
    int    thumbIntervalSec = 30;    // 0 = never push screenshots
    int    thumbWidth = 512;
    int    thumbQuality = 75;
    int    wsPort = 0;           // command channel port (0 = server port + 1)
    string wsUrl;                // explicit ws(s):// override (bypasses host:port+1
                                 // derivation — needed behind a reverse proxy /
                                 // Cloudflare tunnel that routes a PATH to the hub)
    bool   allowControl = false; // let the server relay MUTATING tools
    bool   allowUpdate = false;  // let the server trigger the update pipeline
    bool   updateCustom = false; // config overrode the pipeline (no smart skip)
    vector<SinkConfig> sinks;    // outbound notifications (config "sinks" array)
    vector<string> updateCmds = {// remote-update pipeline, run in projectDir;
        "git pull --ff-only",    // override with the config "update" array
        "trusscli update",       // (e.g. prepend "trusscli upgrade" to also
        "trusscli build"};       //  pull TrussC itself, or fetch a release)

    // Derived: health poll cadence — a third of the watchdog window, clamped.
    // The watchdog itself is wall-clock (time since the last healthy reply),
    // so HTTP timeouts don't stretch the effective window.
    int pollSec() const {
        if (watchdogTimeoutSec <= 0) return 3;  // logs/channel still need a heartbeat
        return std::clamp(watchdogTimeoutSec / 3, 1, 5);
    }
};

atomic<bool> g_stop{false};
atomic<bool> g_restartRequested{false};  // set by the remote restart command
atomic<bool> g_updateRunning{false};     // one update pipeline at a time
atomic<bool> g_updateVerify{false};      // next app run must prove the update
void onSignal(int) { g_stop = true; }

// Sleep in small slices so SIGINT stays responsive.
void sleepChecked(double sec) {
    auto until = chrono::steady_clock::now() + chrono::duration<double>(sec);
    while (!g_stop && chrono::steady_clock::now() < until) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}

// Raw JSON-RPC call, returns the "result" object (used for tools/list).
optional<Json> callRpc(httplib::Client& cli, const string& method) {
    Json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", method},
                {"params", Json::object()}};
    auto res = cli.Post("/mcp", req.dump(), "application/json");
    if (!res || res->status != 200) return nullopt;
    try {
        return Json::parse(res->body).at("result");
    } catch (...) {
        return nullopt;
    }
}

// JSON-RPC tools/call against the app's local MCP endpoint.
// nullopt = transport failure or malformed reply (both count as a miss).
// Content blocks are folded back into one object: image tools return an MCP
// image block + a text metadata block; those merge into the familiar
// {data, mimeType, width, height} shape, so consumers don't care.
optional<Json> callTool(httplib::Client& cli, const string& name,
                        const Json& arguments = Json::object()) {
    Json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "tools/call"},
                {"params", {{"name", name}, {"arguments", arguments}}}};
    auto res = cli.Post("/mcp", req.dump(), "application/json");
    if (!res || res->status != 200) return nullopt;
    try {
        Json content = Json::parse(res->body).at("result").at("content");
        Json out = Json::object();
        bool haveImage = false, haveText = false;
        for (auto& block : content) {
            string type = block.value("type", "");
            if (type == "image") {
                out["data"] = block.at("data");
                out["mimeType"] = block.value("mimeType", "image/jpeg");
                haveImage = true;
            } else if (type == "text" && !haveText) {
                haveText = true;
                try {
                    Json t = Json::parse(block.at("text").get<string>());
                    if (t.is_object()) out.update(t);
                    else if (!haveImage) return t;
                } catch (...) {
                    if (!haveImage) return nullopt;
                }
            }
        }
        if (!haveImage && !haveText) return nullopt;
        return out;
    } catch (...) {
        return nullopt;
    }
}

// Decode standard base64 (tc_get_screenshot ships its images this way; core has
// toBase64 but no decoder yet). Skips padding and whitespace.
vector<unsigned char> fromBase64(const string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// nlohmann::json::dump() THROWS on invalid UTF-8 (type_error.316), and log
// lines are not under our control: a localized MSVC streams CP932 build
// output into the supervisor log during a remote update, which crashed the
// whole supervisor (uncaught -> std::terminate, 0xC0000409) when LogShipper
// shipped those lines — and kept crashing on every rerun until the
// "poisoned" file aged out. Every OUTBOUND dump goes through this: invalid
// sequences become U+FFFD instead of a crash. (Found on a JP-locale Windows
// box; clang emits UTF-8 so mac/linux never hit it.)
string dumpSafe(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// Machine-wide memory, the OS's view. Rides every heartbeat and gets
// snapshotted into the event log on failures — so an operator can tell
// "our app leaked" from "something else ate the machine" (OOM exoneration).
struct MachineMem {
    int64_t totalBytes = 0;
    int64_t availBytes = 0;
};

MachineMem machineMem() {
    MachineMem m;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.totalBytes = (int64_t)ms.ullTotalPhys;
        m.availBytes = (int64_t)ms.ullAvailPhys;
    }
#elif defined(__APPLE__)
    int64_t total = 0;
    size_t len = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) == 0) m.totalBytes = total;
    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        m.availBytes = (int64_t)(vm.free_count + vm.inactive_count) * (int64_t)pageSize;
    }
#elif !defined(_WIN32)
    ifstream in("/proc/meminfo");
    string line;
    while (getline(in, line)) {
        long kb = 0;
        if (sscanf(line.c_str(), "MemTotal: %ld", &kb) == 1) m.totalBytes = (int64_t)kb * 1024;
        else if (sscanf(line.c_str(), "MemAvailable: %ld", &kb) == 1) m.availBytes = (int64_t)kb * 1024;
    }
#endif
    return m;
}

string fmtMB(int64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0fMB", bytes / 1048576.0);
    return buf;
}

// Supervisor event log: one line per lifecycle event, daily files next to the
// app logs. This is what you read the morning after — when/why restarts
// happened, with the machine's memory state at each failure.
void eventLog(const fs::path& logDir, const string& msg) {
    ofstream out(logDir / ("anchorbolt-" + getTimestampString("%Y-%m-%d") + ".log"), ios::app);
    out << "[" << getTimestampString("%H:%M:%S") << "] " << msg << "\n";
}

string machineMemNote() {
    MachineMem m = machineMem();
    return "machine mem avail " + fmtMB(m.availBytes) + " / " + fmtMB(m.totalBytes);
}

// --- remote update -------------------------------------------------------------
// Dashboard-triggered deploy: run a pipeline (default git pull -> trusscli
// update -> trusscli build) in the project directory WHILE the old binary
// keeps running, and restart onto the new binary only if every step succeeds
// — a failed build never takes the installation down. The previous binary is
// kept as <binary>.prev; if the new one doesn't prove itself on its first
// run, the supervision loop rolls back automatically.

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

// Quote a path for the shell popen() runs commands in: cmd.exe wants double
// quotes, /bin/sh gets the bulletproof single-quote form.
string quotePath(const string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

// Current commit of the project (short hash), for the fleet dashboard —
// "which venue runs which version" at a glance. Empty if not a git checkout.
string gitHash(const string& projectDir) {
    if (projectDir.empty()) return "";
#ifdef _WIN32
    string cmd = "git -C " + quotePath(projectDir) + " rev-parse --short HEAD 2>nul";
#else
    string cmd = "git -C " + quotePath(projectDir) + " rev-parse --short HEAD 2>/dev/null";
#endif
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "";
    char buf[64] = {};
    string out;
    if (fgets(buf, sizeof(buf), f)) out = buf;
    pclose(f);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return out;
}

// The directory git/trusscli operate in: the ancestor above bin/ (trusscli
// project layout), falling back to the app's working directory.
string deriveProjectDir(const fs::path& runPath, const string& cwd) {
    for (fs::path p = runPath.parent_path(); !p.empty() && p != p.root_path();
         p = p.parent_path()) {
        if (p.filename() == "bin") return p.parent_path().string();
    }
    return cwd;
}

// A pipeline that failed remembers the commit it failed AT: a retry on the
// same commit must not be short-circuited by the "already up to date" skip,
// or a transient build failure (disk full etc.) could never be retried.
// Single writer: only one update thread runs at a time (g_updateRunning).
string g_updateFailedAt;

// Runs detached: pipeline output streams into the supervisor event log line
// by line (prefixed "[update]"), which the log push forwards live — the
// dashboard's log panel doubles as the build console.
void startUpdateJob(const StartOptions& opt, const fs::path& logDir) {
    if (g_updateRunning.exchange(true)) return;
    thread([opt, logDir]() {
#ifdef _WIN32
        _putenv_s("GIT_TERMINAL_PROMPT", "0");  // never hang on a credential prompt
#else
        setenv("GIT_TERMINAL_PROMPT", "0", 1);  // never hang on a credential prompt
#endif
        eventLog(logDir, "[update] started (" + to_string(opt.updateCmds.size()) +
                         " steps in " + opt.projectDir + ")");
        string gitBefore = gitHash(opt.projectDir);
        // Keep the running binary for rollback BEFORE the build replaces it.
        fs::path prev = opt.runPath + ".prev";
        error_code ec;
#ifdef _WIN32
        // Windows write-locks a running exe but allows RENAME: moving the
        // live binary aside both frees the path for the linker and doubles
        // as the rollback backup. The running process keeps its image.
        fs::remove(prev, ec);
        ec.clear();
        fs::rename(opt.runPath, prev, ec);
        // Any exit below that leaves no new binary at runPath must put the
        // live one back, or the next (re)spawn finds nothing to launch.
        auto restoreIfMissing = [&]() {
            error_code e2;
            if (!fs::exists(opt.runPath, e2)) fs::rename(prev, opt.runPath, e2);
        };
#else
        fs::copy_file(opt.runPath, prev, fs::copy_options::overwrite_existing, ec);
        auto restoreIfMissing = []() {};
#endif
        if (ec) eventLog(logDir, "[update] warning: binary backup failed: " + ec.message());

        bool ok = true;
        for (size_t i = 0; i < opt.updateCmds.size(); ++i) {
            const string& cmd = opt.updateCmds[i];
            eventLog(logDir, "[update] $ " + cmd);
#ifdef _WIN32
            string full = "cd /d " + quotePath(opt.projectDir) + " && (" + cmd + ") 2>&1";
#else
            string full = "cd " + quotePath(opt.projectDir) + " && (" + cmd + ") 2>&1";
#endif
            FILE* f = popen(full.c_str(), "r");
            if (!f) {
                eventLog(logDir, "[update] FAILED to launch: " + cmd);
                ok = false;
                break;
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), f)) {
                string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                if (!line.empty()) eventLog(logDir, "[update] " + line);
            }
            int st = pclose(f);
#ifdef _WIN32
            int code = st;
#else
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
            if (code != 0) {
                string fmsg = "update FAILED (exit " + to_string(code) + "): " + cmd +
                              " — app keeps running on the old binary";
                eventLog(logDir, "[update] " + fmsg);
                if (auto n = g_notifier) n->notify("update", fmsg);
                ok = false;
                break;
            }
            // Smart skip, DEFAULT pipeline only (there step 1 IS the pull; a
            // custom pipeline makes no such promise, so it always runs fully
            // and restarts): if the pull brought nothing new, stop here —
            // no regeneration, no rebuild, no restart. Pressing Update "just
            // in case" costs seconds and touches nothing.
            if (i == 0 && !opt.updateCustom) {
                string gitAfter = gitHash(opt.projectDir);
                if (!gitAfter.empty() && gitAfter == gitBefore && gitAfter != g_updateFailedAt) {
                    eventLog(logDir, "[update] already up to date (" + gitAfter +
                                     "); nothing to do, app keeps running");
                    restoreIfMissing();
                    g_updateRunning = false;
                    return;
                }
            }
        }
        if (ok) {
            error_code e2;
            if (!fs::exists(opt.runPath, e2)) {
                // A "successful" pipeline that produced no binary (custom
                // pipeline oddity): nothing to restart onto.
                eventLog(logDir, "[update] pipeline produced no binary at " + opt.runPath +
                                 "; app keeps running");
                restoreIfMissing();
                ok = false;
            }
        }
        if (ok) {
            g_updateFailedAt.clear();
            eventLog(logDir, "[update] pipeline succeeded; restarting onto the new binary");
            g_updateVerify = true;
            g_restartRequested = true;
        } else {
            g_updateFailedAt = gitHash(opt.projectDir);
            restoreIfMissing();
        }
        g_updateRunning = false;
    }).detach();
}

// --- log shipping (delivery cursor) ------------------------------------------
// The local daily log files ARE the offline spool. A persisted cursor marks
// how far the server has CONFIRMED receipt; it advances only on a 200, so
// lines produced while the server was unreachable ship on reconnect — even
// across anchorbolt or machine restarts. Semantics: at-least-once. The batch
// in flight is frozen (a retry resends it byte-identical) and the server
// dedups by (file, end offset), so a lost ack costs a duplicate, never a gap.

struct LogBatch {
    string file;                 // daily file name, e.g. "app-2026-07-16.log"
    int64_t start = 0, end = 0;  // byte range covered: [start, end)
    vector<string> lines;
};

// Best-effort push to the fleet server. Failures never affect supervision;
// reachability changes are logged once, not per attempt.
class ServerPush {
public:
    ServerPush(const string& url, const string& appId, const string& token)
        : appId_(appId) {
        // tcxCurl (libcurl + native TLS) so the push works over an https tunnel;
        // httplib here is built without OpenSSL and rejects https:// outright.
        cli_.setBaseUrl(url);
        cli_.setTimeout(2);
        if (!token.empty()) cli_.setBearerToken(token);
    }

    void heartbeat(Json health) {
        health["app"] = appId_;
        auto res = cli_.request("POST", "/api/heartbeat", dumpSafe(health), "application/json");
        report(res.statusCode == 200, "heartbeat");
    }

    void thumbnail(const vector<unsigned char>& jpg) {
        auto res = cli_.request("POST", "/api/thumb/" + appId_,
                                string((const char*)jpg.data(), jpg.size()), "image/jpeg");
        report(res.statusCode == 200, "thumbnail");
    }

    void image(const string& name, const vector<unsigned char>& jpg) {
        auto res = cli_.request("POST", "/api/image/" + appId_ + "/" + name,
                                string((const char*)jpg.data(), jpg.size()), "image/jpeg");
        report(res.statusCode == 200, "image");
    }

    // Supervisor/app event for the dashboard (badge + event list). Best-effort
    // one-shot: the same information also reaches the server through the
    // cursor-spooled logs, so a miss here is cosmetic.
    void alert(const string& event, const string& text) {
        Json body = {{"alerts", Json::array({Json{
            {"at", getTimestampString("%Y-%m-%dT%H:%M:%S")},
            {"event", event},
            {"text", text}}})}};
        auto res = cli_.request("POST", "/api/alert/" + appId_, dumpSafe(body), "application/json");
        report(res.statusCode == 200, "alert");
    }

    // src is "app" (the app's TRUSSC_LOG_FILE) or "sup" (supervisor events).
    // Returns delivery success — the shipper's cursor advances only on true.
    bool logs(const string& src, const LogBatch& b) {
        Json body = {{"src", src}, {"file", b.file},
                     {"start", b.start}, {"end", b.end}, {"lines", b.lines}};
        auto res = cli_.request("POST", "/api/log/" + appId_, dumpSafe(body), "application/json");
        bool ok = res.statusCode == 200;
        report(ok, "log");
        return ok;
    }

private:
    void report(bool success, const char* what) {
        if (success && !reachable_) {
            reachable_ = true;
            logNotice("anchorbolt") << "fleet server reachable again";
        } else if (!success && reachable_) {
            reachable_ = false;
            logWarning("anchorbolt") << "fleet server unreachable (" << what << " failed)";
        }
    }

    tcx::curl::HttpClient cli_;
    string appId_;
    bool reachable_ = true;  // assume up so the first failure logs
};

// One log source (prefix "app-" or "anchorbolt-"): reads complete lines from
// the daily files in cursor order, rolling across days, and ships them via
// ServerPush. Cursor persistence is the caller's job (lazily, so SD cards
// aren't worn down by a write every cycle).
class LogShipper {
public:
    LogShipper(string src, string prefix) : src_(std::move(src)), prefix_(std::move(prefix)) {}

    // No saved cursor: anchor at the current end of the active file — history
    // from before anchorbolt ever pushed is not uploaded. A saved cursor
    // resumes exactly, including a previous session's unsent tail.
    void init(const Json& saved, const fs::path& activeFile) {
        if (saved.is_object() && saved.contains("file")) {
            file_ = saved.value("file", "");
            offset_ = saved.value("offset", (int64_t)0);
        } else {
            error_code ec;
            file_ = activeFile.filename().string();
            offset_ = (int64_t)fs::file_size(activeFile, ec);
            if (ec) offset_ = 0;
            dirty_ = true;
        }
    }

    // Ship pending lines, bounded per call so a big backlog can't stall the
    // supervision loop. Transport failure keeps the batch for the next cycle.
    void ship(ServerPush& push, const fs::path& dir, const fs::path& activeFile) {
        for (int i = 0; i < kMaxBatchesPerCycle; ++i) {
            if (!inflight_) inflight_ = nextBatch(dir, activeFile.filename().string());
            if (!inflight_) return;                    // drained
            if (!push.logs(src_, *inflight_)) return;  // unreachable; retry as-is later
            offset_ = inflight_->end;                  // confirmed — advance the cursor
            dirty_ = true;
            inflight_.reset();
        }
    }

    Json cursorJson() const { return {{"file", file_}, {"offset", offset_}}; }
    const string& file() const { return file_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    static constexpr int     kMaxBatchesPerCycle = 20;
    static constexpr int64_t kMaxBatchBytes      = 256 * 1024;
    static constexpr size_t  kMaxBatchLines      = 500;

    // Move the cursor to the oldest of our daily files newer than it (the
    // date-stamped names sort chronologically). Pure bookkeeping: called only
    // once every byte of the current file has been confirmed.
    bool advanceFile(const fs::path& dir, const string& active) {
        string best;
        error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            string n = e.path().filename().string();
            if (n.rfind(prefix_, 0) != 0 || n.size() < 4 ||
                n.compare(n.size() - 4, 4, ".log") != 0) continue;
            if (n <= file_) continue;
            if (best.empty() || n < best) best = n;
        }
        if (best.empty()) {
            if (active <= file_) return false;  // nothing newer yet
            best = active;                      // active not on disk yet — jump
        }
        file_ = best;
        offset_ = 0;
        dirty_ = true;
        return true;
    }

    optional<LogBatch> nextBatch(const fs::path& dir, const string& active) {
        for (int hop = 0; hop < 64; ++hop) {  // bounded day-file rolls per call
            if (file_.empty()) return nullopt;
            bool isActive = (file_ == active);
            error_code ec;
            int64_t size = (int64_t)fs::file_size(dir / file_, ec);
            if (ec) {                          // pruned or deleted underneath us
                if (isActive || !advanceFile(dir, active)) return nullopt;
                continue;
            }
            if (offset_ > size) { offset_ = 0; dirty_ = true; }  // truncated/replaced
            if (offset_ == size) {
                if (isActive || !advanceFile(dir, active)) return nullopt;
                continue;
            }
            ifstream in(dir / file_, ios::binary);
            if (!in) return nullopt;
            string chunk((size_t)min(size - offset_, kMaxBatchBytes), '\0');
            in.seekg(offset_);
            in.read(chunk.data(), (streamsize)chunk.size());
            chunk.resize((size_t)max<streamsize>(in.gcount(), 0));

            LogBatch b;
            b.file = file_;
            b.start = offset_;
            size_t consumed = 0, pos = 0;
            while (b.lines.size() < kMaxBatchLines) {
                size_t nl = chunk.find('\n', pos);
                if (nl == string::npos) break;
                if (nl > pos) b.lines.emplace_back(chunk, pos, nl - pos);
                pos = nl + 1;
                consumed = pos;
            }
            bool sawEof = offset_ + (int64_t)chunk.size() == size;
            if (!isActive && sawEof && consumed < chunk.size() &&
                b.lines.size() < kMaxBatchLines) {
                // A finished file's trailing partial line will never gain its
                // newline — ship it as-is instead of stranding it.
                b.lines.emplace_back(chunk, consumed, chunk.size() - consumed);
                consumed = chunk.size();
            }
            if (b.lines.empty() && consumed == 0 &&
                (int64_t)chunk.size() == kMaxBatchBytes) {
                // A single line larger than a whole batch: ship the raw slab
                // rather than stall the cursor forever.
                b.lines.emplace_back(chunk);
                consumed = chunk.size();
            }
            if (b.lines.empty()) {
                if (consumed > 0) {  // blank lines only — nothing to claim, skip
                    offset_ += (int64_t)consumed;
                    dirty_ = true;
                    continue;
                }
                return nullopt;      // partial line on the active file; wait
            }
            b.end = b.start + (int64_t)consumed;
            return b;
        }
        return nullopt;
    }

    string src_, prefix_;
    string file_;          // cursor: daily file name...
    int64_t offset_ = 0;   // ...and confirmed-delivered bytes within it
    bool dirty_ = false;   // cursor changed since the last save
    optional<LogBatch> inflight_;  // frozen until confirmed (exact resend)
};

// The agent's end of the command channel: one outbound WebSocket to the
// server (NAT-friendly). Commands arrive on the socket thread; restart is
// handed to the supervision loop via g_restartRequested, tool calls are
// relayed to the app's MCP endpoint directly from here.
class CommandChannel {
public:
    CommandChannel(const StartOptions& opt, fs::path logDir)
        : opt_(opt), logDir_(std::move(logDir)) {
        if (!opt.wsUrl.empty()) {
            // Explicit override, only needed for a proxy that routes the hub to
            // a non-standard path (something other than the /ws convention below).
            url_ = opt.wsUrl;
        } else {
            // Derive the hub URL from --server. Strip scheme + path, keep host
            // and any explicit :port.
            string url = opt.serverUrl;
            bool tls = url.rfind("https://", 0) == 0;
            if (tls) url = url.substr(8);
            else if (url.rfind("http://", 0) == 0) url = url.substr(7);
            size_t slash = url.find('/');
            if (slash != string::npos) url = url.substr(0, slash);
            size_t colon = url.find(':');
            string host = (colon == string::npos) ? url : url.substr(0, colon);

            if (tls && opt.wsPort == 0) {
                // An https --server means a TLS-terminating proxy / tunnel sits
                // in front, routing by PATH on 443 — a separate port+1 isn't
                // exposed. Convention: the hub is at /ws on the same host (see
                // the cloudflared ingress in the docs). No per-venue flag needed.
                url_ = "wss://" + host + "/ws";
            } else {
                // Plain LAN (http), or an explicit --ws-port: the hub is reached
                // directly at http-port + 1.
                int httpPort = tls ? 443 : 80;
                if (colon != string::npos) httpPort = stoi(url.substr(colon + 1));
                int wsPort = opt.wsPort ? opt.wsPort : httpPort + 1;
                url_ = (tls ? "wss://" : "ws://") + host + ":" + to_string(wsPort) + "/";
            }
        }

        openL_ = ws_.onOpen.listen([this]() {
            wsSend(Json({{"type", "hello"},
                         {"app", opt_.appId},
                         {"token", opt_.token}}).dump());
            logNotice("anchorbolt") << "command channel connected (" << url_ << ")";
        });
        msgL_ = ws_.onMessage.listen([this](tcx::websocket::WebSocketEventArgs& e) {
            handleMessage(e.message);
        });
        closeL_ = ws_.onClose.listen([this]() {
            logWarning("anchorbolt") << "command channel disconnected";
        });
    }

    // Called each poll cycle: (re)connect when down. connect() is async.
    void maintain() {
        if (ws_.getState() == tcx::websocket::WebSocketClient::State::Disconnected) {
            ws_.connect(url_);
        }
    }

    void shutdown() {
        stopLive("shutdown");
        ws_.disconnect();
    }

    // The live thread outlives individual commands; make sure it is joined
    // before the WebSocket and options it touches are torn down.
    ~CommandChannel() {
        liveRunning_ = false;
        if (liveThread_.joinable()) liveThread_.join();
    }

private:
    // All frame/reply/hello sends funnel through here: the live thread and the
    // WS receive thread can both write concurrently, and WebSocketClient::send
    // is not internally serialized. dumpSafe'd JSON only (invalid UTF-8 in a
    // reply string would otherwise throw).
    void wsSend(const string& text) {
        lock_guard<mutex> lk(sendMutex_);
        ws_.send(text);
    }

    static int64_t nowNs() {
        return chrono::duration_cast<chrono::nanoseconds>(
                   chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Spin up the live screenshot streamer (idempotent). A repeat live_start
    // while already streaming only refreshes params + the keepalive stamp —
    // the server re-sends live_start every few seconds as a heartbeat.
    void startLive() {
        if (liveRunning_.exchange(true)) return;   // already streaming
        if (liveThread_.joinable()) liveThread_.join();  // reap a finished run
        eventLog(logDir_, "live stream started (fps " + to_string(liveFps_.load()) +
                          ", width " + to_string(liveWidth_.load()) +
                          ", quality " + to_string(liveQuality_.load()) + ")");
        logNotice("anchorbolt") << "live stream started";
        liveThread_ = thread([this]() { liveLoop(); });
    }

    // Stop streaming and join. Safe to call when not streaming (reaps a thread
    // that already self-exited on idle without double-logging the stop).
    void stopLive(const string& reason) {
        bool was = liveRunning_.exchange(false);
        if (liveThread_.joinable()) liveThread_.join();
        if (was) {
            eventLog(logDir_, "live stream stopped (" + reason + ")");
            logNotice("anchorbolt") << "live stream stopped (" << reason << ")";
        }
    }

    // Poll the app's screenshot tool at the requested fps on a dedicated
    // connection and push each JPEG frame over the command WebSocket. Base64
    // TEXT frames, not binary: WsHub (serve) only decodes text, and
    // WebSocketClient::send(vector) actually emits a TEXT opcode regardless —
    // so raw binary would arrive mislabeled. The screenshot tool already hands
    // us base64, so we forward it verbatim (no decode/re-encode). Failed polls
    // (app restarting/dead) just skip a frame; this never touches the
    // supervision loop's client.
    void liveLoop() {
        httplib::Client cli("localhost", opt_.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(3, 0);
        bool idle = false;
        while (liveRunning_ && !g_stop) {
            auto frameStart = chrono::steady_clock::now();
            // Auto-stop: the browser's polling keeps live_start flowing; 10s of
            // silence means nobody is watching.
            if (nowNs() - liveKeepaliveNs_.load() > 10LL * 1000 * 1000 * 1000) {
                idle = true;
                break;
            }
            int fps = std::clamp(liveFps_.load(), 1, 15);
            Json args = {{"format", "jpg"},
                         {"width", liveWidth_.load()},
                         {"quality", liveQuality_.load()}};
            if (auto t = callTool(cli, "tc_get_screenshot", args)) {
                if (t->contains("data") && (*t)["data"].is_string()) {
                    wsSend(dumpSafe(Json{{"type", "frame"}, {"data", (*t)["data"]}}));
                }
            }
            // Pace to the target fps, sleeping in slices so stop stays snappy.
            auto until = frameStart + chrono::duration_cast<chrono::steady_clock::duration>(
                                          chrono::duration<double>(1.0 / fps));
            while (liveRunning_ && !g_stop && chrono::steady_clock::now() < until) {
                this_thread::sleep_for(chrono::milliseconds(10));
            }
        }
        liveRunning_ = false;
        if (idle) {
            eventLog(logDir_, "live stream stopped (idle timeout)");
            logNotice("anchorbolt") << "live stream stopped (idle timeout)";
        }
    }

    void handleMessage(const string& text) {
        Json m;
        try {
            m = Json::parse(text);
        } catch (...) {
            return;
        }
        if (m.value("type", "") == "error") {
            logError("anchorbolt") << "server rejected command channel: " << m.value("error", "");
            return;
        }
        if (m.value("type", "") != "cmd") return;
        uint64_t id = m.value("id", (uint64_t)0);
        string action = m.value("action", "");
        Json reply = {{"type", "result"}, {"id", id}};

        if (action == "restart") {
            g_restartRequested = true;
            reply["ok"] = true;
            reply["result"] = {{"message", "restart initiated"}};
        } else if (action == "update") {
            // Remote code execution by definition — the venue operator's
            // explicit --allow-update is the gate, like --allow-control.
            if (!opt_.allowUpdate) {
                reply["ok"] = false;
                reply["error"] = "blocked by supervisor: start with --allow-update to permit remote updates";
            } else if (g_updateRunning) {
                reply["ok"] = false;
                reply["error"] = "an update is already running";
            } else {
                startUpdateJob(opt_, logDir_);
                reply["ok"] = true;
                reply["result"] = {{"message",
                    "update started — progress streams into the log; the app restarts only if the build succeeds"}};
            }
        } else if (action == "rollback") {
            fs::path prev = opt_.runPath + ".prev";
            error_code ec;
            if (!opt_.allowUpdate) {
                reply["ok"] = false;
                reply["error"] = "blocked by supervisor: start with --allow-update to permit rollback";
            } else if (g_updateRunning) {
                reply["ok"] = false;
                reply["error"] = "an update is running; wait for it to finish";
            } else if (!fs::exists(prev, ec)) {
                reply["ok"] = false;
                reply["error"] = "no previous binary kept (no update has run here)";
            } else {
#ifdef _WIN32
                // The running exe is write-locked but renameable: swap by
                // moving it aside instead of copying over it.
                error_code e2;
                fs::remove(opt_.runPath + ".old", e2);
                fs::rename(opt_.runPath, opt_.runPath + ".old", ec);
                if (!ec) fs::rename(prev, opt_.runPath, ec);
#else
                fs::copy_file(prev, opt_.runPath, fs::copy_options::overwrite_existing, ec);
#endif
                if (ec) {
                    reply["ok"] = false;
                    reply["error"] = "restoring previous binary failed: " + ec.message();
                } else {
                    eventLog(logDir_, "[update] manual rollback: previous binary restored; restarting");
                    g_restartRequested = true;
                    reply["ok"] = true;
                    reply["result"] = {{"message", "previous binary restored; restarting"}};
                }
            }
        } else if (action == "list_tools" || action == "call") {
            // Fresh client per command: this runs on the WS thread, never
            // share the supervision loop's connection.
            httplib::Client cli("localhost", opt_.port);
            cli.set_connection_timeout(2, 0);
            cli.set_read_timeout(10, 0);
            if (action == "list_tools") {
                if (auto r = callRpc(cli, "tools/list")) {
                    reply["ok"] = true;
                    reply["result"] = *r;
                } else {
                    reply["ok"] = false;
                    reply["error"] = "app did not answer tools/list";
                }
            } else {
                string tool = m.value("tool", "");
                // Read-only tools relay freely; anything that can mutate the
                // app (input injection, node writes, custom tools, tc_quit)
                // requires the venue operator's explicit --allow-control.
                bool readOnly = tool.rfind("tc_get_", 0) == 0;
                if (!readOnly && !opt_.allowControl) {
                    reply["ok"] = false;
                    reply["error"] = "blocked by supervisor: '" + tool +
                                     "' is not read-only (start with --allow-control to permit)";
                } else if (auto r = callTool(cli, tool, m.value("args", Json::object()))) {
                    reply["ok"] = true;
                    reply["result"] = *r;
                } else {
                    reply["ok"] = false;
                    reply["error"] = "tool call failed or app unresponsive";
                }
            }
        } else if (action == "live_start") {
            // Keepalive + params for the live screenshot stream. Read-only
            // (screenshots only), so it never needs --allow-control.
            liveFps_     = std::clamp(m.value("fps", 10), 1, 15);
            liveWidth_   = std::clamp(m.value("width", 960), 320, 1920);
            liveQuality_ = std::clamp(m.value("quality", 60), 20, 90);
            liveKeepaliveNs_ = nowNs();
            startLive();
            reply["ok"] = true;
            reply["result"] = {{"message", "live streaming"}};
        } else if (action == "live_stop") {
            stopLive("stopped by request");
            reply["ok"] = true;
            reply["result"] = {{"message", "live stopped"}};
        } else {
            reply["ok"] = false;
            reply["error"] = "unknown action";
        }
        wsSend(dumpSafe(reply));
    }

    StartOptions opt_;
    fs::path logDir_;
    string url_;
    tcx::websocket::WebSocketClient ws_;
    EventListener openL_, msgL_, closeL_;

    // Live view: a dedicated screenshot streamer over the same WebSocket.
    mutex sendMutex_;                    // serializes all ws_.send() callers
    thread liveThread_;
    atomic<bool> liveRunning_{false};
    atomic<int> liveFps_{10}, liveWidth_{960}, liveQuality_{60};
    atomic<int64_t> liveKeepaliveNs_{0}; // last live_start; drives the 10s auto-stop
};

bool parseArgs(const vector<string>& args, StartOptions& opt) {
    for (size_t i = 0; i < args.size(); ++i) {
        const string& a = args[i];
        auto next = [&](const char* flag) -> optional<string> {
            if (i + 1 >= args.size()) {
                cerr << "anchorbolt agent: " << flag << " needs a value" << endl;
                return nullopt;
            }
            return args[++i];
        };
        if      (a == "--cwd")      { auto v = next("--cwd");      if (!v) return false; opt.cwd = *v; }
        else if (a == "--log-dir")  { auto v = next("--log-dir");  if (!v) return false; opt.logDir = *v; }
        else if (a == "--port")     { auto v = next("--port");     if (!v) return false; opt.port = stoi(*v); opt.portExplicit = true; }
        else if (a == "--grace")    { auto v = next("--grace");    if (!v) return false; opt.graceSec = stoi(*v); }
        else if (a == "--watchdog-timeout") { auto v = next("--watchdog-timeout"); if (!v) return false; opt.watchdogTimeoutSec = stoi(*v); }
        else if (a == "--log-keep") { auto v = next("--log-keep"); if (!v) return false; opt.logKeepDays = stoi(*v); }
        else if (a == "--server")   { auto v = next("--server");   if (!v) return false; opt.serverUrl = *v; }
        else if (a == "--id")       { auto v = next("--id");       if (!v) return false; opt.appId = *v; }
        else if (a == "--token")    { auto v = next("--token");    if (!v) return false; opt.token = *v; }
        else if (a == "--token-file") {
            auto v = next("--token-file");
            if (!v) return false;
            ifstream tin(*v);
            string tok;
            if (!tin || !getline(tin, tok)) {
                cerr << "anchorbolt start: cannot read token file " << *v << endl;
                return false;
            }
            while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
            opt.token = tok;
        }
        else if (a == "--pair")     { auto v = next("--pair");     if (!v) return false; opt.pairCode = *v; }
        else if (a == "--ws-port")  { auto v = next("--ws-port");  if (!v) return false; opt.wsPort = stoi(*v); }
        else if (a == "--ws-url")   { auto v = next("--ws-url");   if (!v) return false; opt.wsUrl = *v; }
        else if (a == "--allow-control") { opt.allowControl = true; }
        else if (a == "--allow-update")  { opt.allowUpdate = true; }
        else if (a == "--thumb-interval") { auto v = next("--thumb-interval"); if (!v) return false; opt.thumbIntervalSec = stoi(*v); }
        else if (a == "--thumb-width")    { auto v = next("--thumb-width");    if (!v) return false; opt.thumbWidth = stoi(*v); }
        else if (a == "--thumb-quality")  { auto v = next("--thumb-quality");  if (!v) return false; opt.thumbQuality = stoi(*v); }
        else if (a == "--config") { ++i; /* consumed in the pre-pass */ }
        else if (a == "-p" || a == "--path") { auto v = next("-p"); if (!v) return false; opt.runPath = *v; }
        else if (a == "--") {
            // Everything after -- goes to the app verbatim.
            opt.appArgs.assign(args.begin() + i + 1, args.end());
            break;
        }
        else {
            cerr << "anchorbolt start: unexpected argument '" << a
                 << "' (use -p <path> for the app, app arguments go after --)" << endl;
            return false;
        }
    }
    return true;
}

// Resolve what the user pointed at — a binary, a .app bundle, or a TrussC
// project directory (mirrors trusscli's layout: bin/<name>.app on macOS,
// bin/<name> elsewhere). Empty input means the current directory, so a bare
// `anchorbolt start` inside a project just works. `tried` collects the
// locations probed, for the error message.
fs::path resolveApp(fs::path input, vector<string>& tried) {
    error_code ec;
    if (input.empty()) input = fs::current_path(ec);
    input = fs::absolute(input, ec);

    if (fs::is_regular_file(input, ec)) return input;

    auto bundleBinary = [&](const fs::path& app) -> fs::path {
        fs::path inner = app / "Contents" / "MacOS" / app.stem();
        tried.push_back(inner.string());
        error_code e2;
        return fs::is_regular_file(inner, e2) ? inner : fs::path{};
    };

    if (input.extension() == ".app" && fs::is_directory(input, ec)) {
        return bundleBinary(input);
    }
    if (fs::is_directory(input, ec)) {
        string name = input.filename().string();
        if (fs::path p = bundleBinary(input / "bin" / (name + ".app")); !p.empty()) return p;
#ifdef _WIN32
        fs::path exe = input / "bin" / (name + ".exe");
        tried.push_back(exe.string());
        if (fs::is_regular_file(exe, ec)) return exe;
#endif
        fs::path flat = input / "bin" / name;
        tried.push_back(flat.string());
        if (fs::is_regular_file(flat, ec)) return flat;
        // Last resort: exactly one .app in bin/ (project dir renamed etc.)
        fs::path onlyApp;
        for (auto& e : fs::directory_iterator(input / "bin", ec)) {
            if (e.path().extension() == ".app" && fs::is_directory(e.path(), ec)) {
                if (!onlyApp.empty()) { onlyApp.clear(); break; }  // ambiguous
                onlyApp = e.path();
            }
        }
        if (!onlyApp.empty()) {
            if (fs::path p = bundleBinary(onlyApp); !p.empty()) return p;
        }
    } else {
        tried.push_back(input.string());
    }
    return {};
}

// Config file: JSON with comments allowed. Loaded BEFORE flags, so flags win.
// The token itself is deliberately NOT a config key — configs get committed
// to git; secrets live in `tokenFile` (a path) or the ANCHORBOLT_TOKEN env.
bool loadConfig(const fs::path& path, StartOptions& opt) {
    ifstream in(path);
    if (!in) {
        cerr << "anchorbolt start: cannot read config " << path.string() << endl;
        return false;
    }
    Json c;
    try {
        c = Json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        cerr << "anchorbolt start: config parse error in " << path.string()
             << ": " << e.what() << endl;
        return false;
    }
    if (c.contains("token")) {
        cerr << "anchorbolt start: config contains a 'token' key — refusing to read it.\n"
             << "Configs end up in git; put the token in a separate file and point\n"
             << "'tokenFile' at it (or use the ANCHORBOLT_TOKEN env var)." << endl;
    }
    if (c.contains("app")) {
        // Relative app paths resolve next to the config file, not our cwd.
        fs::path a = c["app"].get<string>();
        opt.runPath = (a.is_absolute() ? a : path.parent_path() / a).string();
    }
    if (c.contains("args") && c["args"].is_array()) {
        opt.appArgs.clear();
        for (auto& a : c["args"]) opt.appArgs.push_back(a.get<string>());
    }
    opt.cwd       = c.value("cwd", opt.cwd);
    opt.appId     = c.value("id", opt.appId);
    opt.serverUrl = c.value("server", opt.serverUrl);
    if (c.contains("port")) { opt.port = c["port"].get<int>(); opt.portExplicit = true; }
    opt.wsPort    = c.value("wsPort", opt.wsPort);
    opt.wsUrl     = c.value("wsUrl", opt.wsUrl);
    opt.allowControl = c.value("allowControl", opt.allowControl);
    opt.allowUpdate  = c.value("allowUpdate", opt.allowUpdate);
    if (c.contains("update") && c["update"].is_array()) {
        opt.updateCmds.clear();
        for (auto& u : c["update"]) opt.updateCmds.push_back(u.get<string>());
        opt.updateCustom = true;
    }
    opt.graceSec  = c.value("grace", opt.graceSec);
    opt.watchdogTimeoutSec = c.value("watchdogTimeout", opt.watchdogTimeoutSec);
    if (c.contains("log") && c["log"].is_object()) {
        auto& l = c["log"];
        opt.logDir      = l.value("dir", opt.logDir);
        opt.logKeepDays = l.value("keepDays", opt.logKeepDays);
    }
    if (c.contains("thumb") && c["thumb"].is_object()) {
        auto& t = c["thumb"];
        opt.thumbIntervalSec = t.value("interval", opt.thumbIntervalSec);
        opt.thumbWidth       = t.value("width", opt.thumbWidth);
        opt.thumbQuality     = t.value("quality", opt.thumbQuality);
    }
    if (c.contains("sinks")) {
        opt.sinks = parseSinks(c["sinks"], path.parent_path());
    }
    if (c.contains("tokenFile")) {
        // Relative paths resolve next to the config file, not our cwd.
        fs::path tf = path.parent_path() / fs::path(c["tokenFile"].get<string>());
        ifstream tin(tf);
        string tok;
        if (tin && getline(tin, tok)) {
            while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
            opt.token = tok;
        } else {
            cerr << "anchorbolt start: cannot read tokenFile " << tf.string() << endl;
            return false;
        }
    }
    opt.configPath = fs::absolute(path).string();
    logNotice("anchorbolt") << "config loaded: " << path.string();
    return true;
}

// Where the paired identity persists: ALWAYS the platform-private state dir
// (~/Library/Logs/anchorbolt/<key>, %LOCALAPPDATA%\anchorbolt\<key>, etc.),
// never next to the config or the --log-dir — those can live inside a git
// repo, and this file holds a token (the exact leak the config 'token' ban
// guards against). The location is keyed by the BINARY NAME (stable across
// the pairing run and later restarts); the server-assigned id lives INSIDE
// the file. Read back on later (non-pair) runs as the lowest-priority token
// source, so a venue keeps its token across restarts.
fs::path platformLogDir(const string& appId);  // defined below

fs::path pairTokenFile(const fs::path& runPath) {
    string key = runPath.stem().string();
    for (char& c : key)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return platformLogDir(key) / "anchorbolt.token.json";
}

// Adopt a persisted {id, token} as a base: only fills fields nothing else set,
// so config/env/flags all still win over it.
void readPairTokenFile(const fs::path& file, StartOptions& opt) {
    ifstream in(file);
    if (!in) return;
    Json j;
    try { j = Json::parse(in); } catch (...) { return; }
    if (opt.appId.empty() && j.contains("id") && j["id"].is_string())
        opt.appId = j["id"].get<string>();
    if (opt.token.empty() && j.contains("token") && j["token"].is_string())
        opt.token = j["token"].get<string>();
}

// Platform-conventional log home (CWD-relative defaults break under
// launchd/systemd, whose cwd is /). --log-dir / config log.dir override.
fs::path platformLogDir(const string& appId) {
    fs::path base;
#if defined(_WIN32)
    const char* lad = getenv("LOCALAPPDATA");
    base = fs::path(lad ? lad : ".") / "anchorbolt";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    base = fs::path(home ? home : ".") / "Library" / "Logs" / "anchorbolt";
#else
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_STATE_HOME");
    base = xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "state";
    base /= "anchorbolt";
#endif
    return base / appId;
}

// Delete our own daily files older than keepDays. Only touches names we
// create (app-*.log / anchorbolt-*.log) — never anything else in the dir.
// `protect` maps a prefix to that shipper's cursor file: files not yet fully
// delivered to the fleet server outlive keepDays until they ship (the local
// files are the offline spool — pruning an unsent file would be a hard gap).
void pruneLogs(const fs::path& logDir, int keepDays,
               const map<string, string>& protect = {}) {
    if (keepDays <= 0) return;
    auto cutoff = chrono::system_clock::now() - chrono::hours(24 * keepDays);
    error_code ec;
    int removed = 0;
    for (auto& e : fs::directory_iterator(logDir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        string name = e.path().filename().string();
        bool ours = (name.rfind("app-", 0) == 0 || name.rfind("anchorbolt-", 0) == 0) &&
                    name.find(".log") != string::npos;
        if (!ours) continue;
        string prefix = name.rfind("app-", 0) == 0 ? "app-" : "anchorbolt-";
        if (auto p = protect.find(prefix); p != protect.end() && name >= p->second) continue;
        auto ft = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        auto sys = chrono::time_point_cast<chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + chrono::system_clock::now());
        if (sys < cutoff) {
            if (fs::remove(e.path(), ec)) ++removed;
        }
    }
    if (removed > 0) {
        logNotice("anchorbolt") << "pruned " << removed << " log file(s) older than "
                                << keepDays << " days";
    }
}

// Can we bind this port right now? Checks BOTH loopback stacks: the app's
// server binds "localhost", which resolves to ::1 on some systems and
// 127.0.0.1 on others — two apps once coexisted on one port by taking one
// stack each, and health polls then reached a random one of them.
// (POSIX: SO_REUSEADDR matches how the app's own HTTP server binds, so
// TIME_WAIT leftovers don't read as "busy". Windows: SO_REUSEADDR would let
// the probe bind OVER a live listener, so it stays off there.)
#ifdef _WIN32

bool portFree(int port) {
    auto probe = [port](int family) -> bool {
        SOCKET s = ::socket(family, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return true;  // stack unavailable -> not our problem
        bool ok;
        if (family == AF_INET) {
            sockaddr_in a4{};
            a4.sin_family = AF_INET;
            a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a4.sin_port = htons((u_short)port);
            ok = ::bind(s, (sockaddr*)&a4, sizeof(a4)) == 0;
        } else {
            int yes = 1;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&yes, sizeof(yes));
            sockaddr_in6 a6{};
            a6.sin6_family = AF_INET6;
            a6.sin6_addr = in6addr_loopback;
            a6.sin6_port = htons((u_short)port);
            ok = ::bind(s, (sockaddr*)&a6, sizeof(a6)) == 0;
        }
        closesocket(s);
        return ok;
    };
    return probe(AF_INET) && probe(AF_INET6);
}

#else

bool portFree(int port) {
    int yes = 1;

    int s4 = socket(AF_INET, SOCK_STREAM, 0);
    if (s4 >= 0) {
        setsockopt(s4, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a4{};
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a4.sin_port = htons((uint16_t)port);
        bool ok = ::bind(s4, (sockaddr*)&a4, sizeof(a4)) == 0;
        close(s4);
        if (!ok) return false;
    }

    int s6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (s6 >= 0) {
        setsockopt(s6, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
        sockaddr_in6 a6{};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_loopback;
        a6.sin6_port = htons((uint16_t)port);
        bool ok = ::bind(s6, (sockaddr*)&a6, sizeof(a6)) == 0;
        close(s6);
        if (!ok) return false;
    }
    return true;
}

#endif

// Port reservation across concurrent supervisors on ONE machine. A bind-probe
// alone races: several `anchorbolt start` launched together each probe 47777,
// all see it free (no app has bound yet), all inject the same MCP port, and
// health polls then hit the wrong app (pid mismatch -> restart storm). An OS
// advisory lock on a per-port lockfile serializes the choice: the first
// supervisor to lock a port owns it for its whole lifetime (the lock releases
// on process exit / fd close), so the next one deterministically moves on.
struct PortLock {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};
std::vector<PortLock> g_portLocks;  // held for the process lifetime

bool reservePort(int port) {
    fs::path lf = fs::temp_directory_path() /
                  ("anchorbolt-port-" + std::to_string(port) + ".lock");
#ifdef _WIN32
    HANDLE h = CreateFileA(lf.string().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;  // can't lock -> don't block use
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &ov)) {
        CloseHandle(h);
        return false;  // another supervisor holds this port
    }
    g_portLocks.push_back({h});
    return true;
#else
    int fd = ::open(lf.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return true;  // can't lock -> don't block use
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;  // EWOULDBLOCK: another supervisor holds this port
    }
    g_portLocks.push_back({fd});
    return true;
#endif
}

void releaseLastPortLock() {
    if (g_portLocks.empty()) return;
#ifdef _WIN32
    if (g_portLocks.back().h != INVALID_HANDLE_VALUE) CloseHandle(g_portLocks.back().h);
#else
    if (g_portLocks.back().fd >= 0) ::close(g_portLocks.back().fd);
#endif
    g_portLocks.pop_back();
}

// Default port taken (another anchorbolt on this machine) -> scan upward so
// a second bare `anchorbolt start` just works. An EXPLICIT --port is a
// contract: fail loudly instead of silently moving. reservePort() serializes
// concurrent supervisors; portFree() then rules out a non-anchorbolt process.
int choosePort(const StartOptions& opt) {
    if (opt.portExplicit) {
        if (!reservePort(opt.port)) return -1;      // another supervisor owns it
        if (portFree(opt.port)) return opt.port;
        releaseLastPortLock();                      // a foreign process has it
        return -1;
    }
    for (int p = opt.port; p < opt.port + 100; ++p) {
        if (!reservePort(p)) continue;              // owned by another supervisor
        if (portFree(p)) return p;
        releaseLastPortLock();                      // foreign process; give it back
    }
    return -1;
}

// --- child process control (platform layer) -----------------------------------
// Proc + spawnApp / pollExit / terminateApp / closeProc. The supervision loop
// only sees these five names; everything OS-specific lives here.

#ifdef _WIN32

struct Proc {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD  pid = 0;
    bool valid() const { return process != nullptr; }
};

// CreateProcess with the ops env injected (children inherit our environment).
// The child goes into a job object with KILL_ON_JOB_CLOSE, so the whole app
// process tree dies with the supervisor — no orphaned fullscreen window left
// on the venue machine if anchorbolt itself is killed.
Proc spawnApp(const StartOptions& opt, const string& logFile) {
    Proc p;
    _putenv_s("TRUSSC_MCP", "1");
    _putenv_s("TRUSSC_MCP_PORT", to_string(opt.port).c_str());
    _putenv_s("TRUSSC_LOG_FILE", logFile.c_str());
    string cmdLine = "\"" + opt.runPath + "\"";
    for (const auto& a : opt.appArgs) cmdLine += " \"" + a + "\"";
    vector<char> cl(cmdLine.begin(), cmdLine.end());
    cl.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(opt.runPath.c_str(), cl.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED,  // join the job BEFORE it runs
                        nullptr, opt.cwd.empty() ? nullptr : opt.cwd.c_str(),
                        &si, &pi)) {
        return p;  // invalid -> caller gives up (bad path is the usual cause)
    }
    p.job = CreateJobObjectA(nullptr, nullptr);
    if (p.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p.job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(p.job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p.process = pi.hProcess;
    p.pid = pi.dwProcessId;
    return p;
}

// Exited? Fills `how` for the event log; giveUp = unrecoverable, stop retrying.
bool pollExit(Proc& p, string& how, bool& giveUp) {
    if (WaitForSingleObject(p.process, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(p.process, &code);
    how = "app exited (code " + to_string((unsigned long)code) + ")";
    giveUp = false;
    return true;
}

// WM_CLOSE to the app's top-level windows first (a clean quit request — the
// sokol window handles it), then TerminateProcess. Windows has no SIGTERM.
void terminateApp(Proc& p) {
    struct Ctx { DWORD pid; };
    Ctx ctx{p.pid};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == ((Ctx*)lp)->pid) PostMessageA(h, WM_CLOSE, 0, 0);
        return TRUE;
    }, (LPARAM)&ctx);
    if (WaitForSingleObject(p.process, 5000) != WAIT_OBJECT_0) {
        logWarning("anchorbolt") << "app ignored WM_CLOSE for 5s; terminating";
        TerminateProcess(p.process, 1);
        WaitForSingleObject(p.process, 2000);
    }
}

// Closing the job handle fires KILL_ON_JOB_CLOSE for any leftover children.
void closeProc(Proc& p) {
    if (p.process) CloseHandle(p.process);
    if (p.job) CloseHandle(p.job);
    p = Proc{};
}

#else  // POSIX

struct Proc {
    pid_t pid = -1;
    bool valid() const { return pid > 0; }
};

Proc spawnApp(const StartOptions& opt, const string& logFile) {
    pid_t pid = fork();
    if (pid == 0) {
        if (!opt.cwd.empty() && chdir(opt.cwd.c_str()) != 0) _exit(126);
        setenv("TRUSSC_MCP", "1", 1);
        setenv("TRUSSC_MCP_PORT", to_string(opt.port).c_str(), 1);
        setenv("TRUSSC_LOG_FILE", logFile.c_str(), 1);
        vector<char*> argv;
        argv.push_back(const_cast<char*>(opt.runPath.c_str()));
        for (const auto& a : opt.appArgs) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(opt.runPath.c_str(), argv.data());
        _exit(127);  // exec failed
    }
    return Proc{pid};
}

// Exited? Fills `how` for the event log; giveUp = exec failure (bad path) —
// retrying would fail forever.
bool pollExit(Proc& p, string& how, bool& giveUp) {
    int st = 0;
    if (waitpid(p.pid, &st, WNOHANG) != p.pid) return false;
    giveUp = false;
    if (WIFEXITED(st)) {
        int code = WEXITSTATUS(st);
        if (code == 127) {
            how = "app failed to exec (bad path?)";
            giveUp = true;
        } else {
            how = "app exited (code " + to_string(code) + ")";
        }
    } else if (WIFSIGNALED(st)) {
        how = "app killed by signal " + to_string(WTERMSIG(st));
    } else {
        how = "app stopped unexpectedly";
    }
    return true;
}

// SIGTERM with a 5s window for a clean shutdown, then SIGKILL. Reaps the child.
void terminateApp(Proc& p) {
    kill(p.pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int st = 0;
        if (waitpid(p.pid, &st, WNOHANG) == p.pid) return;
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    logWarning("anchorbolt") << "app ignored SIGTERM for 5s; sending SIGKILL";
    kill(p.pid, SIGKILL);
    int st = 0;
    waitpid(p.pid, &st, 0);
}

void closeProc(Proc&) {}

#endif // platform layer

} // namespace

int cmdStart(const vector<string>& args) {
#ifdef _WIN32
    // Raw port probes may run before any library socket use.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // Console close / logoff / shutdown all funnel into the same clean stop.
    SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop = true; return TRUE; }, TRUE);
#endif
    StartOptions opt;

    // Pre-pass: config file loads first so flags override it. Explicit
    // --config <path>, or ./anchorbolt.json when present.
    {
        fs::path cfg;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--config") cfg = args[i + 1];
            if (args[i] == "--") break;
        }
        if (cfg.empty() && fs::exists("anchorbolt.json")) cfg = "anchorbolt.json";
        if (!cfg.empty() && !loadConfig(cfg, opt)) return 1;
    }
    // Env token sits between config (tokenFile) and the --token flag.
    if (const char* envTok = getenv("ANCHORBOLT_TOKEN")) opt.token = envTok;
    if (!parseArgs(args, opt)) return 1;
    const bool pairArg = !opt.pairCode.empty();  // now that flags are parsed

    // --pair <code>: before anything else, redeem the code at the fleet server
    // for this venue's id + agent token. No tc-... string ever gets copied by
    // hand — the operator pastes a 6-digit code they read off the dashboard.
    if (!opt.pairCode.empty()) {
        if (opt.serverUrl.empty()) {
            cerr << "anchorbolt start: --pair needs --server <fleet url>" << endl;
            return 1;
        }
        tcx::curl::HttpClient cli;
        cli.setBaseUrl(opt.serverUrl);
        cli.setTimeout(5);
        auto res = cli.request("POST", "/api/pair", Json({{"code", opt.pairCode}}).dump(), "application/json");
        if (res.statusCode != 200) {
            cerr << "anchorbolt start: pairing failed"
                 << (res.statusCode ? " (HTTP " + to_string(res.statusCode) + ": " + res.body + ")"
                                    : " (server " + opt.serverUrl + " unreachable)") << endl;
            return 1;
        }
        Json pr;
        try { pr = Json::parse(res.body); } catch (...) {
            cerr << "anchorbolt start: pairing response was not JSON" << endl;
            return 1;
        }
        opt.appId = pr.value("app", "");   // server-assigned id + token win this run
        opt.token = pr.value("token", "");
        if (opt.appId.empty() || opt.token.empty()) {
            cerr << "anchorbolt start: pairing response missing app/token" << endl;
            return 1;
        }
        logNotice("anchorbolt") << "paired as '" << opt.appId << "' via code";
    }

    // Resolve what -p / config / the current directory points at: a binary,
    // a .app bundle, or a TrussC project. Absolute up front so restarts
    // don't depend on our cwd.
    vector<string> tried;
    fs::path runPath = resolveApp(opt.runPath, tried);
    if (runPath.empty()) {
        cerr << "anchorbolt start: no app found. Looked for:" << endl;
        for (auto& t : tried) cerr << "  " << t << endl;
        cerr << "point -p at a binary, a .app bundle, or a TrussC project directory" << endl;
        return 1;
    }
    opt.runPath = runPath.string();
    if (opt.cwd.empty()) opt.cwd = runPath.parent_path().string();
    opt.projectDir = deriveProjectDir(runPath, opt.cwd);

    // Adopt a previously-paired identity BEFORE the binary-name fallback, so a
    // plain restart reuses the server-assigned id. Lowest priority: fills only
    // id/token that nothing else (config, env, flags, --pair) already set. The
    // file is keyed by the binary name, so we don't need the paired id to find
    // it. Skipped on the pairing run itself (we just got a fresh identity).
    if (!pairArg) readPairTokenFile(pairTokenFile(runPath), opt);

    if (opt.appId.empty()) opt.appId = runPath.stem().string();
    for (char& c : opt.appId) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    }
    fs::path logDir = opt.logDir.empty() ? platformLogDir(opt.appId)
                                         : fs::absolute(opt.logDir);
    fs::create_directories(logDir);

    // Just paired: persist id + token so the next plain run reuses them.
    if (pairArg) {
        fs::path pf = pairTokenFile(runPath);
        error_code ec;
        fs::create_directories(pf.parent_path(), ec);
        { ofstream out(pf, ios::trunc);
          if (out) out << Json({{"id", opt.appId}, {"token", opt.token}}).dump(2) << "\n"; }
        if (fs::exists(pf, ec)) {
#ifndef _WIN32
            // Owner read/write only — it holds a token. (Windows: %LOCALAPPDATA%
            // is already a per-user directory, so ACL inheritance covers it.)
            fs::permissions(pf, fs::perms::owner_read | fs::perms::owner_write,
                            fs::perm_options::replace, ec);
#endif
            logNotice("anchorbolt") << "paired identity saved to " << pf.string();
        } else {
            logWarning("anchorbolt") << "could not write paired identity to " << pf.string();
        }
    }
    string pruneDay = getTimestampString("%Y-%m-%d");

    // MCP port: scan up from the default so several supervisors coexist on
    // one machine; an explicit --port that's busy is a hard error.
    int chosen = choosePort(opt);
    if (chosen < 0) {
        cerr << "anchorbolt start: mcp port " << opt.port
             << (opt.portExplicit ? " is busy (explicit --port is used as-is; pick another)"
                                  : "..+99 all busy") << endl;
        return 1;
    }
    if (chosen != opt.port) {
        logNotice("anchorbolt") << "mcp port " << chosen << " (" << opt.port << " busy)";
    }
    opt.port = chosen;
    shared_ptr<Notifier> notifier;
    if (!opt.sinks.empty()) {
        notifier = make_shared<Notifier>(opt.sinks, opt.appId);
        g_notifier = notifier;
        logNotice("anchorbolt") << opt.sinks.size() << " notification sink(s) armed";
    }
    optional<ServerPush> push;
    optional<CommandChannel> channel;
    // ONE event stream, two audiences: the notification sinks (Slack etc.)
    // and the fleet dashboard (wall badge + event list) get the same events.
    auto fireEvent = [&](const string& ev, const string& msg) {
        if (notifier) notifier->notify(ev, msg);
        if (push) push->alert(ev, msg);
    };
    if (!opt.serverUrl.empty()) {
        push.emplace(opt.serverUrl, opt.appId, opt.token);
        channel.emplace(opt, logDir);
        logNotice("anchorbolt") << "pushing to fleet server " << opt.serverUrl
                                << " as '" << opt.appId << "'"
                                << (opt.allowControl ? " (REMOTE CONTROL ENABLED)" : "")
                                << (opt.allowUpdate ? " (REMOTE UPDATE ENABLED)" : "");
    }
    // Log forwarding: app log + supervisor events. Pushed every poll cycle
    // INDEPENDENT of app health — while the app hangs, the supervisor's
    // "unresponsive / restarting" lines still reach the server, which is what
    // distinguishes "app down, being handled" from "machine gone" remotely.
    // Each source keeps a delivery cursor (see LogShipper): what the server
    // hasn't confirmed stays pending in the local files and ships later.
    LogShipper appShip("app", "app-"), supShip("sup", "anchorbolt-");
    fs::path cursorPath = logDir / "push-cursor.json";
    auto supLogPath = [&logDir]() {
        return logDir / ("anchorbolt-" + getTimestampString("%Y-%m-%d") + ".log");
    };
    if (push) {
        // Resume from the persisted cursor; first contact anchors both tails
        // at the current end, BEFORE this session writes anything — the app's
        // own startup lines (written between spawn and the first poll) must flow.
        Json saved = Json::object();
        if (ifstream in(cursorPath); in) {
            try { saved = Json::parse(in); } catch (...) { saved = Json::object(); }
        }
        appShip.init(saved.contains("app") ? saved["app"] : Json(),
                     logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log"));
        supShip.init(saved.contains("sup") ? saved["sup"] : Json(), supLogPath());
    }
    auto saveCursorNow = [&]() {
        Json j = {{"app", appShip.cursorJson()}, {"sup", supShip.cursorJson()}};
        fs::path tmp = cursorPath;
        tmp += ".tmp";
        { ofstream out(tmp, ios::trunc); out << j.dump() << "\n"; }
        error_code ec;
        fs::rename(tmp, cursorPath, ec);  // atomic: never a half-written cursor
        appShip.clearDirty();
        supShip.clearDirty();
    };
    auto lastCursorSave = chrono::steady_clock::now();
    auto pushLogs = [&](const string& appLogFile) {
        if (!push) return;
        appShip.ship(*push, logDir, appLogFile);
        supShip.ship(*push, logDir, supLogPath());
        // Cursor persistence is lazy (SD-card-friendly): losing the last
        // flush in a crash re-sends at most ~60s of lines, and the server
        // dedups those. Gaps are impossible either way.
        auto now = chrono::steady_clock::now();
        if ((appShip.dirty() || supShip.dirty()) &&
            now - lastCursorSave >= chrono::seconds(60)) {
            lastCursorSave = now;
            saveCursorNow();
        }
    };
    // Prune AFTER the cursor is restored: files the server hasn't confirmed
    // yet are protected from deletion regardless of age.
    auto protectUnshipped = [&]() {
        map<string, string> protect;
        if (push) {
            protect["app-"] = appShip.file();
            protect["anchorbolt-"] = supShip.file();
        }
        return protect;
    };
    pruneLogs(logDir, opt.logKeepDays, protectUnshipped());

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    logNotice("anchorbolt") << "kiosk mode: " << opt.runPath
                       << " (mcp port " << opt.port << ", logs " << logDir.string() << ")";
    eventLog(logDir, "kiosk mode started: " + opt.runPath + " (" + machineMemNote() + ")");

    int restarts = 0;
    while (!g_stop) {
        // One file per day; TRUSSC_LOG_FILE appends, so restarts on the same
        // day continue the same file.
        string logFile = (logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log")).string();
        // Once per app run (it only changes through updates): the commit the
        // dashboard shows next to this venue.
        string appGit = gitHash(opt.projectDir);
        // Claimed at spawn, not at loop exit: the flag is raised BEFORE the
        // update-restart, and it's the run AFTER that restart being audited.
        bool audit = g_updateVerify.exchange(false);

        Proc proc = spawnApp(opt, logFile);
        if (!proc.valid()) {
#ifdef _WIN32
            // CreateProcess fails synchronously — almost always a bad path.
            logError("anchorbolt") << "app failed to launch (bad path?); giving up";
            eventLog(logDir, "app failed to launch; giving up");
            fireEvent("down", "app failed to launch; giving up");
            if (notifier) notifier->flushAndStop();
            return 1;
#else
            logError("anchorbolt") << "fork failed; retrying in 5s";
            eventLog(logDir, "fork failed; retrying in 5s");
            sleepChecked(5);
            continue;
#endif
        }
        logNotice("anchorbolt") << "app launched (pid " << proc.pid << ")";
        eventLog(logDir, "app launched (pid " + to_string(proc.pid) + ")");

        httplib::Client cli("localhost", opt.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(2, 0);

        auto bootAt = chrono::steady_clock::now();
        auto lastOkAt = bootAt;  // last healthy reply (watchdog reference point)
        bool healthy = false;    // first successful poll seen
        bool childAlive = true;
        bool pidWarned = false;  // one-shot port-collision warning
        chrono::steady_clock::time_point lastThumb{};  // epoch -> push right away
        bool statusChecked = false;   // tools/list probed for optional conventions
        bool hasStatus = false;
        bool hasAlerts = false;
        // Why this run ended. The "restart" event fires at the actual respawn
        // (not at detection): when an OS shutdown kills the app before us,
        // g_stop arrives during the pre-respawn pause and no phantom incident
        // is recorded — a normal power-off leaves only a quiet "stop".
        string restartReason;
        vector<string> imageNames;    // statusImage names from the last status

        while (!g_stop) {
            // Process exit?
            string how;
            bool giveUp = false;
            if (pollExit(proc, how, giveUp)) {
                childAlive = false;
                if (giveUp) {
                    logError("anchorbolt") << how << "; giving up";
                    eventLog(logDir, how + "; giving up");
                    fireEvent("down", how + "; giving up");
                    if (notifier) notifier->flushAndStop();
                    closeProc(proc);
                    return 1;
                }
                logWarning("anchorbolt") << how;
                eventLog(logDir, how + " (" + machineMemNote() + ")");
                restartReason = how + " (" + machineMemNote() + ")";
                break;
            }

            sleepChecked(opt.pollSec());
            if (g_stop) break;

            // Day changed? Prune old logs once per day.
            string today = getTimestampString("%Y-%m-%d");
            if (today != pruneDay) {
                pruneDay = today;
                pruneLogs(logDir, opt.logKeepDays, protectUnshipped());
            }

            pushLogs(logFile);
            if (channel) channel->maintain();

            // Remote restart (dashboard button relayed over the command
            // channel): same path as hang recovery, but by request.
            if (g_restartRequested.exchange(false)) {
                logNotice("anchorbolt") << "remote restart requested";
                eventLog(logDir, "remote restart requested; terminating app");
                restartReason = "remote restart requested";
                terminateApp(proc);
                childAlive = false;
                break;
            }

            // Watchdog off: supervise process exit only (works for apps
            // without an MCP endpoint); logs and the channel still flow.
            if (opt.watchdogTimeoutSec <= 0 && !push) continue;

            // Hang detection via tc_get_health.
            auto h = callTool(cli, "tc_get_health");
            if (h) {
                // Confirm the reply is from OUR child — on a shared port a
                // stale or foreign app could answer and mask a dead child.
                int64_t hpid = h->value("pid", (int64_t)0);
                if (hpid != 0 && hpid != (int64_t)proc.pid) {
                    if (!pidWarned) {
                        pidWarned = true;
                        logError("anchorbolt") << "health answered by pid " << hpid
                                               << ", expected " << proc.pid << " (port collision?)";
                        eventLog(logDir, "health answered by pid " + to_string(hpid) +
                                         ", expected " + to_string(proc.pid) + " (port collision?)");
                    }
                    h.reset();  // our app is unobservable -> counts as a miss
                }
            }
            if (h) {
                lastOkAt = chrono::steady_clock::now();
                if (notifier) notifier->healthyTick();
                if (!healthy) {
                    healthy = true;
                    logNotice("anchorbolt") << "app healthy (fps "
                                       << (*h).value("fps", 0.0f) << ", "
                                       << (*h).value("width", 0) << "x" << (*h).value("height", 0) << ")";
                    eventLog(logDir, "app healthy");
                    if (restarts > 0) {
                        fireEvent("up", "app healthy again (pid " +
                                        to_string(proc.pid) + ", restart #" +
                                        to_string(restarts) + ")");
                    }
                    if (audit) {
                        audit = false;
                        string vmsg = "update verified: app healthy on the new binary" +
                                      (appGit.empty() ? "" : " (" + appGit + ")");
                        eventLog(logDir, "[update] " + vmsg);
                        fireEvent("update", vmsg);
                    }
                }
                // Probe tools/list once per app run: which of the optional
                // conventions does this app speak? (status for push, alerts
                // for the notification sinks — either consumer can be absent.)
                if ((push || notifier) && !statusChecked) {
                    statusChecked = true;
                    if (auto r = callRpc(cli, "tools/list")) {
                        for (auto& t : r->value("tools", Json::array())) {
                            string n = t.value("name", "");
                            if (n == "tc_get_status") hasStatus = true;
                            if (n == "tc_get_alerts") hasAlerts = true;
                        }
                    }
                }
                // App-raised operator alerts (mcp::alert) -> sinks. Drain on
                // the health cadence; the tool clears what it returns.
                if ((notifier || push) && hasAlerts) {
                    if (auto a = callTool(cli, "tc_get_alerts")) {
                        for (auto& al : a->value("alerts", Json::array())) {
                            string text = al.value("text", "");
                            if (!text.empty()) fireEvent("alert", text);
                        }
                    }
                }
                if (push) {
                    Json hb = *h;
                    MachineMem mm = machineMem();
                    hb["machine"] = {{"memTotalBytes", mm.totalBytes},
                                     {"memAvailBytes", mm.availBytes}};
                    if (!appGit.empty()) hb["git"] = appGit;
                    if (hasStatus) {
                        if (auto s = callTool(cli, "tc_get_status")) {
                            imageNames.clear();
                            for (auto& n : s->value("images", Json::array()))
                                imageNames.push_back(n.get<string>());
                            if (!s->value("values", Json::array()).empty() || !imageNames.empty())
                                hb["custom"] = *s;
                        }
                    }
                    push->heartbeat(hb);

                    auto now = chrono::steady_clock::now();
                    if (opt.thumbIntervalSec > 0 &&
                        now - lastThumb >= chrono::seconds(opt.thumbIntervalSec)) {
                        lastThumb = now;
                        if (auto t = callTool(cli, "tc_get_screenshot",
                                              {{"format", "jpg"},
                                               {"width", opt.thumbWidth},
                                               {"quality", opt.thumbQuality}})) {
                            if (t->contains("data") && (*t)["data"].is_string()) {
                                auto jpg = fromBase64((*t)["data"].get<string>());
                                if (!jpg.empty()) push->thumbnail(jpg);
                            }
                        }
                        for (const auto& n : imageNames) {
                            if (auto t = callTool(cli, "tc_get_status_image", {{"name", n}})) {
                                if (t->contains("data") && (*t)["data"].is_string()) {
                                    auto jpg = fromBase64((*t)["data"].get<string>());
                                    if (!jpg.empty()) push->image(n, jpg);
                                }
                            }
                        }
                    }
                }
            } else if (opt.watchdogTimeoutSec > 0) {
                auto now = chrono::steady_clock::now();
                bool inGrace = !healthy && now - bootAt < chrono::seconds(opt.graceSec);
                if (!inGrace) {
                    int silentSec = (int)chrono::duration_cast<chrono::seconds>(now - lastOkAt).count();
                    logWarning("anchorbolt") << "health poll miss (silent " << silentSec
                                             << "s / " << opt.watchdogTimeoutSec << "s)";
                    if (silentSec >= opt.watchdogTimeoutSec) {
                        logError("anchorbolt") << "app unresponsive; restarting";
                        eventLog(logDir, "app unresponsive for " + to_string(silentSec) +
                                         "s; terminating (" + machineMemNote() + ")");
                        restartReason = "app unresponsive for " + to_string(silentSec) +
                                        "s (" + machineMemNote() + ")";
                        terminateApp(proc);
                        childAlive = false;
                        break;
                    }
                }
            }
        }

        if (g_stop) {
            if (childAlive) terminateApp(proc);
            closeProc(proc);
            break;
        }
        closeProc(proc);

        // First run after an update must prove itself: with the watchdog on,
        // "never became healthy" fails the audition; without MCP, dying
        // within 30s does. Failure restores the pre-update binary — the
        // respawn below then relaunches the old, known-good version.
        if (audit) {
            bool bootFailed = opt.watchdogTimeoutSec > 0
                ? !healthy
                : chrono::steady_clock::now() - bootAt < chrono::seconds(30);
            if (bootFailed) {
                fs::path prev = opt.runPath + ".prev";
                error_code ec;
                if (fs::exists(prev, ec)) {
                    fs::copy_file(prev, opt.runPath, fs::copy_options::overwrite_existing, ec);
                    string rmsg = ec
                        ? "rollback FAILED: " + ec.message()
                        : "rolling back: the new binary never became healthy; previous binary restored";
                    eventLog(logDir, "[update] " + rmsg);
                    fireEvent("update", rmsg);
                } else {
                    eventLog(logDir, "[update] rollback impossible: no previous binary kept");
                    fireEvent("update", "rollback impossible: no previous binary kept");
                }
            }
        }

        ++restarts;
        logNotice("anchorbolt") << "restarting app (#" << restarts << ") in 2s";
        eventLog(logDir, "restarting app (#" + to_string(restarts) + ")");
        sleepChecked(2);
        // Fire only if we are actually going to respawn — if g_stop arrived
        // during the pause (OS shutdown killed the app first), this was never
        // an incident and only the "stop" event goes out.
        if (!g_stop) {
            fireEvent("restart", restartReason.empty()
                ? "restarting (#" + to_string(restarts) + ")"
                : restartReason + " — restarting (#" + to_string(restarts) + ")");
        }
    }

    logNotice("anchorbolt") << "anchorbolt stopped (restarts: " << restarts << ")";
    eventLog(logDir, "anchorbolt stopped (restarts: " + to_string(restarts) + ")");
    fireEvent("stop", "anchorbolt stopped (restarts: " + to_string(restarts) + ")");
    if (notifier) {
        notifier->flushAndStop();
        g_notifier.reset();
    }
    // Final flush so the shutdown events reach the server too, then persist
    // the cursor unconditionally — the lazy 60s cadence doesn't apply to exit.
    pushLogs((logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log")).string());
    if (push && (appShip.dirty() || supShip.dirty())) saveCursorNow();
    if (channel) channel->shutdown();
    return 0;
}
