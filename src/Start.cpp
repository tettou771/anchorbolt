// =============================================================================
// anchorbolt start — kiosk mode: the venue-side supervisor
//
// Spawns the app as a child process with the ops environment injected
// (TRUSSC_MCP / TRUSSC_MCP_PORT / TRUSSC_LOG_FILE), then watches it two ways:
//   - process exit  (waitpid)            -> restart
//   - hang          (get_health silence) -> SIGTERM/SIGKILL -> restart
// The app needs zero code changes; everything rides the standard MCP tools.
// =============================================================================

#include "Start.h"

#include <TrussC.h>
#include <impl/httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
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

// JSON-RPC tools/call against the app's local MCP endpoint.
// nullopt = transport failure or malformed reply (both count as a miss).
optional<Json> callTool(httplib::Client& cli, const string& name) {
    Json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "tools/call"},
                {"params", {{"name", name}, {"arguments", Json::object()}}}};
    auto res = cli.Post("/mcp", req.dump(), "application/json");
    if (!res || res->status != 200) return nullopt;
    try {
        Json reply = Json::parse(res->body);
        return Json::parse(reply.at("result").at("content").at(0).at("text").get<string>());
    } catch (...) {
        return nullopt;
    }
}

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

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    logNotice("anchorbolt") << "kiosk mode: " << opt.runPath
                       << " (mcp port " << opt.port << ", logs " << logDir.string() << ")";

    int restarts = 0;
    while (!g_stop) {
        // One file per day; TRUSSC_LOG_FILE appends, so restarts on the same
        // day continue the same file.
        string logFile = (logDir / ("app-" + getTimestampString("%Y-%m-%d") + ".log")).string();

        pid_t pid = spawnApp(opt, logFile);
        if (pid < 0) {
            logError("anchorbolt") << "fork failed; retrying in 5s";
            sleepChecked(5);
            continue;
        }
        logNotice("anchorbolt") << "app launched (pid " << pid << ")";

        httplib::Client cli("localhost", opt.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(2, 0);

        auto bootAt = chrono::steady_clock::now();
        bool healthy = false;    // first successful poll seen
        bool childAlive = true;
        int  misses = 0;

        while (!g_stop) {
            // Process exit?
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                childAlive = false;
                if (WIFEXITED(st)) {
                    int code = WEXITSTATUS(st);
                    if (code == 127) {
                        logError("anchorbolt") << "app failed to exec (bad path?); giving up";
                        return 1;
                    }
                    logWarning("anchorbolt") << "app exited (code " << code << ")";
                } else if (WIFSIGNALED(st)) {
                    logWarning("anchorbolt") << "app killed by signal " << WTERMSIG(st);
                }
                break;
            }

            sleepChecked(opt.intervalSec);
            if (g_stop) break;

            // Hang detection via get_health.
            if (auto h = callTool(cli, "get_health")) {
                misses = 0;
                if (!healthy) {
                    healthy = true;
                    logNotice("anchorbolt") << "app healthy (fps "
                                       << (*h).value("fps", 0.0f) << ", "
                                       << (*h).value("width", 0) << "x" << (*h).value("height", 0) << ")";
                }
            } else {
                bool inGrace = !healthy &&
                    chrono::steady_clock::now() - bootAt < chrono::seconds(opt.graceSec);
                if (!inGrace) {
                    ++misses;
                    logWarning("anchorbolt") << "health poll miss (" << misses << "/" << opt.maxMisses << ")";
                    if (misses >= opt.maxMisses) {
                        logError("anchorbolt") << "app unresponsive; restarting";
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
        sleepChecked(2);
    }

    logNotice("anchorbolt") << "anchorbolt stopped (restarts: " << restarts << ")";
    return 0;
#endif
}
