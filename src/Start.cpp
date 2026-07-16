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

#include <TrussC.h>
#include <impl/httplib.h>
#include <tcWebSocketClient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
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
    string logDir;               // empty = platform default (~/Library/Logs/anchorbolt/<id> etc.)
    int    logKeepDays = 30;     // prune logs older than this (0 = keep forever)
    int    port        = 47777;  // TRUSSC_MCP_PORT
    int    watchdogTimeoutSec = 10;  // unresponsive this long -> restart (0 = process-exit only)
    int    graceSec    = 15;     // boot grace before the watchdog arms
    string serverUrl;            // fleet server base URL (empty = no push)
    string appId;                // id on the fleet server (default: binary name)
    string token;                // agent token (--token > ANCHORBOLT_TOKEN > tokenFile)
    int    thumbIntervalSec = 30;    // 0 = never push screenshots
    int    thumbWidth = 512;
    int    thumbQuality = 75;
    int    wsPort = 0;           // command channel port (0 = server port + 1)
    bool   allowControl = false; // let the server relay MUTATING tools

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

// Machine-wide memory, the OS's view. Rides every heartbeat and gets
// snapshotted into the event log on failures — so an operator can tell
// "our app leaked" from "something else ate the machine" (OOM exoneration).
struct MachineMem {
    int64_t totalBytes = 0;
    int64_t availBytes = 0;
};

MachineMem machineMem() {
    MachineMem m;
#if defined(__APPLE__)
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

// Incremental reader for a log file that other code appends to. First poll
// starts at the current end (history is not re-uploaded); day rollover hands
// in a new path, which resets to the top of the (fresh) file.
class LogTailer {
public:
    // Anchor at the current end of `path`: everything already in the file is
    // history and won't be uploaded; everything appended after this call will.
    // Call once at startup, BEFORE the session's own first lines are written.
    void baseline(const fs::path& path) {
        path_ = path;
        error_code ec;
        offset_ = (streamoff)fs::file_size(path, ec);
        if (ec) offset_ = 0;
        firstPoll_ = false;
    }

    // Returns complete new lines appended since the last poll.
    vector<string> poll(const fs::path& path) {
        if (path != path_) {
            path_ = path;
            error_code ec;
            offset_ = firstPoll_ ? (streamoff)fs::file_size(path, ec) : 0;
            if (ec) offset_ = 0;
            firstPoll_ = false;
        }
        vector<string> lines;
        ifstream in(path_, ios::binary);
        if (!in) return lines;
        in.seekg(0, ios::end);
        streamoff size = in.tellg();
        if (size <= offset_) { if (size < offset_) offset_ = 0; return lines; }
        in.seekg(offset_);
        string chunk(size_t(size - offset_), '\0');
        in.read(chunk.data(), (streamsize)chunk.size());
        size_t consumed = 0, pos = 0;
        while (true) {
            size_t nl = chunk.find('\n', pos);
            if (nl == string::npos) break;  // partial last line stays for next poll
            if (nl > pos) lines.emplace_back(chunk, pos, nl - pos);
            pos = nl + 1;
            consumed = pos;
        }
        offset_ += (streamoff)consumed;
        return lines;
    }

private:
    fs::path path_;
    streamoff offset_ = 0;
    bool firstPoll_ = true;
};

// Best-effort push to the fleet server. Failures never affect supervision;
// reachability changes are logged once, not per attempt.
class ServerPush {
public:
    ServerPush(const string& url, const string& appId, const string& token)
        : cli_(url), appId_(appId) {
        cli_.set_connection_timeout(2, 0);
        cli_.set_read_timeout(2, 0);
        if (!token.empty()) {
            cli_.set_default_headers({{"Authorization", "Bearer " + token}});
        }
    }

    void heartbeat(Json health) {
        health["app"] = appId_;
        auto res = cli_.Post("/api/heartbeat", health.dump(), "application/json");
        report(res && res->status == 200, "heartbeat");
    }

    void thumbnail(const vector<unsigned char>& jpg) {
        auto res = cli_.Post("/api/thumb/" + appId_,
                             string((const char*)jpg.data(), jpg.size()), "image/jpeg");
        report(res && res->status == 200, "thumbnail");
    }

    void image(const string& name, const vector<unsigned char>& jpg) {
        auto res = cli_.Post("/api/image/" + appId_ + "/" + name,
                             string((const char*)jpg.data(), jpg.size()), "image/jpeg");
        report(res && res->status == 200, "image");
    }

    // src is "app" (the app's TRUSSC_LOG_FILE) or "sup" (supervisor events).
    void logs(const string& src, const vector<string>& lines) {
        if (lines.empty()) return;
        Json arr = Json::array();
        for (const auto& l : lines) arr.push_back({{"src", src}, {"text", l}});
        auto res = cli_.Post("/api/log/" + appId_,
                             Json({{"lines", arr}}).dump(), "application/json");
        report(res && res->status == 200, "log");
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

    httplib::Client cli_;
    string appId_;
    bool reachable_ = true;  // assume up so the first failure logs
};

// The agent's end of the command channel: one outbound WebSocket to the
// server (NAT-friendly). Commands arrive on the socket thread; restart is
// handed to the supervision loop via g_restartRequested, tool calls are
// relayed to the app's MCP endpoint directly from here.
class CommandChannel {
public:
    CommandChannel(const StartOptions& opt) : opt_(opt) {
        // ws://host:wsPort/ derived from the HTTP server URL.
        string url = opt.serverUrl;
        bool tls = url.rfind("https://", 0) == 0;
        if (tls) url = url.substr(8);
        else if (url.rfind("http://", 0) == 0) url = url.substr(7);
        size_t slash = url.find('/');
        if (slash != string::npos) url = url.substr(0, slash);
        size_t colon = url.find(':');
        string host = (colon == string::npos) ? url : url.substr(0, colon);
        int wsPort = opt.wsPort;
        if (wsPort == 0) {
            int httpPort = tls ? 443 : 80;
            if (colon != string::npos) httpPort = stoi(url.substr(colon + 1));
            wsPort = httpPort + 1;
        }
        url_ = (tls ? "wss://" : "ws://") + host + ":" + to_string(wsPort) + "/";

        openL_ = ws_.onOpen.listen([this]() {
            ws_.send(Json({{"type", "hello"},
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

    void shutdown() { ws_.disconnect(); }

private:
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
        } else {
            reply["ok"] = false;
            reply["error"] = "unknown action";
        }
        ws_.send(reply.dump());
    }

    StartOptions opt_;
    string url_;
    tcx::websocket::WebSocketClient ws_;
    EventListener openL_, msgL_, closeL_;
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
        else if (a == "--port")     { auto v = next("--port");     if (!v) return false; opt.port = stoi(*v); }
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
        else if (a == "--ws-port")  { auto v = next("--ws-port");  if (!v) return false; opt.wsPort = stoi(*v); }
        else if (a == "--allow-control") { opt.allowControl = true; }
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
    opt.port      = c.value("port", opt.port);
    opt.wsPort    = c.value("wsPort", opt.wsPort);
    opt.allowControl = c.value("allowControl", opt.allowControl);
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
    logNotice("anchorbolt") << "config loaded: " << path.string();
    return true;
}

// Platform-conventional log home (CWD-relative defaults break under
// launchd/systemd, whose cwd is /). --log-dir / config log.dir override.
fs::path platformLogDir(const string& appId) {
    const char* home = getenv("HOME");
    fs::path base;
#if defined(__APPLE__)
    base = fs::path(home ? home : ".") / "Library" / "Logs" / "anchorbolt";
#else
    const char* xdg = getenv("XDG_STATE_HOME");
    base = xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "state";
    base /= "anchorbolt";
#endif
    return base / appId;
}

// Delete our own daily files older than keepDays. Only touches names we
// create (app-*.log / anchorbolt-*.log) — never anything else in the dir.
void pruneLogs(const fs::path& logDir, int keepDays) {
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

#ifndef _WIN32

pid_t spawnApp(const StartOptions& opt, const string& logFile) {
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
    return pid;
}

// SIGTERM with a 5s window for a clean shutdown, then SIGKILL. Reaps the child.
void terminateApp(pid_t pid) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    logWarning("anchorbolt") << "app ignored SIGTERM for 5s; sending SIGKILL";
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
}

#endif // !_WIN32

} // namespace

int cmdStart(const vector<string>& args) {
#ifdef _WIN32
    cerr << "anchorbolt start: Windows support is not implemented yet" << endl;
    return 1;
#else
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
    if (opt.appId.empty()) opt.appId = runPath.stem().string();
    for (char& c : opt.appId) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    }
    fs::path logDir = opt.logDir.empty() ? platformLogDir(opt.appId)
                                         : fs::absolute(opt.logDir);
    fs::create_directories(logDir);
    pruneLogs(logDir, opt.logKeepDays);
    string pruneDay = getTimestampString("%Y-%m-%d");
    optional<ServerPush> push;
    optional<CommandChannel> channel;
    if (!opt.serverUrl.empty()) {
        push.emplace(opt.serverUrl, opt.appId, opt.token);
        channel.emplace(opt);
        logNotice("anchorbolt") << "pushing to fleet server " << opt.serverUrl
                                << " as '" << opt.appId << "'"
                                << (opt.allowControl ? " (REMOTE CONTROL ENABLED)" : "");
    }
    // Log forwarding: app log + supervisor events. Pushed every poll cycle
    // INDEPENDENT of app health — while the app hangs, the supervisor's
    // "unresponsive / restarting" lines still reach the server, which is what
    // distinguishes "app down, being handled" from "machine gone" remotely.
    LogTailer appTail, supTail;
    auto supLogPath = [&logDir]() {
        return logDir / ("anchorbolt-" + getTimestampString("%Y-%m-%d") + ".log");
    };
    auto pushLogs = [&](const string& appLogFile) {
        if (!push) return;
        push->logs("app", appTail.poll(appLogFile));
        push->logs("sup", supTail.poll(supLogPath()));
    };
    // Anchor both tails now, before this session writes anything — the app's
    // own startup lines (written between spawn and the first poll) must flow.
    appTail.baseline(logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log"));
    supTail.baseline(supLogPath());

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

        pid_t pid = spawnApp(opt, logFile);
        if (pid < 0) {
            logError("anchorbolt") << "fork failed; retrying in 5s";
            eventLog(logDir, "fork failed; retrying in 5s");
            sleepChecked(5);
            continue;
        }
        logNotice("anchorbolt") << "app launched (pid " << pid << ")";
        eventLog(logDir, "app launched (pid " + to_string(pid) + ")");

        httplib::Client cli("localhost", opt.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(2, 0);

        auto bootAt = chrono::steady_clock::now();
        auto lastOkAt = bootAt;  // last healthy reply (watchdog reference point)
        bool healthy = false;    // first successful poll seen
        bool childAlive = true;
        bool pidWarned = false;  // one-shot port-collision warning
        chrono::steady_clock::time_point lastThumb{};  // epoch -> push right away
        bool statusChecked = false;   // tools/list probed for tc_get_status
        bool hasStatus = false;
        vector<string> imageNames;    // statusImage names from the last status

        while (!g_stop) {
            // Process exit?
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                childAlive = false;
                if (WIFEXITED(st)) {
                    int code = WEXITSTATUS(st);
                    if (code == 127) {
                        logError("anchorbolt") << "app failed to exec (bad path?); giving up";
                        eventLog(logDir, "app failed to exec; giving up");
                        return 1;
                    }
                    logWarning("anchorbolt") << "app exited (code " << code << ")";
                    eventLog(logDir, "app exited (code " + to_string(code) + ", " + machineMemNote() + ")");
                } else if (WIFSIGNALED(st)) {
                    logWarning("anchorbolt") << "app killed by signal " << WTERMSIG(st);
                    eventLog(logDir, "app killed by signal " + to_string(WTERMSIG(st)) + " (" + machineMemNote() + ")");
                }
                break;
            }

            sleepChecked(opt.pollSec());
            if (g_stop) break;

            // Day changed? Prune old logs once per day.
            string today = getTimestampString("%Y-%m-%d");
            if (today != pruneDay) {
                pruneDay = today;
                pruneLogs(logDir, opt.logKeepDays);
            }

            pushLogs(logFile);
            if (channel) channel->maintain();

            // Remote restart (dashboard button relayed over the command
            // channel): same path as hang recovery, but by request.
            if (g_restartRequested.exchange(false)) {
                logNotice("anchorbolt") << "remote restart requested";
                eventLog(logDir, "remote restart requested; terminating app");
                terminateApp(pid);
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
                if (hpid != 0 && hpid != (int64_t)pid) {
                    if (!pidWarned) {
                        pidWarned = true;
                        logError("anchorbolt") << "health answered by pid " << hpid
                                               << ", expected " << pid << " (port collision?)";
                        eventLog(logDir, "health answered by pid " + to_string(hpid) +
                                         ", expected " + to_string(pid) + " (port collision?)");
                    }
                    h.reset();  // our app is unobservable -> counts as a miss
                }
            }
            if (h) {
                lastOkAt = chrono::steady_clock::now();
                if (!healthy) {
                    healthy = true;
                    logNotice("anchorbolt") << "app healthy (fps "
                                       << (*h).value("fps", 0.0f) << ", "
                                       << (*h).value("width", 0) << "x" << (*h).value("height", 0) << ")";
                    eventLog(logDir, "app healthy");
                }
                if (push) {
                    // Custom ops status (the anchorbolt convention): probe
                    // tools/list once per app run, then ride every heartbeat.
                    if (!statusChecked) {
                        statusChecked = true;
                        if (auto r = callRpc(cli, "tools/list")) {
                            for (auto& t : r->value("tools", Json::array())) {
                                if (t.value("name", "") == "tc_get_status") {
                                    hasStatus = true;
                                    break;
                                }
                            }
                        }
                    }
                    Json hb = *h;
                    MachineMem mm = machineMem();
                    hb["machine"] = {{"memTotalBytes", mm.totalBytes},
                                     {"memAvailBytes", mm.availBytes}};
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
                        terminateApp(pid);
                        childAlive = false;
                        break;
                    }
                }
            }
        }

        if (g_stop) {
            if (childAlive) terminateApp(pid);
            break;
        }

        ++restarts;
        logNotice("anchorbolt") << "restarting app (#" << restarts << ") in 2s";
        eventLog(logDir, "restarting app (#" + to_string(restarts) + ")");
        sleepChecked(2);
    }

    logNotice("anchorbolt") << "anchorbolt stopped (restarts: " << restarts << ")";
    eventLog(logDir, "anchorbolt stopped (restarts: " + to_string(restarts) + ")");
    // Final flush so the shutdown events reach the server too.
    pushLogs((logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log")).string());
    if (channel) channel->shutdown();
    return 0;
#endif
}
