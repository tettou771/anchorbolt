// =============================================================================
// anchorbolt serve — fleet server
//
// Phase 0: the observation surface. Agents (`anchorbolt start --server`) push
//   POST /api/heartbeat        health JSON  {"app": "<id>", ...get_health fields}
//   POST /api/thumb/<id>       raw JPEG bytes (image/jpeg body, no base64)
// and the browser reads
//   GET  /                     thumbnail-wall dashboard (polls /api/apps)
//   GET  /api/apps             all known apps + latest health + freshness
//   GET  /api/thumb/<id>       latest JPEG
// Storage is DB-free: JSONL per day for heartbeats, timestamp-named JPEGs.
// No WS, no auth yet — Phase 0 assumes a trusted network / localhost.
// =============================================================================

#include "Serve.h"
#include "Token.h"
#include "WsHub.h"

#include <TrussC.h>
#include <impl/httplib.h>

#include <future>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

namespace {

// Decode standard base64 (agents ship live frames as base64 text — see the
// note in the WS onMessage frame handler for why text, not binary). Skips
// padding and whitespace. Mirrors fromBase64() in Start.cpp; core has toBase64
// but no decoder.
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

struct ServeOptions {
    int    port     = 54722;  // "truss" typed on the QWERTY number row
    int    wsPort   = 0;      // 0 = port + 1
    string dataDir  = "anchorbolt-data";
    int    keepDays = 90;     // server-side retention (0 = keep forever);
                              // independent of any venue's local --log-keep
};

struct ImageSlot {
    vector<unsigned char> jpeg;
    uint64_t seq = 0;
};

// Recent log lines held in memory for the UI (full history lives on disk).
// seq is a per-app cursor so the browser can fetch only what's new.
struct LogLine {
    uint64_t seq;
    string   at;    // server receive time HH:MM:SS
    string   src;   // "app" | "sup"
    string   text;
};

struct AppState {
    Json     health;                                // last heartbeat payload
    chrono::steady_clock::time_point lastSeen{};
    vector<unsigned char> thumb;                    // latest JPEG
    uint64_t thumbSeq = 0;                          // bumped per upload (cache busting)
    map<string, ImageSlot> images;                  // custom statusImage uploads
    deque<LogLine> logs;                            // ring buffer, newest last
    uint64_t nextLogSeq = 1;
    map<string, pair<string, int64_t>> logSeen;     // src -> (file, acked end offset)
    deque<LogLine> alerts;                          // app-raised operator alerts
    uint64_t nextAlertSeq = 1;
    int unackedAlerts = 0;                          // wall badge; Clear resets

    // Live view: latest frame pushed by the agent, plus lifecycle bookkeeping.
    // The stream is driven implicitly by GET /api/live polling — no explicit
    // start/stop from the browser.
    vector<unsigned char> liveFrame;                // latest JPEG (decoded)
    uint64_t liveSeq = 0;                           // bumped per frame (X-Live-Seq)
    chrono::steady_clock::time_point liveAt{};      // when it arrived (staleness)
    chrono::steady_clock::time_point lastLiveStart{}; // last live_start we sent
};

constexpr size_t kLogRingSize = 500;

map<string, AppState> g_apps;
mutex g_appsMutex;

atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

// --- agent command channel (WebSocket) ---------------------------------------
// Agents keep one outbound WS to the hub; the dashboard's HTTP requests are
// bridged to WS commands and block on the agent's reply.

struct AgentChannel {
    WsHub hub;
    map<string, int> byApp;          // appId -> ws clientId
    map<int, string> byClient;       // ws clientId -> appId
    map<uint64_t, shared_ptr<promise<Json>>> pending;
    uint64_t nextCmdId = 1;
    mutex mutex_;

    bool live(const string& appId) {
        lock_guard<mutex> lock(mutex_);
        return byApp.count(appId) > 0;
    }

    // Send a command to the app's agent and wait for its reply.
    Json command(const string& appId, Json cmd, int timeoutSec = 15) {
        int clientId = -1;
        uint64_t id = 0;
        auto prom = make_shared<promise<Json>>();
        {
            lock_guard<mutex> lock(mutex_);
            auto it = byApp.find(appId);
            if (it == byApp.end()) {
                return Json{{"ok", false}, {"error", "agent not connected"}};
            }
            clientId = it->second;
            id = nextCmdId++;
            pending[id] = prom;
        }
        cmd["type"] = "cmd";
        cmd["id"] = id;
        if (!hub.sendText(clientId, cmd.dump())) {
            lock_guard<mutex> lock(mutex_);
            pending.erase(id);
            return Json{{"ok", false}, {"error", "agent send failed"}};
        }
        auto fut = prom->get_future();
        if (fut.wait_for(chrono::seconds(timeoutSec)) != future_status::ready) {
            lock_guard<mutex> lock(mutex_);
            pending.erase(id);
            return Json{{"ok", false}, {"error", "agent reply timed out"}};
        }
        return fut.get();
    }
};

AgentChannel g_agents;

// --- server-side retention ----------------------------------------------------
// Independent policy from any venue's local --log-keep (deletions never
// propagate in either direction; the server typically keeps LONGER than the
// kiosk). All timestamps live in filenames, so pruning is pure string math —
// no mtime, no parsing.

// Format a past time with strftime (getTimestampString only does "now").
string fmtTimePoint(chrono::system_clock::time_point t, const char* fmt) {
    time_t tt = chrono::system_clock::to_time_t(t);
    tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    char buf[40];
    strftime(buf, sizeof(buf), fmt, &local);
    return buf;
}

// Thumbnail/statusImage thinning in one JPEG directory (names are
// YYYYMMDD-HHMMSS.jpg): the last 24h stays complete, older shots thin to the
// first per hour, and past keepDays they go entirely.
int thinJpegs(const fs::path& dir, const string& hourlyBefore, const string& dropBefore) {
    error_code ec;
    vector<string> names;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        string n = e.path().filename().string();
        if (n.size() == 19 && n.compare(15, 4, ".jpg") == 0) names.push_back(n);
    }
    sort(names.begin(), names.end());
    int removed = 0;
    string keptHour;
    for (const auto& n : names) {
        bool drop = false;
        if (!dropBefore.empty() && n < dropBefore) {
            drop = true;
        } else if (n < hourlyBefore) {
            string hour = n.substr(0, 11);  // YYYYMMDD-HH
            if (hour == keptHour) drop = true;
            else keptHour = hour;
        }
        if (drop && fs::remove(dir / n, ec)) ++removed;
    }
    return removed;
}

void pruneData(const fs::path& dataDir, int keepDays) {
    auto now = chrono::system_clock::now();
    string dropDate  = keepDays > 0
        ? fmtTimePoint(now - chrono::hours(24) * keepDays, "%Y-%m-%d") : "";
    string dropStamp = keepDays > 0
        ? fmtTimePoint(now - chrono::hours(24) * keepDays, "%Y%m%d-%H%M%S") : "";
    string hourlyBefore = fmtTimePoint(now - chrono::hours(24), "%Y%m%d-%H%M%S");

    error_code ec;
    int removed = 0;
    for (auto& appDir : fs::directory_iterator(dataDir, ec)) {
        error_code e2;
        if (!appDir.is_directory(e2)) continue;  // skips tokens.json etc.
        if (!dropDate.empty()) {
            // Daily JSONL (heartbeat-YYYY-MM-DD.jsonl / log-YYYY-MM-DD.jsonl):
            // compare the date embedded in the name against the cutoff.
            for (auto& e : fs::directory_iterator(appDir.path(), e2)) {
                string n = e.path().filename().string();
                size_t ps = 0;
                if (n.rfind("heartbeat-", 0) == 0) ps = 10;
                else if (n.rfind("log-", 0) == 0) ps = 4;
                else if (n.rfind("alert-", 0) == 0) ps = 6;
                else continue;
                if (n.size() < ps + 10 + 6 || n.compare(n.size() - 6, 6, ".jsonl") != 0) continue;
                if (n.compare(ps, 10, dropDate) < 0) {
                    error_code e3;
                    if (fs::remove(e.path(), e3)) ++removed;
                }
            }
        }
        removed += thinJpegs(appDir.path() / "thumbs", hourlyBefore, dropStamp);
        for (auto& e : fs::directory_iterator(appDir.path() / "images", e2)) {
            if (e.is_directory(e2)) removed += thinJpegs(e.path(), hourlyBefore, dropStamp);
        }
    }
    if (removed > 0) {
        logNotice("anchorbolt") << "retention: removed " << removed << " stored file(s)"
                                << (keepDays > 0 ? " (keep " + to_string(keepDays) + " days)" : "");
    }
}

// App ids become directory names — restrict to a safe charset.
bool validAppId(const string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
    }
    return id != "." && id != "..";
}

bool parseArgs(const vector<string>& args, ServeOptions& opt) {
    for (size_t i = 0; i < args.size(); ++i) {
        const string& a = args[i];
        auto next = [&](const char* flag) -> optional<string> {
            if (i + 1 >= args.size()) {
                cerr << "anchorbolt serve: " << flag << " needs a value" << endl;
                return nullopt;
            }
            return args[++i];
        };
        if      (a == "--port")              { auto v = next("--port"); if (!v) return false; opt.port = stoi(*v); }
        else if (a == "--ws-port")           { auto v = next("--ws-port"); if (!v) return false; opt.wsPort = stoi(*v); }
        else if (a == "--data")              { auto v = next("--data"); if (!v) return false; opt.dataDir = *v; }
        else if (a == "--keep-days")         { auto v = next("--keep-days"); if (!v) return false; opt.keepDays = stoi(*v); }
        else {
            cerr << "anchorbolt serve: unknown option '" << a << "'" << endl;
            return false;
        }
    }
    return true;
}



// Same U+FFFD-replacing dump as the agent side: stored log text can carry
// invalid UTF-8 (localized toolchains), and a search result must not crash
// the fleet server.
string dumpSafeS(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}
// --- fleet MCP server ---------------------------------------------------------
// POST /mcp: the fleet server is itself an MCP server (same JSON-RPC-over-
// HTTP transport as TrussC apps), so an AI assistant can investigate "the
// Osaka piece crashed last night" end-to-end: search logs, pull screenshot
// history, check health series, and — with the operator role — restart the
// app or call its own MCP tools through the WS passthrough. Auth: Bearer
// operator token (viewer = read-only tools, operator+ = restart/app_call);
// open mode when no operators are registered.

Json mcpText(const string& s) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", s}}})}};
}

Json mcpJson(const Json& j) {
    return mcpText(j.dump(2));
}

Json mcpError(const string& msg) {
    Json r = mcpText(msg);
    r["isError"] = true;
    return r;
}

Json mcpImage(const vector<unsigned char>& jpg, const Json& meta) {
    return Json{{"content", Json::array({
        Json{{"type", "image"}, {"data", toBase64(jpg)}, {"mimeType", "image/jpeg"}},
        Json{{"type", "text"}, {"text", meta.dump()}}})}};
}

// Newest-first list of an app's daily JSONL files with the given prefix.
vector<fs::path> dailyFiles(const fs::path& dataDir, const string& app, const string& prefix) {
    vector<fs::path> out;
    error_code ec;
    for (auto& e : fs::directory_iterator(dataDir / app, ec)) {
        string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.size() > 6 &&
            n.compare(n.size() - 6, 6, ".jsonl") == 0) {
            out.push_back(e.path());
        }
    }
    sort(out.begin(), out.end(), std::greater<>());
    return out;
}

Json fleetToolsList() {
    auto tool = [](const char* name, const char* desc, Json props, Json required) {
        return Json{{"name", name}, {"description", desc},
                    {"inputSchema", Json{{"type", "object"},
                                         {"properties", props},
                                         {"required", required}}}};
    };
    Json appArg = Json{{"app", {{"type", "string"}, {"description", "App id as shown by list_apps"}}}};
    Json tools = Json::array();
    tools.push_back(tool("list_apps",
        "All apps this fleet server knows: id, live (agent WS connected), seconds since last heartbeat, unacked incident count, and the latest health snapshot (fps, uptime, memory, git commit).",
        Json::object(), Json::array()));
    tools.push_back(tool("search_logs",
        "Case-insensitive substring search over an app's stored log lines (app log + supervisor events), newest day first. Great for 'what happened last night': try queries like ERROR, unresponsive, restarting, [update].",
        Json{{"app", appArg["app"]},
             {"query", {{"type", "string"}, {"description", "Substring to match (case-insensitive). Empty matches everything."}}},
             {"days", {{"type", "integer"}, {"description", "How many recent days of files to scan (default 7)"}}},
             {"limit", {{"type", "integer"}, {"description", "Max matching lines returned (default 100)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("tail_logs",
        "The last N stored log lines for an app (app log + supervisor events interleaved as received).",
        Json{{"app", appArg["app"]},
             {"lines", {{"type", "integer"}, {"description", "Line count (default 50)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("get_events",
        "Recent supervisor/app events for an app (restart/up/down/update/stop/alert) from the event store, newest first — the same list the dashboard shows.",
        Json{{"app", appArg["app"]},
             {"limit", {{"type", "integer"}, {"description", "Max events (default 50)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("get_health_history",
        "Recent heartbeat entries for an app (timestamped health snapshots: fps, rssBytes, machine memory, custom status values). Thinned to at most `entries` evenly spaced samples from today's records.",
        Json{{"app", appArg["app"]},
             {"entries", {{"type", "integer"}, {"description", "Max samples returned (default 60)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("list_screenshots",
        "Stored thumbnail timestamps for an app (names are YYYYMMDD-HHMMSS). Recent 24h is complete; older thins to one per hour.",
        Json{{"app", appArg["app"]},
             {"day", {{"type", "string"}, {"description", "Filter to one day, YYYYMMDD (default: all)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("get_screenshot",
        "A stored thumbnail as an image. Omit `at` for the latest; pass a name from list_screenshots (or a prefix like 20260716-03) to see what the installation looked like at that moment.",
        Json{{"app", appArg["app"]},
             {"at", {{"type", "string"}, {"description", "Timestamp name or prefix (default: latest)"}}}},
        Json::array({"app"})));
    tools.push_back(tool("restart_app",
        "Restart the app via its supervisor (operator role). Same path as the dashboard Restart button.",
        Json{{"app", appArg["app"]}}, Json::array({"app"})));
    tools.push_back(tool("app_list_tools",
        "List the app's OWN MCP tools via the live agent passthrough (tc_* standard tools plus any custom ones the app registered).",
        Json{{"app", appArg["app"]}}, Json::array({"app"})));
    tools.push_back(tool("app_call",
        "Call one of the app's own MCP tools through the agent passthrough. Read-only tc_get_* tools relay freely; anything mutating needs the operator role here AND --allow-control on the venue side. Image results come back as images.",
        Json{{"app", appArg["app"]},
             {"tool", {{"type", "string"}, {"description", "Tool name from app_list_tools"}}},
             {"args", {{"type", "object"}, {"description", "Tool arguments (optional)"}}}},
        Json::array({"app", "tool"})));
    return Json{{"tools", tools}};
}

// rank: caller's role rank (3 in open mode). Returns an MCP result object.
Json fleetToolCall(const fs::path& dataDir, const string& name, const Json& args, int rank) {
    auto appArg = [&]() { return args.value("app", ""); };

    if (name == "list_apps") {
        Json list = Json::array();
        auto now = chrono::steady_clock::now();
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            Json h = st.health;
            Json e = {{"id", id},
                      {"live", g_agents.live(id)},
                      {"lastSeenSec", (int)chrono::duration<double>(now - st.lastSeen).count()},
                      {"unackedAlerts", st.unackedAlerts}};
            for (auto key : {"fps", "uptimeSec", "rssBytes", "git", "version"}) {
                if (h.contains(key)) e[key] = h[key];
            }
            list.push_back(e);
        }
        return mcpJson(list);
    }

    string app = appArg();
    if (!validAppId(app)) return mcpError("missing or invalid 'app' (see list_apps)");

    if (name == "search_logs" || name == "tail_logs") {
        bool tailMode = (name == "tail_logs");
        string query = args.value("query", "");
        transform(query.begin(), query.end(), query.begin(), ::tolower);
        int days  = tailMode ? 1 : max(1, args.value("days", 7));
        int limit = tailMode ? max(1, args.value("lines", 50)) : max(1, args.value("limit", 100));
        vector<string> hits;
        auto files = dailyFiles(dataDir, app, "log-");
        for (size_t fi = 0; fi < files.size() && (int)fi < days && (int)hits.size() < limit; ++fi) {
            // Collect per file (oldest line first), then take from the END so
            // "newest first across days" works without loading everything.
            string date = files[fi].filename().string().substr(4, 10);
            vector<string> lines;
            ifstream in(files[fi]);
            string line;
            while (getline(in, line)) {
                Json j;
                try { j = Json::parse(line); } catch (...) { continue; }
                string text = j.value("text", "");
                if (!query.empty()) {
                    string lower = text;
                    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find(query) == string::npos) continue;
                }
                lines.push_back(date + " " + j.value("at", "") + " [" + j.value("src", "app") + "] " + text);
            }
            for (auto it = lines.rbegin(); it != lines.rend() && (int)hits.size() < limit; ++it) {
                hits.push_back(*it);
            }
        }
        if (hits.empty()) return mcpText(tailMode ? "no stored log lines" : "no matches");
        string out;
        for (auto& h : hits) out += h + "\n";
        return mcpText(out);
    }

    if (name == "get_events") {
        int limit = max(1, args.value("limit", 50));
        vector<string> out;
        auto files = dailyFiles(dataDir, app, "alert-");
        for (size_t fi = 0; fi < files.size() && (int)out.size() < limit; ++fi) {
            vector<string> lines;
            ifstream in(files[fi]);
            string line;
            while (getline(in, line)) {
                Json j;
                try { j = Json::parse(line); } catch (...) { continue; }
                lines.push_back(j.value("at", "") + " [" + j.value("event", "alert") + "] " + j.value("text", ""));
            }
            for (auto it = lines.rbegin(); it != lines.rend() && (int)out.size() < limit; ++it) {
                out.push_back(*it);
            }
        }
        if (out.empty()) return mcpText("no events recorded");
        string text;
        for (auto& l : out) text += l + "\n";
        return mcpText(text);
    }

    if (name == "get_health_history") {
        int entries = max(1, args.value("entries", 60));
        auto files = dailyFiles(dataDir, app, "heartbeat-");
        if (files.empty()) return mcpText("no heartbeats recorded");
        vector<Json> all;
        ifstream in(files[0]);
        string line;
        while (getline(in, line)) {
            try { all.push_back(Json::parse(line)); } catch (...) {}
        }
        if (all.empty()) return mcpText("no heartbeats recorded");
        Json out = Json::array();
        size_t step = max<size_t>(1, all.size() / entries);
        for (size_t i = 0; i < all.size(); i += step) out.push_back(all[i]);
        if (out.back() != all.back()) out.push_back(all.back());
        return mcpJson(out);
    }

    if (name == "list_screenshots") {
        string day = args.value("day", "");
        vector<string> names;
        error_code ec;
        for (auto& e : fs::directory_iterator(dataDir / app / "thumbs", ec)) {
            string n = e.path().filename().string();
            if (n.size() == 19 && n.compare(15, 4, ".jpg") == 0) {
                string stamp = n.substr(0, 15);
                if (day.empty() || stamp.rfind(day, 0) == 0) names.push_back(stamp);
            }
        }
        sort(names.begin(), names.end());
        if (names.empty()) return mcpText("no screenshots stored");
        string out;
        for (auto& n : names) out += n + "\n";
        return mcpText(out);
    }

    if (name == "get_screenshot") {
        string at = args.value("at", "");
        vector<string> names;
        error_code ec;
        for (auto& e : fs::directory_iterator(dataDir / app / "thumbs", ec)) {
            string n = e.path().filename().string();
            if (n.size() == 19 && n.compare(15, 4, ".jpg") == 0) names.push_back(n);
        }
        sort(names.begin(), names.end());
        if (names.empty()) return mcpError("no screenshots stored for '" + app + "'");
        string pick;
        if (at.empty()) {
            pick = names.back();
        } else {
            // newest name at or before/matching the prefix; else the closest after
            for (auto& n : names) {
                if (n.substr(0, at.size()) <= at) pick = n;
            }
            if (pick.empty()) pick = names.front();
        }
        ifstream in(dataDir / app / "thumbs" / pick, ios::binary);
        vector<unsigned char> jpg((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
        if (jpg.empty()) return mcpError("could not read " + pick);
        return mcpImage(jpg, Json{{"app", app}, {"at", pick.substr(0, 15)}});
    }

    if (name == "restart_app") {
        if (rank < 2) return mcpError("restart_app needs the operator role");
        Json r = g_agents.command(app, Json{{"action", "restart"}});
        return r.value("ok", false) ? mcpJson(r["result"])
                                    : mcpError(r.value("error", "restart failed"));
    }

    if (name == "app_list_tools") {
        Json r = g_agents.command(app, Json{{"action", "list_tools"}});
        return r.value("ok", false) ? mcpJson(r["result"])
                                    : mcpError(r.value("error", "agent not reachable"));
    }

    if (name == "app_call") {
        string toolName = args.value("tool", "");
        if (toolName.empty()) return mcpError("'tool' is required (see app_list_tools)");
        // Read-only passthrough for viewers; anything else needs operator here
        // (the venue's --allow-control gate still applies on the agent side).
        if (toolName.rfind("tc_get_", 0) != 0 && rank < 2) {
            return mcpError("'" + toolName + "' is not read-only; it needs the operator role");
        }
        Json r = g_agents.command(app, Json{{"action", "call"},
                                            {"tool", toolName},
                                            {"args", args.value("args", Json::object())}});
        if (!r.value("ok", false)) return mcpError(r.value("error", "tool call failed"));
        Json result = r["result"];
        // The agent folds app image blocks into {data, mimeType, ...}; unfold
        // back into a real image block so the AI sees the picture.
        if (result.is_object() && result.contains("data") && result.contains("mimeType") &&
            result["data"].is_string()) {
            Json meta = Json::object();
            for (auto key : {"width", "height"}) {
                if (result.contains(key)) meta[key] = result[key];
            }
            return Json{{"content", Json::array({
                Json{{"type", "image"}, {"data", result["data"]}, {"mimeType", result["mimeType"]}},
                Json{{"type", "text"}, {"text", meta.dump()}}})}};
        }
        return mcpJson(result);
    }

    return mcpError("unknown tool '" + name + "'");
}

// Dashboard: one self-contained page, no external assets. Polls /api/apps and
// refreshes each card's thumbnail whenever its upload sequence changes.
// Clicking a card opens a detail view: live thumbnail, app-published status
// values, time-series graphs (fps / memory / mode=graph customs, fed by
// /api/history), and custom statusImage streams.
const char* kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AnchorBolt</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #16181d; color: #d4d7dd;
         font: 14px/1.5 -apple-system, "Segoe UI", Roboto, sans-serif; }
  header { display: flex; align-items: baseline; gap: 12px;
           padding: 14px 20px; border-bottom: 1px solid #2a2e36; }
  header h1 { margin: 0; font-size: 17px; font-weight: 600; letter-spacing: .03em; }
  header .sub { color: #7d838e; font-size: 12px; }
  #grid { display: grid; gap: 16px; padding: 20px;
          grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); }
  .card { background: #1e2128; border: 1px solid #2a2e36; border-radius: 10px;
          overflow: hidden; cursor: pointer; }
  .card:hover { border-color: #3d4450; }
  .card.stale { border-color: #7a3030; }
  .thumbWrap { position: relative; aspect-ratio: 16 / 10; background: #0e0f12; }
  .thumbWrap img { position: absolute; inset: 0; width: 100%; height: 100%;
                   object-fit: contain; }
  .thumbWrap .none { position: absolute; inset: 0; display: flex;
                     align-items: center; justify-content: center;
                     color: #4a4f59; font-size: 12px; }
  .meta { padding: 10px 14px; display: flex; justify-content: space-between;
          align-items: baseline; gap: 8px; }
  .meta .name { font-weight: 600; overflow: hidden; text-overflow: ellipsis;
                white-space: nowrap; }
  .meta .stats { color: #7d838e; font-size: 12px; white-space: nowrap;
                 overflow: hidden; text-overflow: ellipsis; }
  .abadge { background: #4a1d1d; color: #ff8a80; border: 1px solid #7a3030;
            border-radius: 10px; font-size: 11px; padding: 0 8px; flex: none;
            margin-left: 6px; }
  #dEvWrap { background: #101216; border: 1px solid #262b34; border-radius: 8px; }
  #dEvHead { display: flex; align-items: center; gap: 10px; padding: 8px 12px;
             border-bottom: 1px solid #21252d; }
  #dEvHead .glabel { font-size: 12px; color: #9aa3b2; flex: 1; }
  #dAckBtn { background: #191c22; color: #9aa3b2; border: 1px solid #2a2e36;
             border-radius: 5px; font-size: 12px; padding: 2px 12px; cursor: pointer; }
  #dAckBtn:hover { color: #d4d7dd; }
  #dEvents { max-height: 180px; overflow-y: auto; padding: 6px 0;
             font: 12px/1.6 ui-monospace, "SF Mono", Menlo, monospace; }
  #dEvents .ev { padding: 0 12px; white-space: pre-wrap; word-break: break-all; }
  #dEvents .ev .lt { color: #565c66; margin-right: 8px; }
  #dEvents .ev .tag { display: inline-block; min-width: 52px; margin-right: 8px;
                      font-weight: 600; }
  .ev-restart .tag, .ev-down .tag, .ev-alert .tag { color: #ff8a80; }
  .ev-up .tag     { color: #4ecb71; }
  .ev-update .tag { color: #79b8ff; }
  .ev-stop .tag   { color: #8d939e; }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
         margin-right: 6px; background: #3fb950; vertical-align: baseline;
         flex: none; }
  .stale .dot, .dot.bad { background: #f85149; }
  #empty { color: #4a4f59; text-align: center; padding: 80px 20px; }
  #logoutBtn { background: none; border: 1px solid #2a2e36; color: #7d838e;
               border-radius: 5px; font-size: 11px; padding: 2px 10px; cursor: pointer; }
  #logoutBtn:hover { color: #d4d7dd; }
  #login { position: fixed; inset: 0; background: #16181d; z-index: 100;
           display: flex; align-items: center; justify-content: center; }
  #login[hidden] { display: none; }
  #loginBox { display: flex; flex-direction: column; gap: 12px; width: 320px; }
  #loginBox h2 { margin: 0 0 4px; font-size: 18px; text-align: center;
                 letter-spacing: .04em; }
  #loginTok { background: #1e2128; border: 1px solid #2a2e36; border-radius: 6px;
              color: #d4d7dd; padding: 9px 12px; font-size: 14px; outline: none; }
  #loginTok:focus { border-color: #3d4450; }
  #loginBtn { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
              border-radius: 6px; font-size: 14px; padding: 8px; cursor: pointer; }
  #loginBtn:hover { background: #223449; }
  #loginErr { color: #ff8a80; font-size: 12px; text-align: center; min-height: 16px; }

  /* ---- detail view ---- */
  #detail { position: fixed; inset: 0; background: rgba(10,11,14,.75);
            display: flex; align-items: flex-start; justify-content: center;
            padding: 4vh 16px; overflow-y: auto; z-index: 10; }
  #detail[hidden] { display: none; }
  #dPanel { background: #1b1e24; border: 1px solid #323844; border-radius: 12px;
            width: min(860px, 100%); margin-bottom: 4vh; }
  .dhead { display: flex; align-items: center; gap: 10px; padding: 14px 18px;
           border-bottom: 1px solid #2a2e36; }
  .dhead h2 { margin: 0; font-size: 16px; }
  .dhead .stats { color: #7d838e; font-size: 12px; flex: 1;
                  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  #dClose { background: none; border: none; color: #7d838e; font-size: 22px;
            cursor: pointer; padding: 0 4px; line-height: 1; }
  #dClose:hover { color: #d4d7dd; }
  .dbody { padding: 18px; display: flex; flex-direction: column; gap: 18px; }
  #dThumbWrap { aspect-ratio: 16 / 10; background: #0e0f12; border-radius: 8px;
                position: relative; overflow: hidden; max-height: 380px; }
  #dThumbWrap img { position: absolute; inset: 0; width: 100%; height: 100%;
                    object-fit: contain; }
  #dValues { display: flex; flex-wrap: wrap; gap: 8px; }
  #dValues:empty { display: none; }
  .chip { background: #242832; border: 1px solid #323844; border-radius: 6px;
          padding: 4px 10px; font-size: 13px; }
  .chip b { color: #9aa3b2; font-weight: 500; margin-right: 6px; }
  .graph { background: #14161b; border: 1px solid #262b34; border-radius: 8px;
           padding: 10px 12px 6px; }
  .graph .glabel { display: flex; justify-content: space-between;
                   font-size: 12px; color: #9aa3b2; margin-bottom: 4px; }
  .graph .gval { color: #d4d7dd; font-weight: 600; }
  .graph canvas { width: 100%; height: 88px; display: block; }
  #dGraphs { display: flex; flex-direction: column; gap: 10px; }
  #dImages { display: grid; gap: 12px;
             grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); }
  #dImages:empty { display: none; }
  #dImages figure { margin: 0; background: #14161b; border: 1px solid #262b34;
                    border-radius: 8px; overflow: hidden; }
  #dImages img { width: 100%; display: block; }
  #dImages figcaption { padding: 6px 10px; font-size: 12px; color: #9aa3b2; }
  .sectionTitle { font-size: 12px; color: #6b727e; text-transform: uppercase;
                  letter-spacing: .08em; margin: 0 0 -8px; }
  #dLogWrap { background: #101216; border: 1px solid #262b34; border-radius: 8px; }
  #dLogHead { display: flex; align-items: center; gap: 10px; padding: 8px 12px;
              border-bottom: 1px solid #21252d; }
  #dLogHead .glabel { font-size: 12px; color: #9aa3b2; flex: none; }
  #dLogFilter { flex: 1; background: #191c22; border: 1px solid #2a2e36;
                border-radius: 5px; color: #d4d7dd; font-size: 12px;
                padding: 3px 8px; outline: none; min-width: 60px; }
  #dLogFilter:focus { border-color: #3d4450; }
  #dLogErr { background: #191c22; color: #7d838e; border: 1px solid #2a2e36;
             border-radius: 5px; font-size: 12px; padding: 3px 10px;
             cursor: pointer; flex: none; }
  #dLogErr.on { background: #4a1d1d; color: #ff8a80; border-color: #7a3030; }
  #dLog { height: 260px; overflow-y: auto; padding: 6px 0;
          font: 11px/1.55 ui-monospace, "SF Mono", Menlo, monospace; }
  #dLog .ll { padding: 0 12px; white-space: pre-wrap; word-break: break-all;
              color: #aeb4bf; }
  #dLog .ll.err  { color: #ff8a80; }
  #dLog .ll.warn { color: #e3b341; }
  #dLog .ll.sup  { color: #7ea6d9; }
  #dLog .ll .lt  { color: #565c66; margin-right: 8px; }
  #dLog .empty   { color: #4a4f59; padding: 12px; }
  .liveChip { font-size: 11px; border-radius: 4px; padding: 1px 7px; flex: none;
              background: #1a2f22; color: #4ecb71; border: 1px solid #2b4a35; }
  .liveChip.off { background: #2a2224; color: #8d6a6e; border-color: #443338; }
  #dRestart { background: #33251a; color: #e0a06a; border: 1px solid #55402c;
              border-radius: 6px; font-size: 12px; padding: 4px 12px;
              cursor: pointer; flex: none; }
  #dRestart:hover { background: #40301f; }
  #dRestart:disabled { opacity: .45; cursor: default; }
  #dUpdate { background: #1a2f22; color: #4ecb71; border: 1px solid #2b4a35;
             border-radius: 6px; font-size: 12px; padding: 4px 12px;
             cursor: pointer; flex: none; }
  #dUpdate:hover { background: #223c2b; }
  #dUpdate:disabled { opacity: .45; cursor: default; }
  #dRollback { background: none; color: #7d838e; border: 1px solid #323844;
               border-radius: 6px; font-size: 12px; padding: 4px 10px;
               cursor: pointer; flex: none; }
  #dRollback:hover { color: #d4d7dd; }
  #dRollback:disabled { opacity: .45; cursor: default; }
  #dConsole { background: #101216; border: 1px solid #262b34; border-radius: 8px;
              padding: 10px 12px; display: flex; flex-direction: column; gap: 8px; }
  #dConsole .row { display: flex; gap: 8px; }
  #dTool { background: #191c22; border: 1px solid #2a2e36; border-radius: 5px;
           color: #d4d7dd; font-size: 12px; padding: 4px 8px; flex: 1; min-width: 0; }
  #dArgs { background: #191c22; border: 1px solid #2a2e36; border-radius: 5px;
           color: #d4d7dd; padding: 4px 8px; flex: 2; min-width: 0;
           font: 11px ui-monospace, Menlo, monospace; }
  #dRun { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
          border-radius: 5px; font-size: 12px; padding: 4px 14px; cursor: pointer; }
  #dRun:hover { background: #223449; }
  #dResult { margin: 0; max-height: 180px; overflow: auto; color: #aeb4bf;
             font: 11px/1.5 ui-monospace, Menlo, monospace; white-space: pre-wrap;
             word-break: break-all; }
  #dResult:empty { display: none; }
  #dResult.err { color: #ff8a80; }
  #dLiveBtn { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
              border-radius: 6px; font-size: 12px; padding: 4px 12px;
              cursor: pointer; flex: none; }
  #dLiveBtn:hover { background: #223449; }
  #dLiveBtn.on { background: #2c405a; color: #cfe6ff; }
  #dLiveWrap[hidden] { display: none; }
  #dLiveBar { display: flex; align-items: center; gap: 10px; margin-bottom: 8px;
              font-size: 12px; color: #9aa3b2; }
  #dLiveStatus { flex: 1; color: #7d838e; }
  #dCtlToggle { display: inline-flex; align-items: center; gap: 5px;
                color: #e0a06a; cursor: pointer; user-select: none; }
  #dCtlToggle[hidden] { display: none; }
  #dLiveClose { background: none; color: #7d838e; border: 1px solid #323844;
                border-radius: 5px; font-size: 12px; padding: 3px 10px; cursor: pointer; }
  #dLiveClose:hover { color: #d4d7dd; }
  #dLiveStage { position: relative; aspect-ratio: 16 / 10; background: #0e0f12;
                border-radius: 8px; overflow: hidden; max-height: 460px;
                display: flex; align-items: center; justify-content: center; }
  #dLiveImg { max-width: 100%; max-height: 100%; object-fit: contain;
              outline: none; display: block; }
  #dLiveStage.ctl #dLiveImg { cursor: crosshair; }
  #dLiveStage.ctl { outline: 2px solid #55402c; outline-offset: -2px; }
</style>
</head>
<body>
<header>
  <h1>AnchorBolt</h1>
  <span class="sub" id="summary"></span>
  <span style="flex:1"></span>
  <span class="sub" id="who"></span>
  <button id="logoutBtn" hidden>logout</button>
</header>
<div id="grid"></div>
<div id="empty" hidden>no apps have reported yet &mdash; run
  <code>anchorbolt start &lt;app&gt; --server &lt;this url&gt;</code></div>

<div id="login" hidden>
  <div id="loginBox">
    <h2>AnchorBolt</h2>
    <input id="loginTok" type="password" placeholder="operator token (op-...)" autocomplete="off">
    <button id="loginBtn">Sign in</button>
    <div id="loginErr"></div>
  </div>
</div>

<div id="detail" hidden>
  <div id="dPanel">
    <div class="dhead">
      <span class="dot" id="dDot"></span>
      <h2 id="dTitle"></h2>
      <span class="stats" id="dStats"></span>
      <span class="liveChip off" id="dLive">offline</span>
      <button id="dLiveBtn" title="live view + remote control">Live</button>
      <button id="dUpdate" disabled title="git pull + build + restart on the venue machine">Update</button>
      <button id="dRollback" disabled title="restore the previous binary">Roll back</button>
      <button id="dRestart" disabled>Restart</button>
      <button id="dClose" title="close">&times;</button>
    </div>
    <div class="dbody">
      <div id="dLiveWrap" hidden>
        <div id="dLiveBar">
          <span class="glabel">live view</span>
          <label id="dCtlToggle" hidden><input type="checkbox" id="dCtl"> control</label>
          <span id="dLiveStatus">waiting for stream...</span>
          <button id="dLiveClose">close</button>
        </div>
        <div id="dLiveStage"><img id="dLiveImg" alt="" draggable="false" tabindex="0"></div>
      </div>
      <div id="dThumbWrap"><img hidden></div>
      <div id="dValues"></div>
      <div id="dEvWrap" hidden>
        <div id="dEvHead">
          <span class="glabel">events</span>
          <button id="dAckBtn" title="acknowledge — clears the wall badge">Clear</button>
        </div>
        <div id="dEvents"></div>
      </div>
      <div id="dGraphs"></div>
      <div id="dImages"></div>
      <div id="dLogWrap">
        <div id="dLogHead">
          <span class="glabel">log</span>
          <input id="dLogFilter" type="text" placeholder="filter...">
          <button id="dLogErr" title="show only ERROR / FATAL lines">errors</button>
        </div>
        <div id="dLog"><div class="empty">no log lines yet</div></div>
      </div>
      <div id="dConsole">
        <div class="row">
          <select id="dTool"><option value="">loading tools...</option></select>
          <input id="dArgs" type="text" placeholder='args JSON, e.g. {"width": 256}'>
          <button id="dRun">Run</button>
        </div>
        <pre id="dResult"></pre>
      </div>
    </div>
  </div>
</div>

)HTML"
R"HTML(<script>
const STALE_SEC = 10;
let myRole = null;   // null = open mode (everything allowed)

function showLogin() {
  document.getElementById('login').hidden = false;
  document.getElementById('loginTok').focus();
}

async function doLogin() {
  const tok = document.getElementById('loginTok').value.trim();
  if (!tok) return;
  const r = await fetch('/api/login', { method: 'POST', body: JSON.stringify({ token: tok }) });
  if (r.ok) location.reload();
  else document.getElementById('loginErr').textContent = 'invalid token';
}
document.getElementById('loginBtn').addEventListener('click', doLogin);
document.getElementById('loginTok').addEventListener('keydown', e => {
  if (e.key === 'Enter') doLogin();
});
document.getElementById('logoutBtn').addEventListener('click', async () => {
  await fetch('/api/logout', { method: 'POST' });
  location.reload();
});

// The server enforces roles regardless; this only hides what a viewer
// cannot use anyway.
function applyRole(me) {
  if (me.open) return;
  myRole = me.role;
  document.getElementById('who').textContent = me.name + ' (' + me.role + ')';
  document.getElementById('logoutBtn').hidden = false;
  if (me.role === 'viewer') {
    for (const id of ['dRestart', 'dUpdate', 'dRollback', 'dAckBtn', 'dConsole']) {
      const el = document.getElementById(id);
      if (el) el.style.display = 'none';
    }
  }
}

(async () => {
  try {
    const r = await fetch('/api/me');
    if (r.status === 401) { showLogin(); return; }
    if (r.ok) applyRole(await r.json());
  } catch {}
})();
const seq = {};        // app id -> last rendered thumbSeq (wall)
let lastApps = [];
let detailId = null;
let dThumbSeq = -1;

function fmtUptime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? `${h}h${m}m` : m > 0 ? `${m}m${s % 60}s` : `${s}s`;
}

function statsLine(app) {
  const h = app.health || {};
  const parts = [];
  if (h.fps !== undefined) parts.push(h.fps.toFixed(0) + ' fps');
  if (h.width) parts.push(h.width + 'x' + h.height);
  if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
  if (app.ageSec > STALE_SEC) parts.push('last seen ' + Math.floor(app.ageSec) + 's ago');
  return parts.join(' · ');
}

function card(app) {
  const el = document.createElement('div');
  el.className = 'card';
  el.id = 'app-' + app.id;
  el.innerHTML = `
    <div class="thumbWrap"><span class="none">no thumbnail</span><img hidden></div>
    <div class="meta">
      <span class="name"><span class="dot"></span><span class="label"></span><span class="abadge" hidden></span></span>
      <span class="stats"></span>
    </div>`;
  el.addEventListener('click', () => openDetail(app.id));
  return el;
}

// ---- detail view ----

function openDetail(id) {
  detailId = id;
  dThumbSeq = -1;
  logCursor = 0;
  evCursor = 0;
  logErrOnly = false;
  document.getElementById('dLogErr').classList.remove('on');
  document.getElementById('dEvWrap').hidden = true;
  document.getElementById('dEvents').replaceChildren();
  document.getElementById('dImages').replaceChildren();
  document.getElementById('dLog').replaceChildren(
    Object.assign(document.createElement('div'), {className: 'empty', textContent: 'no log lines yet'}));
  document.getElementById('dResult').textContent = '';
  stopLiveView();                     // a fresh app starts with live view closed
  document.getElementById('detail').hidden = false;
  loadTools(id);
  renderDetail();
}

function closeDetail() {
  stopLiveView();
  detailId = null;
  document.getElementById('detail').hidden = true;
}
document.getElementById('dClose').addEventListener('click', closeDetail);
document.getElementById('detail').addEventListener('click', e => {
  if (e.target === document.getElementById('detail')) closeDetail();
});
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeDetail(); });

function fmtVal(v) {
  if (Math.abs(v - Math.round(v)) < 1e-9) return String(Math.round(v));
  const a = Math.abs(v);
  return a >= 100 ? v.toFixed(0) : a >= 1 ? v.toFixed(1) : v.toFixed(2);
}

// Nice round step covering range/target (Heckbert-style: 1, 2, 2.5, 5 x 10^n)
function niceStep(range, target) {
  const raw = range / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 2.5, 5]) if (raw <= m * mag) return m * mag;
  return 10 * mag;
}

function niceTimeStepSec(spanSec, target) {
  for (const s of [5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200])
    if (spanSec / target <= s) return s;
  return 14400;
}

// Line graph with nice-number bounds, subtle grid (values + time), and a
// hover crosshair showing the value/time under the cursor.
function makeGraph(canvas, pts, times, color) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const valid = pts.filter(p => p != null && isFinite(p));
  if (!valid.length) return;

  let lo = Math.min(...valid), hi = Math.max(...valid);
  if (hi - lo < 1e-9) { const pad = Math.max(Math.abs(hi) * 0.1, 1); lo -= pad; hi += pad; }
  const step = niceStep(hi - lo, 3);
  const min = Math.floor(lo / step) * step;
  const max = Math.ceil(hi / step) * step;

  const PADB = 13;                  // room for time labels
  const PADT = 12;                  // room for the topmost value label
  const plotH = h - PADB;
  const n = Math.max(pts.length - 1, 1);
  const X = i => i / n * w;
  const Y = v => plotH - 3 - (v - min) / (max - min) * (plotH - 3 - PADT);

  const hasTime = times.length > 1 && isFinite(times[0]) && times[times.length - 1] > times[0];
  const t0 = hasTime ? times[0] : 0, t1 = hasTime ? times[times.length - 1] : 1;

  function render(hoverI) {
    ctx.clearRect(0, 0, w, h);
    ctx.font = '9px -apple-system, "Segoe UI", sans-serif';

    // horizontal grid + value labels (on nice-number ticks)
    for (let v = min; v <= max + step * 1e-6; v += step) {
      const y = Y(v);
      ctx.strokeStyle = 'rgba(255,255,255,0.06)';
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      ctx.fillStyle = '#565c66';
      ctx.fillText(fmtVal(v), 4, y - 3);
    }

    // vertical grid + time labels (on nice time steps)
    if (hasTime) {
      const ts = niceTimeStepSec((t1 - t0) / 1000, 4) * 1000;
      for (let t = Math.ceil(t0 / ts) * ts; t <= t1; t += ts) {
        const x = (t - t0) / (t1 - t0) * w;
        ctx.strokeStyle = 'rgba(255,255,255,0.05)';
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, plotH); ctx.stroke();
        const d = new Date(t);
        const hh = String(d.getHours()).padStart(2, '0');
        const mm = String(d.getMinutes()).padStart(2, '0');
        const lbl = ts < 60000 ? hh + ':' + mm + ':' + String(d.getSeconds()).padStart(2, '0')
                               : hh + ':' + mm;
        ctx.fillStyle = '#565c66';
        ctx.fillText(lbl, Math.min(x + 3, w - 40), h - 3);
      }
    }

    // the series
    ctx.strokeStyle = color; ctx.lineWidth = 1.5;
    ctx.beginPath();
    let started = false;
    pts.forEach((p, i) => {
      if (p == null || !isFinite(p)) { started = false; return; }
      started ? ctx.lineTo(X(i), Y(p)) : ctx.moveTo(X(i), Y(p));
      started = true;
    });
    ctx.stroke();

    // hover crosshair + readout
    if (hoverI >= 0 && hoverI < pts.length && pts[hoverI] != null && isFinite(pts[hoverI])) {
      const x = X(hoverI), y = Y(pts[hoverI]);
      ctx.strokeStyle = 'rgba(255,255,255,0.25)';
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, plotH); ctx.stroke();
      ctx.fillStyle = color;
      ctx.beginPath(); ctx.arc(x, y, 3, 0, 2 * Math.PI); ctx.fill();

      let lbl = fmtVal(pts[hoverI]);
      if (hasTime && isFinite(times[hoverI])) {
        const d = new Date(times[hoverI]);
        lbl += '  ' + String(d.getHours()).padStart(2, '0') + ':'
             + String(d.getMinutes()).padStart(2, '0') + ':'
             + String(d.getSeconds()).padStart(2, '0');
      }
      ctx.font = '11px -apple-system, "Segoe UI", sans-serif';
      const tw = ctx.measureText(lbl).width;
      const lx = (x + 10 + tw + 6 > w) ? x - tw - 14 : x + 10;
      ctx.fillStyle = 'rgba(14,16,20,0.92)';
      ctx.fillRect(lx - 5, 4, tw + 10, 17);
      ctx.strokeStyle = 'rgba(255,255,255,0.18)';
      ctx.strokeRect(lx - 5, 4, tw + 10, 17);
      ctx.fillStyle = '#e6e9ee';
      ctx.fillText(lbl, lx, 16);
    }
  }

  render(-1);
  canvas.onmousemove = e => {
    const r = canvas.getBoundingClientRect();
    render(Math.round((e.clientX - r.left) / r.width * n));
  };
  canvas.onmouseleave = () => render(-1);
}

async function renderDetail() {
  if (!detailId) return;
  const app = lastApps.find(a => a.id === detailId);
  if (!app) return;

  const stale = app.ageSec > STALE_SEC;
  document.getElementById('dDot').classList.toggle('bad', stale);
  document.getElementById('dTitle').textContent = app.id;
  const live = document.getElementById('dLive');
  live.textContent = app.live ? 'live' : 'offline';
  live.classList.toggle('off', !app.live);
  document.getElementById('dRestart').disabled = !app.live;
  document.getElementById('dUpdate').disabled = !app.live;
  document.getElementById('dRollback').disabled = !app.live;
  const h = app.health || {};
  const extra = [];
  if (h.git) extra.push('@' + h.git);
  if (h.version) extra.push(h.version);
  const mem = h.rssBytes ?? h.memoryBytes;
  if (mem) extra.push((mem / 1048576).toFixed(0) + ' MB');
  document.getElementById('dStats').textContent =
    [statsLine(app), ...extra].filter(Boolean).join(' · ');

  // Live thumbnail (same seq mechanism as the wall)
  if (app.thumbSeq > 0 && dThumbSeq !== app.thumbSeq) {
    dThumbSeq = app.thumbSeq;
    const img = document.querySelector('#dThumbWrap img');
    img.src = '/api/thumb/' + encodeURIComponent(app.id) + '?s=' + app.thumbSeq;
    img.hidden = false;
  }

  // Status chips (mode=status custom values)
  const custom = (h.custom && h.custom.values) || [];
  const chips = custom.filter(v => v.mode === 'status').map(v => {
    const el = document.createElement('span');
    el.className = 'chip';
    const b = document.createElement('b');
    b.textContent = v.name;
    el.appendChild(b);
    el.appendChild(document.createTextNode(String(v.value)));
    return el;
  });
  document.getElementById('dValues').replaceChildren(...chips);

  // Custom images (statusImage streams)
  syncImages(app);

  await pollLog(app.id);
  await pollEvents(app.id);

  // Graphs: fps + memory + every mode=graph custom value
  let hist = [];
  try {
    hist = await (await fetch('/api/history/' + encodeURIComponent(app.id))).json();
  } catch { return; }
  if (detailId !== app.id) return;   // closed/switched while fetching

  // Memory graphs: process RSS (the leak-hunting number) and machine-wide
  // available (the OOM-exoneration number). Sokol-tracked bytes stay in the
  // JSONL for deep dives but aren't graphed.
  const graphs = [
    { label: 'fps',        color: '#58a6ff', get: e => e.health && e.health.fps },
    { label: 'process MB', color: '#d29922',
      get: e => e.health ? (e.health.rssBytes ?? e.health.memoryBytes) / 1048576 : null },
  ];
  if (h.machine && h.machine.memAvailBytes) {
    graphs.push({ label: 'machine free MB', color: '#8b949e',
      get: e => e.health && e.health.machine ? e.health.machine.memAvailBytes / 1048576 : null });
  }
  const palette = ['#3fb950', '#f778ba', '#a371f7', '#ff7b72', '#79c0ff'];
  custom.filter(v => v.mode === 'graph').forEach((v, i) => {
    graphs.push({
      label: v.name,
      color: palette[i % palette.length],
      get: e => {
        const vs = e.health && e.health.custom && e.health.custom.values;
        if (!vs) return null;
        const found = vs.find(x => x.name === v.name);
        return found && typeof found.value === 'number' ? found.value : null;
      },
    });
  });

  const times = hist.map(e => Date.parse(e.at));
  const cont = document.getElementById('dGraphs');
  // Don't rebuild while the pointer is inspecting a graph — a rebuild would
  // wipe the hover crosshair mid-read. Data resumes once the mouse leaves.
  if (cont.matches(':hover')) return;
  cont.replaceChildren(...graphs.map(g => {
    const el = document.createElement('div');
    el.className = 'graph';
    el.innerHTML = `<div class="glabel"><span></span><span class="gval"></span></div><canvas></canvas>`;
    el.querySelector('.glabel span').textContent = g.label;
    return el;
  }));
  graphs.forEach((g, i) => {
    const el = cont.children[i];
    const pts = hist.map(g.get);
    makeGraph(el.querySelector('canvas'), pts, times, g.color);
    const last = [...pts].reverse().find(p => p != null && isFinite(p));
    el.querySelector('.gval').textContent = last != null ? fmtVal(+last) : '-';
  });
}

)HTML"
R"HTML(// ---- remote control (restart + tool console) ----

async function sendCommand(id, body) {
  try {
    return await (await fetch('/api/command/' + encodeURIComponent(id), {
      method: 'POST', body: JSON.stringify(body),
    })).json();
  } catch {
    return { ok: false, error: 'server unreachable' };
  }
}

document.getElementById('dRestart').addEventListener('click', async () => {
  if (!detailId || !confirm('Restart "' + detailId + '"?')) return;
  const btn = document.getElementById('dRestart');
  btn.disabled = true;
  const r = await sendCommand(detailId, { action: 'restart' });
  showResult(r);
  setTimeout(() => { btn.disabled = false; }, 3000);
});

document.getElementById('dUpdate').addEventListener('click', async () => {
  if (!detailId || !confirm('Update "' + detailId + '"?\n\nRuns the update pipeline on the venue machine ' +
      '(default: git pull → trusscli update → trusscli build) while the app keeps running, ' +
      'then restarts only if the build succeeds. Progress streams into the log below.')) return;
  const btn = document.getElementById('dUpdate');
  btn.disabled = true;
  showResult(await sendCommand(detailId, { action: 'update' }));
  setTimeout(() => { btn.disabled = false; }, 3000);
});

document.getElementById('dRollback').addEventListener('click', async () => {
  if (!detailId || !confirm('Roll back "' + detailId + '" to the previous binary and restart?')) return;
  const btn = document.getElementById('dRollback');
  btn.disabled = true;
  showResult(await sendCommand(detailId, { action: 'rollback' }));
  setTimeout(() => { btn.disabled = false; }, 3000);
});

async function loadTools(id) {
  const sel = document.getElementById('dTool');
  sel.replaceChildren(new Option('loading tools...', ''));
  const r = await sendCommand(id, { action: 'list_tools' });
  if (detailId !== id) return;
  const tools = (r.ok && r.result && r.result.tools) ? r.result.tools : [];
  sel.replaceChildren(...(tools.length
    ? tools.map(t => {
        const o = new Option(t.name, t.name);
        o.title = t.description || '';
        return o;
      })
    : [new Option(r.error || 'no tools (agent offline?)', '')]));
}

function showResult(r) {
  const pre = document.getElementById('dResult');
  pre.classList.toggle('err', !r.ok);
  let payload = r.ok ? (r.result ?? {}) : (r.error || 'failed');
  // Images come back as base64 — show them, don't dump the string.
  if (r.ok && payload && typeof payload === 'object' && payload.data && payload.mimeType) {
    pre.classList.remove('err');
    pre.replaceChildren(Object.assign(new Image(), {
      src: 'data:' + payload.mimeType + ';base64,' + payload.data,
      style: 'max-width:100%;',
    }));
    return;
  }
  pre.textContent = typeof payload === 'string' ? payload : JSON.stringify(payload, null, 2);
}

document.getElementById('dRun').addEventListener('click', async () => {
  if (!detailId) return;
  const tool = document.getElementById('dTool').value;
  if (!tool) return;
  let args = {};
  const raw = document.getElementById('dArgs').value.trim();
  if (raw) {
    try {
      args = JSON.parse(raw);
    } catch {
      showResult({ ok: false, error: 'args is not valid JSON' });
      return;
    }
  }
  showResult({ ok: true, result: '...' });
  showResult(await sendCommand(detailId, { action: 'call', tool, args }));
});

)HTML"
R"HTML(// ---- live view + remote control ----

let liveOn = false;
let liveTimer = null;

document.getElementById('dLiveBtn').addEventListener('click', () => {
  if (liveOn) stopLiveView(); else startLiveView();
});
document.getElementById('dLiveClose').addEventListener('click', stopLiveView);

function startLiveView() {
  if (!detailId) return;
  liveOn = true;
  document.getElementById('dLiveBtn').classList.add('on');
  document.getElementById('dLiveWrap').hidden = false;
  document.getElementById('dLiveStatus').textContent = 'waiting for stream...';
  // Control is operator/admin only; myRole===null means open mode (allowed).
  document.getElementById('dCtlToggle').hidden = (myRole === 'viewer');
  document.getElementById('dCtl').checked = false;
  document.getElementById('dLiveStage').classList.remove('ctl');
  pollLiveFrame();
}

function stopLiveView() {
  liveOn = false;
  if (liveTimer) { clearTimeout(liveTimer); liveTimer = null; }
  document.getElementById('dLiveBtn').classList.remove('on');
  document.getElementById('dLiveWrap').hidden = true;
  liveDown = null;
  const img = document.getElementById('dLiveImg');
  img.onload = img.onerror = null;
  img.removeAttribute('src');
}

// Swap src each tick; schedule the next swap only on load/error so a slow
// frame can't pile up requests. The agent auto-stops 10s after these polls
// cease, so leaving the view IS the stop signal.
function pollLiveFrame() {
  if (!liveOn || !detailId) return;
  const img = document.getElementById('dLiveImg');
  img.onload = () => {
    document.getElementById('dLiveStatus').textContent = 'streaming';
    scheduleLive();
  };
  img.onerror = () => { scheduleLive(); };   // 404 until the first frame lands
  img.src = '/api/live/' + encodeURIComponent(detailId) + '?s=' + Date.now();
}
function scheduleLive() {
  if (liveOn) liveTimer = setTimeout(pollLiveFrame, 110);   // ~9 fps
}

function ctlOn() { return document.getElementById('dCtl').checked; }

document.getElementById('dCtl').addEventListener('change', e => {
  document.getElementById('dLiveStage').classList.toggle('ctl', e.target.checked);
  if (e.target.checked) document.getElementById('dLiveImg').focus();
});

// Map a pointer event on the frame to the app's real pixel coords. The frame
// is downscaled for bandwidth, so scale by the app's true window size from
// health (falls back to the image's natural size).
function liveToApp(e) {
  const img = document.getElementById('dLiveImg');
  const app = lastApps.find(a => a.id === detailId);
  const h = (app && app.health) || {};
  const aw = h.width || img.naturalWidth || img.clientWidth || 1;
  const ah = h.height || img.naturalHeight || img.clientHeight || 1;
  const r = img.getBoundingClientRect();
  const x = Math.max(0, Math.min(aw - 1, Math.round((e.clientX - r.left) / r.width * aw)));
  const y = Math.max(0, Math.min(ah - 1, Math.round((e.clientY - r.top) / r.height * ah)));
  return { x, y };
}

let liveDown = null;   // in-progress press: { start, sx, sy, pressSent }
const liveImg = document.getElementById('dLiveImg');

liveImg.addEventListener('mousedown', e => {
  if (!ctlOn()) return;
  e.preventDefault();
  liveDown = { start: liveToApp(e), sx: e.clientX, sy: e.clientY, pressSent: false };
});
// Track on window so a drag that leaves the frame still completes.
window.addEventListener('mousemove', e => {
  if (!liveDown) return;
  if (!liveDown.pressSent) {
    // Click vs drag: only once the pointer travels a few px is it a drag
    // (press at the origin, then move); a still press+release stays a click.
    if (Math.abs(e.clientX - liveDown.sx) < 3 && Math.abs(e.clientY - liveDown.sy) < 3) return;
    liveDown.pressSent = true;
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_press',
                            args: { x: liveDown.start.x, y: liveDown.start.y } });
  }
  const p = liveToApp(e);
  sendCommand(detailId, { action: 'call', tool: 'tc_mouse_move', args: { x: p.x, y: p.y } });
});
window.addEventListener('mouseup', e => {
  if (!liveDown) return;
  const down = liveDown;
  liveDown = null;
  if (down.pressSent) {
    const p = liveToApp(e);
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_release', args: { x: p.x, y: p.y } });
  } else {
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_click',
                            args: { x: down.start.x, y: down.start.y } });
  }
});

// Keyboard passthrough as sokol keycodes — only while control is on and the
// frame has focus. stopPropagation keeps Escape etc. from leaking to the
// dashboard's own shortcuts.
const SOKOL_KEY = { 'ArrowRight': 262, 'ArrowLeft': 263, 'ArrowDown': 264, 'ArrowUp': 265,
                    ' ': 32, 'Spacebar': 32, 'Enter': 257, 'Escape': 256 };
function sokolKey(e) {
  if (e.key in SOKOL_KEY) return SOKOL_KEY[e.key];
  if (e.key.length === 1) {
    const c = e.key.toUpperCase().charCodeAt(0);
    if ((c >= 65 && c <= 90) || (c >= 48 && c <= 57)) return c;   // A-Z, 0-9
  }
  return null;
}
liveImg.addEventListener('keydown', e => {
  if (!ctlOn()) return;
  const code = sokolKey(e);
  if (code == null) return;
  e.preventDefault();
  e.stopPropagation();
  sendCommand(detailId, { action: 'call', tool: 'tc_key_press', args: { key: code } });
});

)HTML"
R"HTML(// ---- event list (supervisor + app alerts) ----

let evCursor = 0;

async function pollEvents(id) {
  let r;
  try {
    r = await (await fetch('/api/alert/' + encodeURIComponent(id) + '?after=' + evCursor)).json();
  } catch { return; }
  if (detailId !== id) return;
  evCursor = r.next;
  if (!r.alerts.length) return;
  document.getElementById('dEvWrap').hidden = false;
  const box = document.getElementById('dEvents');
  for (const a of r.alerts) {
    const el = document.createElement('div');
    el.className = 'ev ev-' + a.event;
    const t = document.createElement('span');
    t.className = 'lt';
    t.textContent = (a.at || '').replace('T', ' ');
    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = a.event;
    el.appendChild(t);
    el.appendChild(tag);
    el.appendChild(document.createTextNode(a.text));
    box.prepend(el);            // newest on top
  }
  while (box.children.length > 100) box.lastChild.remove();
}

document.getElementById('dAckBtn').addEventListener('click', async () => {
  if (!detailId) return;
  try {
    await fetch('/api/alert/' + encodeURIComponent(detailId) + '/clear', { method: 'POST' });
  } catch {}
});

// ---- log panel ----

let logCursor = 0;

function lineClass(l) {
  if (/\[ERROR\]|\[FATAL\]/.test(l.text)) return 'll err';
  if (/\[WARNING\]/.test(l.text)) return 'll warn';
  if (l.src === 'sup') return 'll sup';
  return 'll';
}

let logErrOnly = false;

function logLineVisible(el) {
  if (logErrOnly && !el.classList.contains('err')) return false;
  const filter = document.getElementById('dLogFilter').value.toLowerCase();
  return !filter || el.dataset.text.includes(filter);
}

function applyLogFilter() {
  for (const el of document.querySelectorAll('#dLog .ll')) {
    el.style.display = logLineVisible(el) ? '' : 'none';
  }
}

function appendLogLines(lines) {
  const box = document.getElementById('dLog');
  const empty = box.querySelector('.empty');
  if (empty && lines.length) empty.remove();
  const follow = box.scrollTop + box.clientHeight >= box.scrollHeight - 8;
  for (const l of lines) {
    const el = document.createElement('div');
    el.className = lineClass(l);
    const t = document.createElement('span');
    t.className = 'lt';
    t.textContent = l.src === 'sup' ? l.at + ' ⚑' : l.at;
    el.appendChild(t);
    el.appendChild(document.createTextNode(l.text));
    el.dataset.text = (l.at + ' ' + l.text).toLowerCase();
    if (!logLineVisible(el)) el.style.display = 'none';
    box.appendChild(el);
  }
  while (box.children.length > 600) box.firstChild.remove();
  if (follow && lines.length) box.scrollTop = box.scrollHeight;
}

document.getElementById('dLogErr').addEventListener('click', () => {
  logErrOnly = !logErrOnly;
  document.getElementById('dLogErr').classList.toggle('on', logErrOnly);
  applyLogFilter();
});

async function pollLog(id) {
  let r;
  try {
    r = await (await fetch('/api/log/' + encodeURIComponent(id) + '?after=' + logCursor)).json();
  } catch { return; }
  if (detailId !== id) return;
  logCursor = r.next;
  appendLogLines(r.lines);
}

document.getElementById('dLogFilter').addEventListener('input', applyLogFilter);

function syncImages(app) {
  const cont = document.getElementById('dImages');
  const names = Object.keys(app.images || {});
  for (const el of [...cont.children]) {
    if (!names.includes(el.dataset.name)) el.remove();
  }
  for (const n of names) {
    let fig = [...cont.children].find(el => el.dataset.name === n);
    if (!fig) {
      fig = document.createElement('figure');
      fig.dataset.name = n;
      fig.innerHTML = '<img><figcaption></figcaption>';
      fig.querySelector('figcaption').textContent = n;
      cont.appendChild(fig);
    }
    const s = app.images[n];
    const img = fig.querySelector('img');
    if (img.dataset.seq != s) {
      img.dataset.seq = s;
      img.src = '/api/image/' + encodeURIComponent(app.id) + '/' + encodeURIComponent(n) + '?s=' + s;
    }
  }
}

// ---- wall ----

async function refresh() {
  let apps;
  try {
    const r = await fetch('/api/apps');
    if (r.status === 401) { showLogin(); return; }
    apps = await r.json();
  } catch { return; }
  lastApps = apps;

  const grid = document.getElementById('grid');
  document.getElementById('empty').hidden = apps.length > 0;
  const alive = new Set();

  for (const app of apps) {
    alive.add('app-' + app.id);
    let el = document.getElementById('app-' + app.id);
    if (!el) { el = card(app); grid.appendChild(el); }

    el.classList.toggle('stale', app.ageSec > STALE_SEC);
    el.querySelector('.label').textContent = app.id;
    el.querySelector('.stats').textContent = statsLine(app);
    const badge = el.querySelector('.abadge');
    badge.hidden = !(app.alerts > 0);
    if (app.alerts > 0) badge.textContent = '\u26a0 ' + app.alerts;

    if (app.thumbSeq > 0 && seq[app.id] !== app.thumbSeq) {
      seq[app.id] = app.thumbSeq;
      const img = el.querySelector('img');
      img.src = '/api/thumb/' + encodeURIComponent(app.id) + '?s=' + app.thumbSeq;
      img.hidden = false;
      el.querySelector('.none').hidden = true;
    }
  }
  for (const el of [...grid.children]) {
    if (!alive.has(el.id)) el.remove();
  }
  const ok = apps.filter(a => a.ageSec <= STALE_SEC).length;
  document.getElementById('summary').textContent =
    apps.length === 0 ? '' : `${ok}/${apps.length} healthy`;

  renderDetail();
}

refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTML";

} // namespace

int cmdServe(const vector<string>& args) {
    ServeOptions opt;
    if (!parseArgs(args, opt)) return 1;
    if (opt.wsPort == 0) opt.wsPort = opt.port + 1;

    fs::path dataDir = fs::absolute(opt.dataDir);
    error_code ec;
    fs::create_directories(dataDir, ec);
    if (ec) {
        cerr << "anchorbolt serve: cannot create data dir " << dataDir.string() << endl;
        return 1;
    }

    // Agent authentication: with at least one token registered, every ingest
    // request and WS hello must present a valid token for its app id.
    // No tokens = open mode (the zero-config path on trusted networks).
    auto authOk = [dataDir](const httplib::Request& req, const string& appId) {
        string d = dataDir.string();
        if (!token::enforcementEnabled(d)) return true;
        string h = req.get_header_value("Authorization");
        const string prefix = "Bearer ";
        if (h.rfind(prefix, 0) != 0) return false;
        return token::verify(d, appId, h.substr(prefix.size()));
    };

    // Dashboard authentication: with any operator registered, every read and
    // command endpoint wants the login cookie. The cookie carries the token
    // itself (HttpOnly — set by /api/login, sent automatically by <img> tags
    // too, which Authorization headers can't do); verification re-hashes per
    // request, so revoking an operator locks them out on their next poll.
    auto cookieToken = [](const httplib::Request& req) -> string {
        string c = req.get_header_value("Cookie");
        size_t pos = c.find("abtoken=");
        if (pos == string::npos) return "";
        pos += 8;
        size_t end = c.find(';', pos);
        return c.substr(pos, end == string::npos ? string::npos : end - pos);
    };
    // Minimum-role gate: viewer=1 operator=2 admin=3. Open mode admits all.
    auto requireRole = [dataDir, cookieToken](const httplib::Request& req,
                                              httplib::Response& res, int minRank) {
        string d = dataDir.string();
        if (!token::operatorsEnabled(d)) return true;
        auto op = token::verifyOperator(d, cookieToken(req));
        if (op && token::roleRank(op->role) >= minRank) return true;
        // 401 = not signed in (UI shows the login overlay); 403 = signed in
        // but this role can't do that (UI hides those controls anyway).
        res.status = op ? 403 : 401;
        res.set_content(op ? "forbidden" : "unauthorized", "text/plain");
        return false;
    };

    // WS hub: agents say hello {type, app, token}; replies to commands come
    // back as {type:"result", id, ...}. Callbacks run on socket threads.
    g_agents.hub.onMessage = [dataDir](int clientId, const string& text) {
        Json m;
        try {
            m = Json::parse(text);
        } catch (...) {
            return;
        }
        string type = m.value("type", "");
        if (type == "hello") {
            string app = m.value("app", "");
            string tok = m.value("token", "");
            string d = dataDir.string();
            if (!validAppId(app) ||
                (token::enforcementEnabled(d) && !token::verify(d, app, tok))) {
                g_agents.hub.sendText(clientId, Json({{"type", "error"},
                                                      {"error", "auth failed"}}).dump());
                g_agents.hub.closeClient(clientId);
                logWarning("anchorbolt") << "agent hello rejected (app '" << app << "')";
                return;
            }
            lock_guard<mutex> lock(g_agents.mutex_);
            // A reconnect replaces any stale mapping.
            if (auto old = g_agents.byApp.find(app); old != g_agents.byApp.end()) {
                g_agents.byClient.erase(old->second);
            }
            g_agents.byApp[app] = clientId;
            g_agents.byClient[clientId] = app;
            logNotice("anchorbolt") << "agent connected: " << app;
        } else if (type == "result") {
            uint64_t id = m.value("id", (uint64_t)0);
            shared_ptr<promise<Json>> prom;
            {
                lock_guard<mutex> lock(g_agents.mutex_);
                auto it = g_agents.pending.find(id);
                if (it == g_agents.pending.end()) return;
                prom = it->second;
                g_agents.pending.erase(it);
            }
            m.erase("type");
            m.erase("id");
            prom->set_value(m);
        } else if (type == "frame") {
            // Live view frame: base64 JPEG. Text (not a binary WS frame)
            // because WsHub only decodes text and the agent's WS client emits a
            // TEXT opcode even for binary payloads — base64 keeps it honest and
            // costs nothing extra (the screenshot tool already produced base64).
            string data = m.value("data", "");
            if (data.empty()) return;
            string app;
            {
                lock_guard<mutex> lock(g_agents.mutex_);
                auto it = g_agents.byClient.find(clientId);
                if (it == g_agents.byClient.end()) return;
                app = it->second;
            }
            auto jpg = fromBase64(data);
            if (jpg.empty()) return;
            lock_guard<mutex> lock(g_appsMutex);
            auto& st = g_apps[app];
            st.liveFrame = std::move(jpg);
            st.liveSeq++;
            st.liveAt = chrono::steady_clock::now();
        }
    };
    g_agents.hub.onClosed = [](int clientId) {
        lock_guard<mutex> lock(g_agents.mutex_);
        auto it = g_agents.byClient.find(clientId);
        if (it == g_agents.byClient.end()) return;
        logNotice("anchorbolt") << "agent disconnected: " << it->second;
        g_agents.byApp.erase(it->second);
        g_agents.byClient.erase(it);
    };
    if (!g_agents.hub.start(opt.wsPort)) {
        cerr << "anchorbolt serve: failed to listen on ws port " << opt.wsPort << "\n"
             << "If the port looks free, on Windows it may sit inside a reserved\n"
             << "range (Hyper-V/WSL2 exclusions). Check with:\n"
             << "  netsh interface ipv4 show excludedportrange protocol=tcp\n"
             << "and pick a port outside those ranges with --port (ws = port+1)." << endl;
        return 1;
    }

    httplib::Server svr;

    // Login: token in, HttpOnly cookie out. The dashboard shows an overlay
    // on any 401 and posts here.
    svr.Post("/api/login", [dataDir](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return;
        }
        string tok = body.value("token", "");
        auto op = token::verifyOperator(dataDir.string(), tok);
        if (!op) {
            res.status = 401;
            res.set_content("invalid token", "text/plain");
            return;
        }
        res.set_header("Set-Cookie", "abtoken=" + tok +
                       "; HttpOnly; SameSite=Lax; Path=/; Max-Age=2592000");
        res.set_content(Json({{"name", op->name}, {"role", op->role}}).dump(),
                        "application/json");
    });


    // The fleet server is itself an MCP server. Auth: Bearer operator token
    // (Authorization header — MCP clients can send headers; the dashboard
    // cookie also works). Open mode admits everyone at admin rank.
    svr.Post("/mcp", [dataDir, cookieToken](const httplib::Request& req, httplib::Response& res) {
        int rank = 3;
        string d = dataDir.string();
        if (token::operatorsEnabled(d)) {
            string tok;
            string h = req.get_header_value("Authorization");
            if (h.rfind("Bearer ", 0) == 0) tok = h.substr(7);
            if (tok.empty()) tok = cookieToken(req);
            auto op = token::verifyOperator(d, tok);
            if (!op) {
                res.status = 401;
                res.set_content("unauthorized (Bearer operator token required)", "text/plain");
                return;
            }
            rank = token::roleRank(op->role);
        }
        Json rpc;
        try {
            rpc = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("invalid JSON-RPC", "text/plain");
            return;
        }
        string method = rpc.value("method", "");
        if (method.rfind("notifications/", 0) == 0) {
            res.status = 202;   // fire-and-forget per MCP-over-HTTP
            return;
        }
        Json reply = {{"jsonrpc", "2.0"}};
        if (rpc.contains("id")) reply["id"] = rpc["id"];
        if (method == "initialize") {
            reply["result"] = {{"protocolVersion", rpc["params"].value("protocolVersion", "2024-11-05")},
                               {"capabilities", {{"tools", Json::object()}}},
                               {"serverInfo", {{"name", "anchorbolt"}, {"version", "0.0.1"}}}};
        } else if (method == "tools/list") {
            reply["result"] = fleetToolsList();
        } else if (method == "tools/call") {
            string name = rpc["params"].value("name", "");
            Json args = rpc["params"].value("arguments", Json::object());
            reply["result"] = fleetToolCall(dataDir, name, args, rank);
        } else if (method == "ping") {
            reply["result"] = Json::object();
        } else {
            reply["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
        }
        res.set_content(dumpSafeS(reply), "application/json");
    });

    svr.Post("/api/logout", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", "abtoken=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
        res.set_content("ok", "text/plain");
    });

    // Who am I? Drives the login overlay and role-based UI hiding.
    svr.Get("/api/me", [dataDir, cookieToken](const httplib::Request& req, httplib::Response& res) {
        string d = dataDir.string();
        if (!token::operatorsEnabled(d)) {
            res.set_content(Json({{"open", true}}).dump(), "application/json");
            return;
        }
        auto op = token::verifyOperator(d, cookieToken(req));
        if (!op) {
            res.status = 401;
            res.set_content("unauthorized", "text/plain");
            return;
        }
        res.set_content(Json({{"open", false}, {"name", op->name}, {"role", op->role}}).dump(),
                        "application/json");
    });


    // Dashboard -> agent command bridge. Blocks until the agent replies (or
    // 15s). Restart is handled by the supervisor itself; 'call' relays an MCP
    // tool call to the app (agent-side allowlist applies).
    svr.Post(R"(/api/command/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 2)) return;
        string id = req.matches[1];
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("invalid JSON", "text/plain");
            return;
        }
        string action = body.value("action", "");
        if (action != "restart" && action != "call" && action != "list_tools" &&
            action != "update" && action != "rollback") {
            res.status = 400;
            res.set_content("unknown action", "text/plain");
            return;
        }
        Json reply = g_agents.command(id, body);
        res.set_content(reply.dump(), "application/json");
    });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kDashboardHtml, "text/html");
    });

    // Agent push: health JSON. Body = get_health result + {"app": "<id>"}.
    svr.Post("/api/heartbeat", [dataDir, authOk](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("invalid JSON", "text/plain");
            return;
        }
        string id = body.value("app", "");
        if (!validAppId(id)) {
            res.status = 400;
            res.set_content("missing or invalid 'app' id", "text/plain");
            return;
        }
        if (!authOk(req, id)) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        body.erase("app");
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto& st = g_apps[id];
            st.health = body;
            st.lastSeen = chrono::steady_clock::now();
        }
        // Append to the day's JSONL (DB-free storage; rotation by filename).
        fs::path dir = dataDir / id;
        error_code ec;
        fs::create_directories(dir, ec);
        Json line = {{"at", getTimestampString("%Y-%m-%dT%H:%M:%S")}, {"health", body}};
        ofstream out(dir / ("heartbeat-" + getTimestampString("%Y-%m-%d") + ".jsonl"), ios::app);
        out << line.dump() << "\n";
        res.set_content("ok", "text/plain");
    });

    // Agent push: latest thumbnail as raw JPEG bytes (no base64 on the wire).
    svr.Post(R"(/api/thumb/([^/]+))", [dataDir, authOk](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        if (!validAppId(id)) {
            res.status = 400;
            res.set_content("invalid app id", "text/plain");
            return;
        }
        if (!authOk(req, id)) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("empty body", "text/plain");
            return;
        }
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto& st = g_apps[id];
            st.thumb.assign(req.body.begin(), req.body.end());
            ++st.thumbSeq;
        }
        fs::path dir = dataDir / id / "thumbs";
        error_code ec;
        fs::create_directories(dir, ec);
        ofstream out(dir / (getTimestampString("%Y%m%d-%H%M%S") + ".jpg"), ios::binary);
        out.write(req.body.data(), (streamsize)req.body.size());
        res.set_content("ok", "text/plain");
    });

    // Agent push: custom statusImage as raw JPEG (same shape as thumbnails,
    // but keyed by app + image name).
    svr.Post(R"(/api/image/([^/]+)/([^/]+))", [dataDir, authOk](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        string name = req.matches[2];
        if (!validAppId(id) || !validAppId(name)) {
            res.status = 400;
            res.set_content("invalid app or image id", "text/plain");
            return;
        }
        if (!authOk(req, id)) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("empty body", "text/plain");
            return;
        }
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto& slot = g_apps[id].images[name];
            slot.jpeg.assign(req.body.begin(), req.body.end());
            ++slot.seq;
        }
        fs::path dir = dataDir / id / "images" / name;
        error_code ec;
        fs::create_directories(dir, ec);
        ofstream out(dir / (getTimestampString("%Y%m%d-%H%M%S") + ".jpg"), ios::binary);
        out.write(req.body.data(), (streamsize)req.body.size());
        res.set_content("ok", "text/plain");
    });

    svr.Get(R"(/api/image/([^/]+)/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        string name = req.matches[2];
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it == g_apps.end()) { res.status = 404; return; }
        auto img = it->second.images.find(name);
        if (img == it->second.images.end() || img->second.jpeg.empty()) { res.status = 404; return; }
        res.set_content((const char*)img->second.jpeg.data(), img->second.jpeg.size(), "image/jpeg");
    });

    // Agent push: forwarded log lines (app log + supervisor events). Arrives
    // even while the app itself is down — the supervisor keeps reporting.
    // A batch claims a byte range of one local daily file: {src, file, start,
    // end, lines}. The agent freezes an unconfirmed batch and resends it
    // verbatim (at-least-once), so a retry whose ack got lost re-arrives with
    // the same (file, end) — confirmed here without appending twice.
    svr.Post(R"(/api/log/([^/]+))", [dataDir, authOk](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        if (!validAppId(id)) { res.status = 400; return; }
        if (!authOk(req, id)) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return;
        }
        string src = body.value("src", "app");
        string bfile = body.value("file", "");
        int64_t bend = body.value("end", (int64_t)0);
        string now = getTimestampString("%H:%M:%S");
        fs::path dir = dataDir / id;
        error_code ec;
        fs::create_directories(dir, ec);
        lock_guard<mutex> lock(g_appsMutex);
        auto& st = g_apps[id];
        auto& seen = st.logSeen[src];
        if (!bfile.empty() && seen.first == bfile && bend <= seen.second) {
            res.set_content("ok (duplicate)", "text/plain");
            return;
        }
        ofstream out(dir / ("log-" + getTimestampString("%Y-%m-%d") + ".jsonl"), ios::app);
        for (auto& l : body.value("lines", Json::array())) {
            if (!l.is_string()) continue;
            LogLine line{st.nextLogSeq++, now, src, l.get<string>()};
            out << Json({{"at", line.at}, {"src", line.src}, {"text", line.text}}).dump() << "\n";
            st.logs.push_back(std::move(line));
            if (st.logs.size() > kLogRingSize) st.logs.pop_front();
        }
        if (!bfile.empty()) seen = {bfile, bend};
        res.set_content("ok", "text/plain");
    });

    // Log lines newer than ?after=<seq> (0 = everything buffered).
    svr.Get(R"(/api/log/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        uint64_t after = 0;
        if (req.has_param("after")) {
            try { after = stoull(req.get_param_value("after")); } catch (...) {}
        }
        Json lines = Json::array();
        uint64_t next = after;
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it != g_apps.end()) {
            for (const auto& l : it->second.logs) {
                if (l.seq <= after) continue;
                lines.push_back({{"seq", l.seq}, {"at", l.at}, {"src", l.src}, {"text", l.text}});
                next = l.seq;
            }
        }
        res.set_content(Json({{"next", next}, {"lines", lines}}).dump(), "application/json");
    });

    // Agent push: app-raised operator alerts (mcp::alert, drained from
    // tc_get_alerts). Stored like logs, but surfaced loudly: wall badge +
    // dedicated list in the detail view.
    svr.Post(R"(/api/alert/([^/]+))", [dataDir, authOk](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        if (!validAppId(id)) { res.status = 400; return; }
        if (!authOk(req, id)) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return;
        }
        fs::path dir = dataDir / id;
        error_code ec;
        fs::create_directories(dir, ec);
        ofstream out(dir / ("alert-" + getTimestampString("%Y-%m-%d") + ".jsonl"), ios::app);
        string now = getTimestampString("%H:%M:%S");
        lock_guard<mutex> lock(g_appsMutex);
        auto& st = g_apps[id];
        for (auto& a : body.value("alerts", Json::array())) {
            // LogLine.src carries the event type (restart/up/down/update/
            // stop/alert). Only incidents bump the wall badge — recoveries
            // and clean stops are list-only, or a normal day would glow red.
            string ev = a.value("event", "alert");
            LogLine line{st.nextAlertSeq++, a.value("at", now), ev, a.value("text", "")};
            if (line.text.empty()) continue;
            out << Json({{"at", line.at}, {"event", ev}, {"text", line.text}}).dump() << "\n";
            st.alerts.push_back(std::move(line));
            if (st.alerts.size() > 100) st.alerts.pop_front();
            if (ev == "restart" || ev == "down" || ev == "alert") ++st.unackedAlerts;
        }
        res.set_content("ok", "text/plain");
    });

    // Alerts newer than ?after=<seq> (0 = everything buffered).
    svr.Get(R"(/api/alert/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        uint64_t after = 0;
        if (req.has_param("after")) {
            try { after = stoull(req.get_param_value("after")); } catch (...) {}
        }
        Json lines = Json::array();
        uint64_t next = after;
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it != g_apps.end()) {
            for (const auto& a : it->second.alerts) {
                if (a.seq <= after) continue;
                lines.push_back({{"seq", a.seq}, {"at", a.at},
                                 {"event", a.src}, {"text", a.text}});
                next = a.seq;
            }
        }
        res.set_content(Json({{"next", next}, {"alerts", lines}}).dump(), "application/json");
    });

    // Operator pressed Clear: acknowledged — the wall badge goes quiet. The
    // list itself (and the JSONL on disk) stays.
    svr.Post(R"(/api/alert/([^/]+)/clear)", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 2)) return;
        string id = req.matches[1];
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it != g_apps.end()) it->second.unackedAlerts = 0;
        res.set_content("ok", "text/plain");
    });

    // Recent heartbeat history for the detail-view graphs: tail of today's
    // JSONL as a JSON array (raw line concatenation — every line is already
    // a JSON object). ~1200 entries = 1h at the default 3s poll.
    svr.Get(R"(/api/history/([^/]+))", [dataDir, requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!validAppId(id)) { res.status = 400; return; }
        ifstream in(dataDir / id / ("heartbeat-" + getTimestampString("%Y-%m-%d") + ".jsonl"));
        deque<string> lines;
        string line;
        while (getline(in, line)) {
            if (line.empty()) continue;
            lines.push_back(line);
            if (lines.size() > 1200) lines.pop_front();
        }
        string body = "[";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) body += ",";
            body += lines[i];
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    svr.Get("/api/apps", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        Json list = Json::array();
        auto now = chrono::steady_clock::now();
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            double age = chrono::duration<double>(now - st.lastSeen).count();
            Json images = Json::object();
            for (auto& [name, slot] : st.images) images[name] = slot.seq;
            list.push_back({{"id", id},
                            {"ageSec", age},
                            {"thumbSeq", st.thumbSeq},
                            {"images", images},
                            {"live", g_agents.live(id)},
                            {"alerts", st.unackedAlerts},
                            {"health", st.health}});
        }
        res.set_content(list.dump(), "application/json");
    });

    svr.Get(R"(/api/thumb/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it == g_apps.end() || it->second.thumb.empty()) {
            res.status = 404;
            res.set_content("no thumbnail", "text/plain");
            return;
        }
        res.set_content((const char*)it->second.thumb.data(), it->second.thumb.size(), "image/jpeg");
    });

    // Live view: latest frame as image/jpeg, and — with zero explicit
    // lifecycle — the driver for the stream itself. Each poll that finds no
    // recent live_start (>5s) fires one at the agent, so the browser polling
    // this endpoint IS what keeps the agent streaming; stop polling and the
    // agent's own 10s idle timeout tears it down. 404 when no fresh frame yet
    // (the very first poll after arming is expected to 404).
    svr.Get(R"(/api/live/([^/]+))", [requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        auto now = chrono::steady_clock::now();
        vector<unsigned char> frame;
        uint64_t seq = 0;
        bool needStart = false, known = false;
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto it = g_apps.find(id);
            if (it != g_apps.end()) {
                known = true;
                auto& st = it->second;
                if (now - st.lastLiveStart > chrono::seconds(5)) {
                    st.lastLiveStart = now;
                    needStart = true;
                }
                if (!st.liveFrame.empty() && now - st.liveAt <= chrono::seconds(5)) {
                    frame = st.liveFrame;   // copy out; don't hold the lock over the send
                    seq = st.liveSeq;
                }
            }
        }
        // Re-arm outside g_appsMutex — command() takes g_agents.mutex_ and
        // blocks on the agent's reply (live_start answers immediately; the 3s
        // is only a safety cap).
        if (needStart && known) g_agents.command(id, Json{{"action", "live_start"}}, 3);
        if (frame.empty()) {
            res.status = 404;
            res.set_content("no live frame", "text/plain");
            return;
        }
        res.set_header("X-Live-Seq", to_string(seq));
        res.set_content((const char*)frame.data(), frame.size(), "image/jpeg");
    });

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // svr.listen() blocks; a watcher thread turns the signal flag into stop().
    thread stopWatcher([&svr]() {
        while (!g_stop) this_thread::sleep_for(chrono::milliseconds(100));
        svr.stop();
    });

    // Retention: prune stored data at startup and hourly after that
    // (sliced sleep so shutdown stays snappy).
    thread pruner([keepDays = opt.keepDays, dataDir]() {
        while (!g_stop) {
            pruneData(dataDir, keepDays);
            for (int i = 0; i < 36000 && !g_stop; ++i) {
                this_thread::sleep_for(chrono::milliseconds(100));
            }
        }
    });

    logNotice("anchorbolt") << "fleet server on http://localhost:" << opt.port
                            << " (ws " << opt.wsPort << ", data " << dataDir.string()
                            << (token::enforcementEnabled(dataDir.string())
                                ? ", agent tokens ENFORCED)" : ", open mode)");
    // Dual-stack listen: Windows resolves `localhost` to ::1 first, so an
    // IPv4-only listener made agent POSTs fail silently there (the WS channel
    // still connected — a confusing half-alive state). "::" with the httplib
    // default v6only=false accepts both stacks; fall back to plain IPv4 on
    // systems without IPv6.
    bool ok = svr.listen("::", opt.port);
    if (!ok && !g_stop) {
        logNotice("anchorbolt") << "IPv6 listen unavailable; falling back to IPv4";
        ok = svr.listen("0.0.0.0", opt.port);
    }
    bool stoppedBySignal = g_stop.load();
    g_stop = true;
    stopWatcher.join();
    pruner.join();
    g_agents.hub.stop();

    if (!ok && !stoppedBySignal) {
        cerr << "anchorbolt serve: failed to listen on port " << opt.port << "\n"
             << "If the port looks free, on Windows it may sit inside a reserved\n"
             << "range (Hyper-V/WSL2 exclusions). Check with:\n"
             << "  netsh interface ipv4 show excludedportrange protocol=tcp\n"
             << "and pick a port outside those ranges with --port." << endl;
        return 1;
    }
    logNotice("anchorbolt") << "fleet server stopped";
    return 0;
}
