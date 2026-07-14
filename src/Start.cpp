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
    string cwd;                  // working dir (default: binary's directory)
    string logDir = "anchorbolt-logs";
    int    port        = 47777;  // TRUSSC_MCP_PORT
    int    intervalSec = 3;      // health poll interval
    int    graceSec    = 15;     // boot grace before misses count
    int    maxMisses   = 3;      // consecutive misses -> restart
    string serverUrl;            // fleet server base URL (empty = no push)
    string appId;                // id on the fleet server (default: binary name)
    int    thumbIntervalSec = 30;
};

atomic<bool> g_stop{false};
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
optional<Json> callTool(httplib::Client& cli, const string& name,
                        const Json& arguments = Json::object()) {
    Json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "tools/call"},
                {"params", {{"name", name}, {"arguments", arguments}}}};
    auto res = cli.Post("/mcp", req.dump(), "application/json");
    if (!res || res->status != 200) return nullopt;
    try {
        Json reply = Json::parse(res->body);
        return Json::parse(reply.at("result").at("content").at(0).at("text").get<string>());
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

// Best-effort push to the fleet server. Failures never affect supervision;
// reachability changes are logged once, not per attempt.
class ServerPush {
public:
    ServerPush(const string& url, const string& appId)
        : cli_(url), appId_(appId) {
        cli_.set_connection_timeout(2, 0);
        cli_.set_read_timeout(2, 0);
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
        else if (a == "--interval") { auto v = next("--interval"); if (!v) return false; opt.intervalSec = stoi(*v); }
        else if (a == "--grace")    { auto v = next("--grace");    if (!v) return false; opt.graceSec = stoi(*v); }
        else if (a == "--misses")   { auto v = next("--misses");   if (!v) return false; opt.maxMisses = stoi(*v); }
        else if (a == "--server")   { auto v = next("--server");   if (!v) return false; opt.serverUrl = *v; }
        else if (a == "--id")       { auto v = next("--id");       if (!v) return false; opt.appId = *v; }
        else if (a == "--thumb-interval") { auto v = next("--thumb-interval"); if (!v) return false; opt.thumbIntervalSec = stoi(*v); }
        else if (!a.empty() && a[0] != '-') {
            if (!opt.runPath.empty()) {
                cerr << "anchorbolt start: unexpected extra argument '" << a << "'" << endl;
                return false;
            }
            opt.runPath = a;  // positional: the app binary
        }
        else {
            cerr << "anchorbolt start: unknown option '" << a << "'" << endl;
            return false;
        }
    }
    if (opt.runPath.empty()) {
        cerr << "anchorbolt start: an app binary is required "
             << "(anchorbolt start <app-binary> [options])" << endl;
        return false;
    }
    return true;
}

#ifndef _WIN32

pid_t spawnApp(const StartOptions& opt, const string& logFile) {
    pid_t pid = fork();
    if (pid == 0) {
        if (!opt.cwd.empty() && chdir(opt.cwd.c_str()) != 0) _exit(126);
        setenv("TRUSSC_MCP", "1", 1);
        setenv("TRUSSC_MCP_PORT", to_string(opt.port).c_str(), 1);
        setenv("TRUSSC_LOG_FILE", logFile.c_str(), 1);
        execl(opt.runPath.c_str(), opt.runPath.c_str(), (char*)nullptr);
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
    if (!parseArgs(args, opt)) return 1;

    // Resolve paths up front so restarts don't depend on our cwd.
    error_code ec;
    fs::path runPath = fs::absolute(opt.runPath, ec);
    if (ec || !fs::exists(runPath)) {
        cerr << "anchorbolt start: app binary not found: " << opt.runPath << endl;
        return 1;
    }
    opt.runPath = runPath.string();
    if (opt.cwd.empty()) opt.cwd = runPath.parent_path().string();
    fs::path logDir = fs::absolute(opt.logDir);
    fs::create_directories(logDir);

    // Fleet server push is optional and never affects supervision.
    if (opt.appId.empty()) opt.appId = runPath.stem().string();
    for (char& c : opt.appId) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    }
    optional<ServerPush> push;
    if (!opt.serverUrl.empty()) {
        push.emplace(opt.serverUrl, opt.appId);
        logNotice("anchorbolt") << "pushing to fleet server " << opt.serverUrl
                                << " as '" << opt.appId << "'";
    }

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
        bool healthy = false;    // first successful poll seen
        bool childAlive = true;
        bool pidWarned = false;  // one-shot port-collision warning
        int  misses = 0;
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

            sleepChecked(opt.intervalSec);
            if (g_stop) break;

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
                misses = 0;
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
                    if (now - lastThumb >= chrono::seconds(opt.thumbIntervalSec)) {
                        lastThumb = now;
                        if (auto t = callTool(cli, "tc_get_screenshot",
                                              {{"format", "jpg"}, {"width", 512}})) {
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
            } else {
                bool inGrace = !healthy &&
                    chrono::steady_clock::now() - bootAt < chrono::seconds(opt.graceSec);
                if (!inGrace) {
                    ++misses;
                    logWarning("anchorbolt") << "health poll miss (" << misses << "/" << opt.maxMisses << ")";
                    if (misses >= opt.maxMisses) {
                        logError("anchorbolt") << "app unresponsive; restarting";
                        eventLog(logDir, "app unresponsive after " + to_string(misses) +
                                         " misses; terminating (" + machineMemNote() + ")");
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
    return 0;
#endif
}
