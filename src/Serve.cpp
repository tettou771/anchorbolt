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
#include <optional>
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
    int64_t  lastSeenUnix = 0;                      // wall-clock last report (0 = never); persisted
    bool     hidden = false;                        // hidden from the wall (restorable)
    vector<unsigned char> thumb;                    // latest JPEG
    uint64_t thumbSeq = 0;                          // bumped per upload (cache busting)
    map<string, ImageSlot> images;                  // custom statusImage uploads
    deque<LogLine> logs;                            // ring buffer, newest last
    uint64_t nextLogSeq = 1;
    bool preloaded = false;                         // disk log tail loaded this session
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

// After a serve restart the in-memory log ring is empty, but the venue keeps its
// own cursor and won't re-ship history — so the detail panel would show nothing
// until fresh lines arrive. On an app's first contact this session, seed the ring
// from the tail of its most recent on-disk log-YYYY-MM-DD.jsonl. Done per-app on
// contact (not for every dir at boot) so long-gone venues aren't resurrected.
// Caller holds g_appsMutex.
void preloadAppLogs(const fs::path& dataDir, const string& id, AppState& st) {
    if (st.preloaded) return;
    st.preloaded = true;
    error_code ec;
    fs::path latest;
    string latestName;
    for (auto& e : fs::directory_iterator(dataDir / id, ec)) {
        string n = e.path().filename().string();
        if (n.rfind("log-", 0) == 0 && n.size() >= 6 &&
            n.compare(n.size() - 6, 6, ".jsonl") == 0 && n > latestName) {
            latestName = n;
            latest = e.path();
        }
    }
    if (latest.empty()) return;
    ifstream in(latest);
    if (!in) return;
    deque<string> tail;
    string ln;
    while (getline(in, ln)) {
        if (ln.empty()) continue;
        tail.push_back(std::move(ln));
        if (tail.size() > kLogRingSize) tail.pop_front();
    }
    for (auto& line : tail) {
        try {
            Json j = Json::parse(line);
            st.logs.push_back(LogLine{st.nextLogSeq++, j.value("at", ""),
                                      j.value("src", "app"), j.value("text", "")});
        } catch (...) {}
    }
}

// The known-apps registry: id -> {lastSeen (unix), health, hidden}. Persisting
// it lets the wall survive a serve restart — apps come back as offline cards
// (with their last screenshot + last-seen) so history stays browsable, and the
// hidden flag lives here too. Written on a timer / at shutdown / when hidden
// toggles, never per heartbeat.
fs::path appsRegistryPath(const fs::path& dataDir) { return dataDir / "apps.json"; }

void flushAppRegistry(const fs::path& dataDir) {
    Json reg = Json::object();
    {
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            if (st.lastSeenUnix == 0 && !st.hidden) continue;  // never reported, not hidden
            reg[id] = {{"lastSeen", st.lastSeenUnix}, {"hidden", st.hidden}, {"health", st.health}};
        }
    }
    error_code ec;
    fs::create_directories(dataDir, ec);
    ofstream out(appsRegistryPath(dataDir));
    if (out) out << reg.dump() << "\n";
}

// Load the registry at startup: recreate each app as an offline entry with its
// last-seen reconstructed (so ageSec shows the real staleness) and its newest
// stored thumbnail loaded for the card. Live reports then update them in place.
void loadAppRegistry(const fs::path& dataDir) {
    ifstream in(appsRegistryPath(dataDir));
    if (!in) return;
    Json reg;
    try { reg = Json::parse(in); } catch (...) { return; }
    if (!reg.is_object()) return;
    auto nowSteady = chrono::steady_clock::now();
    int64_t nowUnix = (int64_t)time(nullptr);
    error_code ec;
    lock_guard<mutex> lock(g_appsMutex);
    for (auto& [id, e] : reg.items()) {
        if (!e.is_object()) continue;
        auto& st = g_apps[id];
        st.lastSeenUnix = e.value("lastSeen", (int64_t)0);
        st.hidden = e.value("hidden", false);
        st.health = e.value("health", Json::object());
        int64_t age = nowUnix - st.lastSeenUnix;
        if (st.lastSeenUnix > 0 && age >= 0)
            st.lastSeen = nowSteady - chrono::seconds(age);   // real offline duration
        // Newest stored thumbnail (YYYYMMDD-HHMMSS.jpg) for the offline card.
        fs::path latest; string latestName;
        for (auto& f : fs::directory_iterator(dataDir / id / "thumbs", ec)) {
            string n = f.path().filename().string();
            if (n.size() == 19 && n.compare(15, 4, ".jpg") == 0 && n > latestName) {
                latestName = n; latest = f.path();
            }
        }
        if (!latest.empty()) {
            ifstream tin(latest, ios::binary);
            string data((istreambuf_iterator<char>(tin)), istreambuf_iterator<char>());
            st.thumb.assign(data.begin(), data.end());
            st.thumbSeq = 1;
        }
    }
}

// Parse a thumbnail name "YYYYMMDD-HHMMSS..." to a unix time (local), 0 on fail.
int64_t parseStampName(const string& n) {
    if (n.size() < 15 || n[8] != '-') return 0;
    try {
        struct tm t{};
        t.tm_year = stoi(n.substr(0, 4)) - 1900;
        t.tm_mon  = stoi(n.substr(4, 2)) - 1;
        t.tm_mday = stoi(n.substr(6, 2));
        t.tm_hour = stoi(n.substr(9, 2));
        t.tm_min  = stoi(n.substr(11, 2));
        t.tm_sec  = stoi(n.substr(13, 2));
        t.tm_isdst = -1;
        return (int64_t)mktime(&t);
    } catch (...) { return 0; }
}

// Every provisioned venue (an agent token in tokens.json) is a known app, even
// if it hasn't reported to this build yet — so the wall matches the settings
// Apps list. Seed a g_apps entry for each; ones the registry already filled keep
// their state. For the rest, recover last-seen + the last screenshot from the
// venue's own stored thumbnails (a known app, not a directory scan) so it shows
// as an offline card; a venue with no history stays "waiting for first report".
void loadKnownAgents(const fs::path& dataDir) {
    Json agents = token::listAgents(dataDir.string());
    auto nowSteady = chrono::steady_clock::now();
    int64_t nowUnix = (int64_t)time(nullptr);
    error_code ec;
    lock_guard<mutex> lock(g_appsMutex);
    for (auto& a : agents) {
        string id = a.value("id", "");
        if (id.empty()) continue;
        auto& st = g_apps[id];
        if (st.lastSeenUnix != 0) continue;   // registry already gave it real state
        fs::path latest; string latestName;
        for (auto& f : fs::directory_iterator(dataDir / id / "thumbs", ec)) {
            string n = f.path().filename().string();
            if (n.size() == 19 && n.compare(15, 4, ".jpg") == 0 && n > latestName) {
                latestName = n; latest = f.path();
            }
        }
        if (latest.empty()) continue;         // no history -> waiting for first report
        int64_t ts = parseStampName(latestName);
        if (ts > 0 && ts <= nowUnix) {
            st.lastSeenUnix = ts;
            st.lastSeen = nowSteady - chrono::seconds(nowUnix - ts);
        }
        ifstream tin(latest, ios::binary);
        string data((istreambuf_iterator<char>(tin)), istreambuf_iterator<char>());
        st.thumb.assign(data.begin(), data.end());
        st.thumbSeq = 1;
    }
}

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

// --- 6-digit code brute-force guard -------------------------------------------
// Codes are only a million-wide space, so redemption endpoints (/api/pair,
// /api/login/code) throttle globally: >=10 failures inside a 10-minute window
// locks ALL redemption with 429 until the window cools. In-memory only — a
// restart clears it, which is fine (an attacker gains nothing from a bounce).
mutex g_codeGuardMutex;
int g_codeFails = 0;
chrono::steady_clock::time_point g_codeWindowStart;
constexpr int  kCodeFailLimit  = 10;
constexpr auto kCodeFailWindow = chrono::minutes(10);

bool codeGuardBlocked() {
    lock_guard<mutex> lock(g_codeGuardMutex);
    if (g_codeFails >= kCodeFailLimit) {
        if (chrono::steady_clock::now() - g_codeWindowStart < kCodeFailWindow) return true;
        g_codeFails = 0;  // window cooled off
    }
    return false;
}

void codeGuardFail() {
    lock_guard<mutex> lock(g_codeGuardMutex);
    auto now = chrono::steady_clock::now();
    if (g_codeFails == 0 || now - g_codeWindowStart >= kCodeFailWindow) {
        g_codeWindowStart = now;
        g_codeFails = 0;
    }
    if (++g_codeFails == kCodeFailLimit) {
        logWarning("anchorbolt") << "code brute-force guard tripped: " << kCodeFailLimit
                                 << " failed redemptions — locking all codes for 10 min";
    }
}

// Scope gate for a resolved operator (nullopt = open mode / full access).
bool opSeesApp(const optional<token::Operator>& op, const string& dataDir,
               const string& appId) {
    if (!op) return true;
    return token::inScope(*op, appId, token::groupOf(dataDir, appId));
}

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

// Minimal in-memory ZIP writer (STORE / no compression). The payload is logs +
// already-compressed JPEGs, so deflate would buy almost nothing and a real zip
// library would be a heavy dependency — a stored archive opens on every OS.
struct ZipWriter {
    string buf;
    struct Ent { string name; uint32_t crc, size, offset; };
    vector<Ent> ents;
    static uint32_t crc32(const string& s) {
        static uint32_t T[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                T[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char ch : s) c = T[(c ^ ch) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    static void put16(string& b, uint16_t v) { b += char(v & 0xFF); b += char((v >> 8) & 0xFF); }
    static void put32(string& b, uint32_t v) {
        b += char(v & 0xFF); b += char((v >> 8) & 0xFF);
        b += char((v >> 16) & 0xFF); b += char((v >> 24) & 0xFF);
    }
    void add(const string& name, const string& data) {
        Ent e{name, crc32(data), (uint32_t)data.size(), (uint32_t)buf.size()};
        put32(buf, 0x04034b50); put16(buf, 20); put16(buf, 0); put16(buf, 0);  // ver, flags, store
        put16(buf, 0); put16(buf, 0x21);                                       // dummy time/date (1980-01-01)
        put32(buf, e.crc); put32(buf, e.size); put32(buf, e.size);
        put16(buf, (uint16_t)name.size()); put16(buf, 0);
        buf += name; buf += data;
        ents.push_back(std::move(e));
    }
    string finish() {
        uint32_t cdStart = (uint32_t)buf.size();
        for (auto& e : ents) {
            put32(buf, 0x02014b50); put16(buf, 20); put16(buf, 20); put16(buf, 0); put16(buf, 0);
            put16(buf, 0); put16(buf, 0x21); put32(buf, e.crc); put32(buf, e.size); put32(buf, e.size);
            put16(buf, (uint16_t)e.name.size()); put16(buf, 0); put16(buf, 0);  // name, extra, comment len
            put16(buf, 0); put16(buf, 0); put32(buf, 0);                        // disk, internal, external attrs
            put32(buf, e.offset); buf += e.name;
        }
        uint32_t cdSize = (uint32_t)buf.size() - cdStart;
        put32(buf, 0x06054b50); put16(buf, 0); put16(buf, 0);
        put16(buf, (uint16_t)ents.size()); put16(buf, (uint16_t)ents.size());
        put32(buf, cdSize); put32(buf, cdStart); put16(buf, 0);
        return buf;
    }
};

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
        "Call one of the app's own MCP tools through the agent passthrough. Read-only tc_get_* tools relay freely; mutating tools need the operator role and only exist if the app registered them (e.g. registerDebuggerTools for input injection). Image results come back as images.",
        Json{{"app", appArg["app"]},
             {"tool", {{"type", "string"}, {"description", "Tool name from app_list_tools"}}},
             {"args", {{"type", "object"}, {"description", "Tool arguments (optional)"}}}},
        Json::array({"app", "tool"})));
    return Json{{"tools", tools}};
}

// rank: caller's role rank (3 in open mode). op: resolved operator for scope
// filtering (nullopt = open mode / full visibility). Returns an MCP result.
Json fleetToolCall(const fs::path& dataDir, const string& name, const Json& args,
                   int rank, const optional<token::Operator>& op) {
    auto appArg = [&]() { return args.value("app", ""); };

    if (name == "list_apps") {
        Json list = Json::array();
        auto now = chrono::steady_clock::now();
        string d = dataDir.string();
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            if (!opSeesApp(op, d, id)) continue;  // out-of-scope apps stay hidden
            Json h = st.health;
            Json e = {{"id", id},
                      {"group", token::groupOf(d, id)},
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
    // Out-of-scope apps 404-equivalent: don't leak that the app exists.
    if (!opSeesApp(op, dataDir.string(), app))
        return mcpError("unknown app '" + app + "' (see list_apps)");

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
        // Read-only passthrough for viewers; anything else needs operator here.
        // On the venue, a mutating tool exists only if the app registered it.
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
  .card.stale .thumbWrap img { filter: blur(3px) brightness(.4) grayscale(.4); }
  .offlay { position: absolute; inset: 0; display: flex; flex-direction: column;
            align-items: center; justify-content: center; gap: 3px; text-align: center;
            padding: 8px; text-shadow: 0 1px 4px #000; }
  .offlay[hidden] { display: none; }
  .offlay .who { font-weight: 600; font-size: 15px; color: #eceff3; }
  .offlay .ago { font-size: 12px; color: #ff9a8a; }
  .thumbWrap { position: relative; aspect-ratio: 16 / 10; background: #0e0f12; }
  .thumbWrap img { position: absolute; inset: 0; width: 100%; height: 100%;
                   object-fit: contain; }
  .thumbWrap .none { position: absolute; inset: 0; display: flex;
                     align-items: center; justify-content: center;
                     color: #4a4f59; font-size: 12px; }
  .meta { padding: 10px 14px; display: flex; flex-direction: column; gap: 3px; }
  .meta .name { font-weight: 600; overflow: hidden; text-overflow: ellipsis;
                white-space: nowrap; min-width: 0; }
  .meta .name .label { overflow: hidden; text-overflow: ellipsis; }
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
  .dhead { display: flex; flex-direction: column; gap: 8px; padding: 14px 18px;
           border-bottom: 1px solid #2a2e36; }
  .dTitleRow { display: flex; align-items: center; gap: 8px; }
  .dTitleRow h2 { margin: 0; font-size: 16px; flex: 1; min-width: 0;
                  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .dBtnRow { display: flex; gap: 8px; }
  .dBtnRow[hidden] { display: none; }
  .dStatusRow .stats { color: #7d838e; font-size: 12px;
                       overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  #dClose { background: none; border: none; color: #7d838e; font-size: 22px;
            cursor: pointer; padding: 0 4px; line-height: 1; }
  #dClose:hover { color: #d4d7dd; }
  .dbody { padding: 18px; display: flex; flex-direction: column; gap: 18px; }
  #dThumbWrap { aspect-ratio: 16 / 10; background: #0e0f12; border-radius: 8px;
                position: relative; overflow: hidden; max-height: 380px; }
  #dThumbWrap img { position: absolute; inset: 0; width: 100%; height: 100%;
                    object-fit: contain; }
  #dScrubWrap { display: flex; align-items: center; gap: 8px; margin-top: 6px; }
  #dScrubWrap[hidden] { display: none; }
  #dScrub { flex: 1; accent-color: #58a6ff; cursor: pointer; }
  #dScrubPlay { background: #191c22; color: #9aa3b2; border: 1px solid #2a2e36;
                border-radius: 5px; font-size: 11px; padding: 2px 9px; cursor: pointer; flex: none; }
  #dScrubPlay:hover { color: #d4d7dd; }
  #dScrubTime { font: 11px ui-monospace, Menlo, monospace; color: #7d838e;
                flex: none; min-width: 34px; text-align: right; }
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
  #dLogHead select { background: #191c22; color: #9aa3b2; border: 1px solid #2a2e36;
                     border-radius: 5px; font-size: 12px; padding: 3px 6px;
                     cursor: pointer; flex: none; max-width: 130px; }
  #dLogHead select:hover { color: #d4d7dd; }
  #dLog { height: 260px; overflow-y: auto; padding: 6px 0;
          font: 11px/1.55 ui-monospace, "SF Mono", Menlo, monospace; }
  #dLog .ll { padding: 0 12px; white-space: pre-wrap; word-break: break-all;
              color: #aeb4bf; }
  #dLog .ll.err  { color: #ff8a80; }
  #dLog .ll.warn { color: #e3b341; }
  #dLog .ll.sup  { color: #7ea6d9; }
  #dLog .ll .lt  { color: #565c66; margin-right: 8px; }
  #dLog .empty   { color: #4a4f59; padding: 12px; }
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
)HTML"
R"HTML(
  /* ---- wall tabs (per-group filtering) ---- */
  #tabs { display: flex; flex-wrap: wrap; gap: 6px; padding: 12px 20px 0; }
  #tabs[hidden] { display: none; }
  #tabs .tab { background: #1e2128; border: 1px solid #2a2e36; color: #9aa3b2;
               border-radius: 14px; font-size: 12px; padding: 3px 14px; cursor: pointer; }
  #tabs .tab:hover { color: #d4d7dd; }
  #tabs .tab.active { background: #1c2a3a; color: #79b8ff; border-color: #2c405a; }

  /* ---- header gear (admin only) ---- */
  #gearBtn { background: none; border: 1px solid #2a2e36; color: #7d838e;
             border-radius: 5px; font-size: 13px; padding: 2px 9px; cursor: pointer; }
  #gearBtn:hover { color: #d4d7dd; }

  /* ---- settings overlay (admin) ---- */
  #settings { position: fixed; inset: 0; background: rgba(10,11,14,.75);
              display: flex; align-items: flex-start; justify-content: center;
              padding: 4vh 16px; overflow-y: auto; z-index: 20; }
  #settings[hidden] { display: none; }
  #sPanel { background: #1b1e24; border: 1px solid #323844; border-radius: 12px;
            width: min(820px, 100%); margin-bottom: 4vh; }
  .sTabs { display: flex; gap: 4px; margin: 0 12px; }
  .sTab { background: none; border: 1px solid #2a2e36; color: #9aa3b2;
          border-radius: 6px; font-size: 12px; padding: 3px 14px; cursor: pointer; }
  .sTab:hover { color: #d4d7dd; }
  .sTab.active { background: #1c2a3a; color: #79b8ff; border-color: #2c405a; }
  .sPane { padding: 16px 18px; }
  .sPane[hidden] { display: none; }
  /* Fixed layout + colgroup widths keep header and body columns aligned
     regardless of what an editable cell (a scope input + Set button) holds. */
  .sTable { width: 100%; border-collapse: collapse; font-size: 13px; table-layout: fixed; }
  .sTable th { text-align: left; color: #6b727e; font-weight: 500; font-size: 11px;
               text-transform: uppercase; letter-spacing: .05em; padding: 4px 8px; }
  .sTable td { padding: 4px 8px; border-top: 1px solid #23262d; vertical-align: middle;
               overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .sTable td.acts { overflow: visible; white-space: normal; }
  .sTable input, .sTable select, .sRow input, .sRow select {
      background: #191c22; border: 1px solid #2a2e36; border-radius: 5px;
      color: #d4d7dd; font-size: 12px; padding: 3px 8px; }
  .sBtn { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
          border-radius: 5px; font-size: 12px; padding: 3px 10px; cursor: pointer; }
  .sBtn:hover { background: #223449; }
  .sBtn.danger { background: #2a1c1c; color: #ff8a80; border-color: #5a2c2c; }
  .sBtn.danger:hover { background: #3a2222; }
  .sRow { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-top: 10px; }
  .sNote { color: #7d838e; font-size: 12px; margin-top: 6px; }
  .sReveal { background: #101216; border: 1px solid #2b4a35; border-radius: 6px;
             color: #4ecb71; font: 12px ui-monospace, Menlo, monospace;
             padding: 8px 10px; margin-top: 8px; word-break: break-all; }
  .sReveal[hidden] { display: none; }
  .sSub { margin: 20px 0 8px; font-size: 13px; color: #9aa3b2; font-weight: 600;
          border-top: 1px solid #21252d; padding-top: 16px; }
</style>
</head>
<body>
<header>
  <h1>AnchorBolt</h1>
  <span class="sub" id="summary"></span>
  <span style="flex:1"></span>
  <span class="sub" id="who"></span>
  <button id="gearBtn" hidden title="settings">&#9881;</button>
  <button id="logoutBtn" hidden>logout</button>
</header>
<div id="tabs" hidden></div>
<div id="grid"></div>
<div id="empty" hidden>no apps have reported yet &mdash; run
  <code>anchorbolt start &lt;app&gt; --server &lt;this url&gt;</code></div>

<div id="login" hidden>
  <div id="loginBox">
    <h2>AnchorBolt</h2>
    <input id="loginTok" type="password" placeholder="operator token or 6-digit code" autocomplete="off">
    <button id="loginBtn">Sign in</button>
    <div id="loginErr"></div>
  </div>
</div>

<div id="settings" hidden>
  <div id="sPanel">
    <div class="dhead">
      <h2>Settings</h2>
      <div class="sTabs">
        <button class="sTab active" data-pane="pApps">Apps</button>
        <button class="sTab" data-pane="pOps">Operators</button>
      </div>
      <span style="flex:1"></span>
      <button id="sClose" title="close" style="background:none;border:none;color:#7d838e;font-size:22px;cursor:pointer;line-height:1">&times;</button>
    </div>

    <div id="pApps" class="sPane">
      <table class="sTable">
        <colgroup><col style="width:34%"><col style="width:30%"><col style="width:16%"><col></colgroup>
        <thead><tr><th>app</th><th>group</th><th>token</th><th></th></tr></thead>
        <tbody id="sApps"></tbody>
      </table>
      <div class="sRow">
        <input id="sPairApp" placeholder="app-id for a new pairing code">
        <button class="sBtn" id="sPairBtn">Pairing code</button>
      </div>
      <div class="sNote">A pairing code lets a venue run
        <code>anchorbolt start --pair &lt;code&gt;</code> to fetch its token — no
        <code>tc-...</code> string is copied by hand.</div>
      <div class="sReveal" id="sPairOut" hidden></div>
    </div>

    <div id="pOps" class="sPane" hidden>
      <table class="sTable">
        <colgroup><col style="width:20%"><col style="width:13%"><col style="width:31%"><col style="width:14%"><col></colgroup>
        <thead><tr><th>name</th><th>role</th><th>scope</th><th>created</th><th></th></tr></thead>
        <tbody id="sOps"></tbody>
      </table>
      <div class="sRow">
        <input id="sOpName" placeholder="new operator name">
        <select id="sOpRole"><option value="viewer">viewer</option><option value="operator">operator</option><option value="admin">admin</option></select>
        <input id="sOpScope" placeholder="scope (e.g. osaka,app:special) — blank = all">
        <button class="sBtn" id="sOpAdd">Create</button>
      </div>
      <div class="sReveal" id="sOpToken" hidden></div>

      <h4 class="sSub">Share links &mdash; read-only, no login</h4>
      <table class="sTable">
        <colgroup><col style="width:46%"><col style="width:22%"><col style="width:16%"><col></colgroup>
        <thead><tr><th>scope</th><th>expires</th><th>created</th><th></th></tr></thead>
        <tbody id="sShares"></tbody>
      </table>
      <div class="sRow">
        <input id="sShareScope" placeholder="scope (e.g. osaka,app:special) — blank = all">
        <input id="sShareDays" type="number" min="0" placeholder="days (0 = never)" style="width:130px; flex:none">
        <button class="sBtn" id="sShareAdd">Create link</button>
      </div>
      <div class="sReveal" id="sShareOut" hidden></div>
    </div>
  </div>
</div>

<div id="detail" hidden>
  <div id="dPanel">
    <div class="dhead">
      <div class="dTitleRow">
        <span class="dot" id="dDot"></span>
        <h2 id="dTitle"></h2>
        <button id="dClose" title="close">&times;</button>
      </div>
      <div class="dBtnRow" id="dBtns" hidden>
        <button id="dLiveBtn" title="live view + remote control">Live</button>
        <button id="dUpdate" title="git pull + build + restart on the venue machine">Update</button>
        <button id="dRollback" title="restore the previous binary">Roll back</button>
        <button id="dRestart">Restart</button>
      </div>
      <div class="dStatusRow"><span class="stats" id="dStats"></span></div>
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
      <div id="dScrubWrap" hidden>
        <button id="dScrubPlay" title="play a timelapse of the day">&#9654;</button>
        <input id="dScrub" type="range" min="0" max="0" value="0">
        <span id="dScrubTime">live</span>
      </div>
      <div id="dValues"></div>
      <div id="dEvWrap" hidden>
        <div id="dEvHead">
          <span class="glabel">events</span>
          <button id="dAckBtn" title="clear the event list and the wall badge (the on-disk log is kept)">Clear</button>
        </div>
        <div id="dEvents"></div>
      </div>
      <div id="dGraphs"></div>
      <div id="dImages"></div>
      <div id="dLogWrap">
        <div id="dLogHead">
          <span class="glabel">log</span>
          <select id="dLogDate" title="view a past day"><option value="">live</option></select>
          <input id="dLogFilter" type="text" placeholder="filter...">
          <button id="dLogErr" title="show only ERROR / FATAL lines">errors</button>
          <select id="dLogDl" title="download logs as a text file">
            <option value="">⬇ download…</option>
            <option value="today">today</option>
            <option value="month">last 30 days</option>
            <option value="all">everything</option>
          </select>
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
  // One field, two credentials: exactly 6 digits posts to the login-code
  // path (mints a session), anything else is treated as an op-... token.
  const val = document.getElementById('loginTok').value.trim();
  if (!val) return;
  const r = /^\d{6}$/.test(val)
    ? await fetch('/api/login/code', { method: 'POST', body: JSON.stringify({ code: val }) })
    : await fetch('/api/login', { method: 'POST', body: JSON.stringify({ token: val }) });
  if (r.ok) location.reload();
  else document.getElementById('loginErr').textContent = 'invalid token or code';
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
  // Open mode: admin already true, gear stays visible (server still enforces
  // rank 3 on every admin endpoint regardless of what the UI shows).
  if (!me.open) {
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
  // The gear opens the settings page — admin only (or open mode).
  if (me.open || me.role === 'admin') document.getElementById('gearBtn').hidden = false;
}

(async () => {
  // Share link: ?share=<token> logs in read-only, then we strip it from the URL
  // so the token isn't left sitting in the address bar / history.
  const share = new URLSearchParams(location.search).get('share');
  if (share) {
    try { await fetch('/api/login', {method: 'POST', body: JSON.stringify({token: share})}); } catch {}
    history.replaceState(null, '', location.pathname);
  }
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

// ---- screenshot scrubber ----
let thumbLive = true;    // false while a past frame is pinned by the slider
let thumbTimes = [];     // YYYYMMDD-HHMMSS of stored frames for the shown day
let scrubDate = '';      // '' = today (slider's max = live); else a past day (all static)
let scrubPlay = null;

// date '' = today (newest = live); a YYYY-MM-DD past day is all historical.
async function loadThumbTimes(id, date = '') {
  scrubDate = date;
  thumbTimes = [];
  const q = date ? '?date=' + encodeURIComponent(date) : '';
  try { thumbTimes = (await (await fetch('/api/thumbtimes/' + encodeURIComponent(id) + q)).json()).times || []; } catch {}
  const sc = document.getElementById('dScrub');
  sc.max = Math.max(0, thumbTimes.length - 1);
  sc.value = sc.max;
  document.getElementById('dScrubWrap').hidden = thumbTimes.length < 1;
  if (date) {                              // a past day has no live end — pin its last frame
    thumbLive = false;
    if (thumbTimes.length) {
      const img = document.querySelector('#dThumbWrap img');
      img.src = '/api/thumb/' + encodeURIComponent(id) + '?ts=' + thumbTimes[thumbTimes.length - 1];
      img.hidden = false;
    }
  } else {
    thumbLive = true;
  }
  updateScrubLabel();
}

function updateScrubLabel() {
  const sc = document.getElementById('dScrub');
  const atEnd = +sc.value >= +sc.max;
  const ts = thumbTimes[+sc.value];
  document.getElementById('dScrubTime').textContent =
    (!scrubDate && (atEnd || !ts)) ? 'live'
      : (ts ? ts.slice(9, 11) + ':' + ts.slice(11, 13) : '—');
}

function stopScrubPlay() {
  if (scrubPlay) { clearInterval(scrubPlay); scrubPlay = null; }
  const b = document.getElementById('dScrubPlay');
  if (b) b.innerHTML = '&#9654;';
}

document.getElementById('dScrub').addEventListener('input', () => {
  const sc = document.getElementById('dScrub');
  const idx = +sc.value;
  const img = document.querySelector('#dThumbWrap img');
  if (!scrubDate && idx >= +sc.max) {                 // today's newest = live
    thumbLive = true;
    dThumbSeq = -1;                                    // force a live refresh next poll
  } else if (thumbTimes[idx]) {
    thumbLive = false;
    img.src = '/api/thumb/' + encodeURIComponent(detailId) + '?ts=' + thumbTimes[idx];
    img.hidden = false;
  }
  updateScrubLabel();
});

document.getElementById('dScrubPlay').addEventListener('click', () => {
  if (scrubPlay) { stopScrubPlay(); return; }
  if (thumbTimes.length < 2) return;
  document.getElementById('dScrubPlay').innerHTML = '&#10073;&#10073;';   // pause glyph
  scrubPlay = setInterval(() => {
    const sc = document.getElementById('dScrub');
    let v = +sc.value + 1;
    if (v > +sc.max) v = 0;                            // loop the day
    sc.value = v;
    sc.dispatchEvent(new Event('input'));
  }, 200);                                             // ~5 fps timelapse
});

function fmtUptime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? `${h}h${m}m` : m > 0 ? `${m}m${s % 60}s` : `${s}s`;
}

function statsLine(app) {
  if (!app.reported) return 'waiting for first report';
  const h = app.health || {};
  const parts = [];
  if (h.fps !== undefined) parts.push(h.fps.toFixed(0) + ' fps');
  if (h.width) parts.push(h.width + 'x' + h.height);
  if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
  if (app.ageSec > STALE_SEC) parts.push('last seen ' + Math.floor(app.ageSec) + 's ago');
  return parts.join(' · ');
}

// Wall cards show only uptime + freshness; fps/size live in the detail view.
function wallStats(app) {
  if (!app.reported) return 'waiting for first report';
  const h = app.health || {};
  const parts = [];
  if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
  if (app.ageSec > STALE_SEC) parts.push('last seen ' + Math.floor(app.ageSec) + 's ago');
  return parts.join(' · ');
}

function card(app) {
  const el = document.createElement('div');
  el.className = 'card';
  el.id = 'app-' + app.id;
  el.innerHTML = `
    <div class="thumbWrap"><span class="none">no thumbnail</span><img hidden>
      <div class="offlay" hidden><span class="who"></span><span class="ago"></span></div></div>
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
  logLiveMode = true;
  document.getElementById('dLogDate').value = '';
  loadLogDays(id);
  thumbLive = true;
  stopScrubPlay();
  document.getElementById('dScrubWrap').hidden = true;
  loadThumbTimes(id);
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
  stopScrubPlay();
  detailId = null;
  document.getElementById('detail').hidden = true;
}
document.getElementById('dClose').addEventListener('click', closeDetail);
document.getElementById('detail').addEventListener('click', e => {
  if (e.target === document.getElementById('detail')) closeDetail();
});
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeDetail(); });
)HTML"
// MSVC の文字列リテラル上限(16380バイト)対策でここで一旦分割。隣接リテラルは連結される
R"HTML(
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
  // The action buttons ride the command channel, so they only make sense when
  // the agent's WS is connected and the operator can act. Update/Rollback show
  // only if the venue allows update (it advertises this); Restart is always
  // there for an operator. Viewers get no action row.
  const caps = (app.health && app.health.caps) || {};
  const canOperate = myRole !== 'viewer';   // null (open mode) / operator / admin
  document.getElementById('dBtns').hidden = !app.live || !canOperate;
  document.getElementById('dUpdate').hidden = !caps.update;
  document.getElementById('dRollback').hidden = !caps.update;
  const h = app.health || {};
  const extra = [];
  if (h.git) extra.push('@' + h.git);
  if (h.version) extra.push(h.version);
  const mem = h.rssBytes ?? h.memoryBytes;
  if (mem) extra.push((mem / 1048576).toFixed(0) + ' MB');
  document.getElementById('dStats').textContent =
    [statsLine(app), ...extra].filter(Boolean).join(' · ');

  // Live thumbnail (same seq mechanism as the wall) — paused while scrubbing.
  if (thumbLive && app.thumbSeq > 0 && dThumbSeq !== app.thumbSeq) {
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
  // Control needs the operator role AND an app that exposes input tools (the
  // venue advertises caps.control). myRole===null is open mode (allowed).
  const ctlApp = lastApps.find(a => a.id === detailId);
  const canControl = myRole !== 'viewer' &&
                     !!(ctlApp && (ctlApp.health || {}).caps && ctlApp.health.caps.control);
  document.getElementById('dCtlToggle').hidden = !canControl;
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
// Hover passthrough: forward plain pointer movement (no button) as
// tc_mouse_move so hover-reactive installations respond. Throttled to ~20/s
// — the command channel is one HTTP POST per event, so an unthrottled hover
// would flood it; 50ms keeps it responsive without the deluge. Drags are
// handled by the window listener above, so skip while a press is in flight.
let lastHoverSent = 0;
liveImg.addEventListener('mousemove', e => {
  if (!ctlOn() || liveDown) return;
  const now = Date.now();
  if (now - lastHoverSent < 50) return;
  lastHoverSent = now;
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
  // Empty the visible list too — the server dropped it, so it won't stream back.
  document.getElementById('dEvents').replaceChildren();
  document.getElementById('dEvWrap').hidden = true;
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

let logLiveMode = true;   // false while viewing a past day (no live streaming)

async function pollLog(id) {
  if (!logLiveMode) return;   // a historical day is pinned; don't append live lines
  let r;
  try {
    r = await (await fetch('/api/log/' + encodeURIComponent(id) + '?after=' + logCursor)).json();
  } catch { return; }
  if (detailId !== id || !logLiveMode) return;
  logCursor = r.next;
  appendLogLines(r.lines);
}

// Populate the date picker with the days that have stored logs.
async function loadLogDays(id) {
  const sel = document.getElementById('dLogDate');
  sel.replaceChildren(Object.assign(document.createElement('option'), {value: '', textContent: 'live'}));
  let dates = [];
  try { dates = (await (await fetch('/api/logdays/' + encodeURIComponent(id))).json()).dates || []; } catch {}
  const today = new Date().toISOString().slice(0, 10);
  for (const d of dates) {
    if (d === today) continue;   // today is the live view
    sel.appendChild(Object.assign(document.createElement('option'), {value: d, textContent: d}));
  }
}

// Switch between the live ring and a static past day.
document.getElementById('dLogDate').addEventListener('change', async e => {
  const date = e.target.value;
  const box = document.getElementById('dLog');
  box.replaceChildren();
  // The date drives the screenshot scrubber too: pick a past day and both the
  // log and the day's screenshots follow it; 'live' returns to today.
  stopScrubPlay();
  loadThumbTimes(detailId, date);
  if (!date) {                    // back to live
    logLiveMode = true;
    logCursor = 0;
    return;                       // the poll cycle refills from the ring
  }
  logLiveMode = false;
  try {
    const r = await (await fetch('/api/log/' + encodeURIComponent(detailId) +
                                 '?date=' + encodeURIComponent(date))).json();
    appendLogLines(r.lines);
    if (r.truncated) appendLogLines([{at: '', src: 'sup', text: '(older lines truncated)'}]);
  } catch {}
});

// Download logs + images for a range as a zip.
document.getElementById('dLogDl').addEventListener('change', e => {
  const range = e.target.value;
  e.target.value = '';
  if (!range || !detailId) return;
  const a = document.createElement('a');
  a.href = '/api/logdownload/' + encodeURIComponent(detailId) + '?range=' + range;
  a.download = '';
  document.body.appendChild(a);
  a.click();
  a.remove();
});

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
    if (app.hidden) continue;             // hidden from the wall (still in settings)
    alive.add('app-' + app.id);
    let el = document.getElementById('app-' + app.id);
    if (!el) { el = card(app); grid.appendChild(el); }

    const offline = app.reported && app.ageSec > STALE_SEC;
    el.classList.toggle('stale', app.ageSec > STALE_SEC);
    el.dataset.group = app.group || '';
    el.querySelector('.label').textContent = app.id;
    el.querySelector('.stats').textContent = wallStats(app);
    // Offline: the last screenshot is blurred (CSS) with a text overlay on top.
    const off = el.querySelector('.offlay');
    off.hidden = !offline;
    if (offline) {
      off.querySelector('.who').textContent = app.id;
      off.querySelector('.ago').textContent = wallStats(app) || 'offline';
    }
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
  const ok = apps.filter(a => a.reported && a.ageSec <= STALE_SEC).length;
  document.getElementById('summary').textContent =
    apps.length === 0 ? '' : `${ok}/${apps.length} healthy`;

  renderTabs(apps);
  applyTabFilter();
  renderDetail();
}

)HTML"
R"HTML(// ---- wall tabs (client-side group filter, selection persisted) ----

let activeTab = localStorage.getItem('abTab') || 'all';

function groupsPresent(apps) {
  const set = new Set();
  let ungrouped = false;
  for (const a of apps) { if (a.group) set.add(a.group); else ungrouped = true; }
  return { groups: [...set].sort(), ungrouped };
}

// Tabs appear only when at least one group exists; "ungrouped" only when the
// visible set actually mixes grouped and ungrouped apps.
function renderTabs(apps) {
  const bar = document.getElementById('tabs');
  const { groups, ungrouped } = groupsPresent(apps);
  if (groups.length === 0) { bar.hidden = true; bar.replaceChildren(); return; }
  const names = ['all', ...groups];
  if (ungrouped) names.push('ungrouped');
  if (!names.includes(activeTab)) activeTab = 'all';
  bar.hidden = false;
  bar.replaceChildren(...names.map(name => {
    const el = document.createElement('div');
    el.className = 'tab' + (name === activeTab ? ' active' : '');
    el.textContent = name;
    el.addEventListener('click', () => {
      activeTab = name;
      localStorage.setItem('abTab', name);
      renderTabs(lastApps);
      applyTabFilter();
    });
    return el;
  }));
}

function applyTabFilter() {
  for (const el of document.querySelectorAll('#grid .card')) {
    const g = el.dataset.group || '';
    const show = activeTab === 'all'
      || (activeTab === 'ungrouped' ? g === '' : g === activeTab);
    el.style.display = show ? '' : 'none';
  }
}

// ---- settings page (admin only; the server enforces rank 3 regardless) ----

function byId(id) { return document.getElementById(id); }
function stCell(text) { const e = document.createElement('td'); e.textContent = text; return e; }
function stMsg(cols, msg) {
  const tr = document.createElement('tr');
  const c = document.createElement('td');
  c.colSpan = cols; c.textContent = msg; c.style.color = '#6b727e';
  tr.appendChild(c);
  return tr;
}
function stReveal(id, text) { const el = byId(id); el.textContent = text; el.hidden = false; }
async function stPost(path, body) {
  return fetch(path, { method: 'POST', body: JSON.stringify(body) });
}

byId('gearBtn').addEventListener('click', openSettings);
byId('sClose').addEventListener('click', () => byId('settings').hidden = true);
byId('settings').addEventListener('click', e => {
  if (e.target === byId('settings')) byId('settings').hidden = true;
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') byId('settings').hidden = true;
});

function showSettingsPane(pane) {
  for (const t of document.querySelectorAll('.sTab'))
    t.classList.toggle('active', t.dataset.pane === pane);
  for (const id of ['pApps', 'pOps']) byId(id).hidden = (id !== pane);
}
for (const t of document.querySelectorAll('.sTab'))
  t.addEventListener('click', () => showSettingsPane(t.dataset.pane));

async function openSettings() {
  byId('settings').hidden = false;
  byId('sOpToken').hidden = true;
  byId('sPairOut').hidden = true;
  byId('sShareOut').hidden = true;
  showSettingsPane('pApps');
  loadSettingsApps();
  loadSettingsOps();
  loadSettingsShares();
}

// One "Apps" table over every app-id this server knows: the union of apps
// that have reported and app-ids that hold an agent token (a venue can be
// provisioned before it first connects). Each row carries its group and its
// token status — "app" and "agent token" are the same identity, so they live
// together instead of in two mismatched sections.
async function loadSettingsApps() {
  const tb = byId('sApps');
  let apps = [], agents = [];
  try { apps = await (await fetch('/api/apps')).json(); } catch {}
  try { agents = await (await fetch('/api/admin/agents')).json(); } catch {}
  const byIdMap = new Map();
  for (const a of apps) byIdMap.set(a.id, { id: a.id, group: a.group || '', hash: null, hidden: !!a.hidden });
  for (const g of agents) {
    const e = byIdMap.get(g.id) || { id: g.id, group: '', hash: null };
    e.hash = g.hashPrefix || '';
    byIdMap.set(g.id, e);
  }
  const rows = [...byIdMap.values()].sort((x, y) => x.id < y.id ? -1 : 1);
  if (!rows.length) { tb.replaceChildren(stMsg(4, 'no apps yet — mint a pairing code below')); return; }
  tb.replaceChildren(...rows.map(a => {
    const tr = document.createElement('tr');
    // group cell: input + Save
    const inp = document.createElement('input');
    inp.value = a.group; inp.placeholder = 'ungrouped'; inp.style.width = '60%';
    const save = document.createElement('button');
    save.className = 'sBtn'; save.textContent = 'Save'; save.style.marginLeft = '4px';
    save.addEventListener('click', async () => {
      await stPost('/api/admin/group', { app: a.id, group: inp.value.trim() });
      save.textContent = 'Saved'; setTimeout(() => save.textContent = 'Save', 1200);
    });
    const tdG = document.createElement('td'); tdG.append(inp, save);
    // token cell
    const tdT = stCell(a.hash === null ? 'none' : a.hash + '...');
    if (a.hash === null) tdT.style.color = '#6b727e';
    // actions
    const pc = document.createElement('button');
    pc.className = 'sBtn'; pc.textContent = 'Pairing code';
    pc.addEventListener('click', () => mintPairCode(a.id));
    const tdX = document.createElement('td'); tdX.className = 'acts'; tdX.append(pc);
    // Hide/Show on the wall (restorable, not a delete).
    const hb = document.createElement('button');
    hb.className = 'sBtn'; hb.textContent = a.hidden ? 'Show' : 'Hide'; hb.style.marginLeft = '4px';
    hb.addEventListener('click', async () => {
      await stPost('/api/admin/app/hide', { app: a.id, hidden: !a.hidden });
      loadSettingsApps();
    });
    tdX.append(hb);
    if (a.hash !== null) {
      const rv = document.createElement('button');
      rv.className = 'sBtn danger'; rv.textContent = 'Revoke'; rv.style.marginLeft = '4px';
      rv.addEventListener('click', async () => {
        if (!confirm('Revoke the token for "' + a.id + '"? It stops authenticating on its next push.')) return;
        await stPost('/api/admin/agent/revoke', { id: a.id });
        loadSettingsApps();
      });
      tdX.append(rv);
    }
    if (a.hidden) tr.style.opacity = '.5';   // hidden from the wall
    tr.append(stCell(a.id), tdG, tdT, tdX);
    return tr;
  }));
}

async function loadSettingsOps() {
  const tb = byId('sOps');
  let ops = [];
  try { ops = await (await fetch('/api/admin/operators')).json(); } catch {}
  if (!ops.length) { tb.replaceChildren(stMsg(5, 'open mode — no operators registered')); return; }
  tb.replaceChildren(...ops.map(o => {
    const tr = document.createElement('tr');
    const si = document.createElement('input');
    si.value = (o.scope || []).join(','); si.placeholder = 'all'; si.style.width = '62%';
    const sset = document.createElement('button');
    sset.className = 'sBtn'; sset.textContent = 'Set'; sset.style.marginLeft = '4px';
    sset.addEventListener('click', async () => {
      await stPost('/api/admin/operator/scope', { name: o.name, scope: si.value.trim() });
      sset.textContent = 'Set!'; setTimeout(() => sset.textContent = 'Set', 1200);
    });
    const tdS = document.createElement('td'); tdS.className = 'acts'; tdS.append(si, sset);
    const lc = document.createElement('button');
    lc.className = 'sBtn'; lc.textContent = 'Login code';
    lc.addEventListener('click', async () => {
      const r = await stPost('/api/admin/login-code', { name: o.name });
      if (r.ok) { const j = await r.json();
        stReveal('sOpToken', 'login code for ' + o.name + ': ' + j.code + '  (valid 10 min)'); }
    });
    const rv = document.createElement('button');
    rv.className = 'sBtn danger'; rv.textContent = 'Revoke'; rv.style.marginLeft = '4px';
    rv.addEventListener('click', async () => {
      if (!confirm('Revoke operator "' + o.name + '"? Their sessions die immediately.')) return;
      await stPost('/api/admin/operator/revoke', { name: o.name });
      loadSettingsOps();
    });
    const tdX = document.createElement('td'); tdX.className = 'acts'; tdX.append(lc, rv);
    tr.append(stCell(o.name), stCell(o.role), tdS, stCell(o.created || ''), tdX);
    return tr;
  }));
}

byId('sOpAdd').addEventListener('click', async () => {
  const name = byId('sOpName').value.trim();
  if (!name) return;
  const r = await stPost('/api/admin/operator/new',
    { name, role: byId('sOpRole').value, scope: byId('sOpScope').value.trim() });
  if (r.ok) {
    const j = await r.json();
    stReveal('sOpToken', 'token for ' + j.name + ' (shown ONCE — copy it now):\n' + j.token);
    byId('sOpName').value = ''; byId('sOpScope').value = '';
    loadSettingsOps();
  } else {
    stReveal('sOpToken', 'error: ' + (await r.text()));
  }
});

function fmtShareExpiry(exp) {
  return exp ? new Date(exp * 1000).toISOString().slice(0, 10) : 'never';
}
async function loadSettingsShares() {
  const tb = byId('sShares');
  let shares = [];
  try { shares = await (await fetch('/api/admin/shares')).json(); } catch {}
  if (!shares.length) { tb.replaceChildren(stMsg(4, 'no share links yet')); return; }
  tb.replaceChildren(...shares.map(s => {
    const tr = document.createElement('tr');
    const rv = document.createElement('button');
    rv.className = 'sBtn danger'; rv.textContent = 'Revoke';
    rv.addEventListener('click', async () => {
      if (!confirm('Revoke this share link? It stops working immediately.')) return;
      await stPost('/api/admin/share/revoke', { id: s.id });
      loadSettingsShares();
    });
    const tdX = document.createElement('td'); tdX.className = 'acts'; tdX.append(rv);
    tr.append(stCell((s.scope || []).join(', ') || 'all'),
              stCell(fmtShareExpiry(s.expires)), stCell(s.created || ''), tdX);
    return tr;
  }));
}

byId('sShareAdd').addEventListener('click', async () => {
  const r = await stPost('/api/admin/share/new',
    { scope: byId('sShareScope').value.trim(), days: parseInt(byId('sShareDays').value, 10) || 0 });
  if (r.ok) {
    const j = await r.json();
    stReveal('sShareOut', 'share link (copy & send — grants read-only access to the scope):\n'
             + location.origin + '/?share=' + j.token);
    byId('sShareScope').value = ''; byId('sShareDays').value = '';
    loadSettingsShares();
  } else {
    stReveal('sShareOut', 'error: ' + (await r.text()));
  }
});

async function mintPairCode(app) {
  const r = await stPost('/api/admin/pair-code', { app });
  if (r.ok) {
    const j = await r.json();
    stReveal('sPairOut', 'pairing code for ' + app + ': ' + j.code + '  (valid 10 min)\n'
      + 'venue: anchorbolt start --pair ' + j.code + ' --server <this server url>');
    loadSettingsApps();   // a code provisions the app-id — show it in the table
  } else {
    stReveal('sPairOut', 'error: ' + (await r.text()));
  }
}

byId('sPairBtn').addEventListener('click', () => {
  const app = byId('sPairApp').value.trim();
  if (app) { mintPairCode(app); byId('sPairApp').value = ''; }
});

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

    // Agent authentication: every ingest request and WS hello must present a
    // valid token for its app id. Secure by default — a venue reaches the fleet
    // only with a token minted here (via `token agent new` or a pairing code);
    // there is no open mode, so knowing the URL alone gets you nothing.
    auto authOk = [dataDir](const httplib::Request& req, const string& appId) {
        string d = dataDir.string();
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

    // The operator behind this request (nullopt in open mode). Used for scope
    // filtering; role gating stays with requireRole above.
    auto currentOp = [dataDir, cookieToken](const httplib::Request& req)
        -> optional<token::Operator> {
        if (!token::operatorsEnabled(dataDir.string())) return nullopt;  // open mode
        return token::verifyOperator(dataDir.string(), cookieToken(req));
    };
    // Per-app visibility: every per-app endpoint 404s an out-of-scope app so
    // its existence never leaks to a narrowly-scoped operator. Open mode and
    // unscoped/admin operators see everything.
    auto appVisible = [dataDir, currentOp](const httplib::Request& req, const string& appId) {
        return opSeesApp(currentOp(req), dataDir.string(), appId);
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
            if (!validAppId(app) || !token::verify(d, app, tok)) {
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
        optional<token::Operator> op;  // nullopt = open mode / full visibility
        if (token::operatorsEnabled(d)) {
            string tok;
            string h = req.get_header_value("Authorization");
            if (h.rfind("Bearer ", 0) == 0) tok = h.substr(7);
            if (tok.empty()) tok = cookieToken(req);
            op = token::verifyOperator(d, tok);
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
            reply["result"] = fleetToolCall(dataDir, name, args, rank, op);
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

    // ---- admin (rank 3): groups, operators, agents, codes -------------------
    // Every admin endpoint reuses the Token.cpp registries so the CLI and the
    // dashboard settings page mint/revoke through exactly the same code path.

    // Scope arrives from the settings UI as free text (comma-separated) or as a
    // JSON array; accept either and normalize to a string vector.
    auto parseScope = [](const Json& v) -> vector<string> {
        vector<string> out;
        if (v.is_array()) {
            for (auto& s : v) if (s.is_string() && !s.get<string>().empty()) out.push_back(s.get<string>());
        } else if (v.is_string()) {
            string cur;
            for (char c : v.get<string>()) {
                if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
                else if (c != ' ') cur.push_back(c);
            }
            if (!cur.empty()) out.push_back(cur);
        }
        return out;
    };
    auto parseBody = [](const httplib::Request& req, httplib::Response& res, Json& body) {
        try { body = Json::parse(req.body); return true; }
        catch (...) { res.status = 400; res.set_content("invalid JSON", "text/plain"); return false; }
    };

    // Assign an app to a group (empty group removes it).
    svr.Post("/api/admin/group", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string app = body.value("app", "");
        if (!validAppId(app)) { res.status = 400; res.set_content("invalid app id", "text/plain"); return; }
        token::setGroup(dataDir.string(), app, body.value("group", ""));
        res.set_content("ok", "text/plain");
    });

    // Hide an app from the wall (restorable — not a delete). Kept in the app
    // registry, so it stays hidden across restarts; flushed immediately.
    svr.Post("/api/admin/app/hide", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string app = body.value("app", "");
        if (!validAppId(app)) { res.status = 400; res.set_content("invalid app id", "text/plain"); return; }
        { lock_guard<mutex> lock(g_appsMutex); g_apps[app].hidden = body.value("hidden", true); }
        flushAppRegistry(dataDir);
        res.set_content("ok", "text/plain");
    });

    svr.Get("/api/admin/operators", [dataDir, requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        res.set_content(dumpSafeS(token::listOperators(dataDir.string())), "application/json");
    });

    svr.Post("/api/admin/operator/new", [dataDir, requireRole, parseBody, parseScope](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string name = body.value("name", "");
        string role = body.value("role", "");
        if (name.empty()) { res.status = 400; res.set_content("name required", "text/plain"); return; }
        string tok = token::mintOperator(dataDir.string(), name, role, parseScope(body.value("scope", Json())));
        if (tok.empty()) { res.status = 400; res.set_content("invalid role (viewer|operator|admin)", "text/plain"); return; }
        // The plaintext token is returned ONCE — the UI shows it and nowhere else.
        res.set_content(dumpSafeS(Json({{"name", name}, {"token", tok}})), "application/json");
    });

    // Share links: a viewer-scoped token delivered as a URL (?share=...). Admin
    // mints one with a scope + optional expiry; recipients get read-only access
    // to just those apps, no login. The token is shown once (it's in the URL).
    svr.Get("/api/admin/shares", [dataDir, requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        res.set_content(dumpSafeS(token::listShares(dataDir.string())), "application/json");
    });
    svr.Post("/api/admin/share/new", [dataDir, requireRole, parseBody, parseScope](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        int days = body.value("days", 0);   // 0 = never expires
        string tok = token::mintShare(dataDir.string(), parseScope(body.value("scope", Json())),
                                      days > 0 ? days * 86400 : 0);
        if (tok.empty()) { res.status = 500; res.set_content("could not mint share", "text/plain"); return; }
        res.set_content(dumpSafeS(Json({{"token", tok}})), "application/json");
    });
    svr.Post("/api/admin/share/revoke", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        if (!token::revokeShare(dataDir.string(), body.value("id", ""))) {
            res.status = 404; res.set_content("not found", "text/plain"); return;
        }
        res.set_content("ok", "text/plain");
    });

    svr.Post("/api/admin/operator/revoke", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        bool ok = token::revokeOperator(dataDir.string(), body.value("name", ""));
        if (!ok) { res.status = 404; res.set_content("no such operator", "text/plain"); return; }
        res.set_content("ok", "text/plain");
    });

    svr.Post("/api/admin/operator/scope", [dataDir, requireRole, parseBody, parseScope](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        bool ok = token::setOperatorScope(dataDir.string(), body.value("name", ""), parseScope(body.value("scope", Json())));
        if (!ok) { res.status = 404; res.set_content("no such operator", "text/plain"); return; }
        res.set_content("ok", "text/plain");
    });

    svr.Get("/api/admin/agents", [dataDir, requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        res.set_content(dumpSafeS(token::listAgents(dataDir.string())), "application/json");
    });

    svr.Post("/api/admin/agent/revoke", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        bool ok = token::revokeAgent(dataDir.string(), body.value("id", ""));
        if (!ok) { res.status = 404; res.set_content("no such agent", "text/plain"); return; }
        res.set_content("ok", "text/plain");
    });

    // Mint a single-use pairing code for a venue (app-id). The venue redeems it
    // with `anchorbolt start --pair <code>` — no token ever gets copied by hand.
    svr.Post("/api/admin/pair-code", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string app = body.value("app", "");
        if (!validAppId(app)) { res.status = 400; res.set_content("invalid app id", "text/plain"); return; }
        string code = token::mintCode(dataDir.string(), "pair", app, 600);
        res.set_content(dumpSafeS(Json({{"code", code}, {"app", app}, {"expiresSec", 600}})), "application/json");
    });

    // Mint a single-use login code for an operator (dashboard sign-in without
    // pasting the op-... token).
    svr.Post("/api/admin/login-code", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string name = body.value("name", "");
        // Only mint for an operator that actually exists.
        bool exists = false;
        for (auto& e : token::listOperators(dataDir.string())) if (e.value("name", "") == name) exists = true;
        if (!exists) { res.status = 404; res.set_content("no such operator", "text/plain"); return; }
        string code = token::mintCode(dataDir.string(), "login", name, 600);
        res.set_content(dumpSafeS(Json({{"code", code}, {"name", name}, {"expiresSec", 600}})), "application/json");
    });

    // ---- code redemption (NO auth — the code itself is the credential) -------

    // Venue pairing: redeem the code, mint an agent token for its app, hand
    // both back. `anchorbolt start --pair` calls this before it does anything.
    svr.Post("/api/pair", [dataDir, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (codeGuardBlocked()) { res.status = 429; res.set_content("too many attempts, try later", "text/plain"); return; }
        Json body;
        if (!parseBody(req, res, body)) return;
        auto redeemed = token::redeemCode(dataDir.string(), body.value("code", ""));
        if (!redeemed || redeemed->first != "pair") {
            codeGuardFail();
            res.status = 401;
            res.set_content("invalid or expired code", "text/plain");
            return;
        }
        string app = redeemed->second;
        string tok = token::mintAgent(dataDir.string(), app);
        if (tok.empty()) { res.status = 500; res.set_content("could not mint token", "text/plain"); return; }
        logNotice("anchorbolt") << "paired venue '" << app << "' via code";
        res.set_content(dumpSafeS(Json({{"app", app}, {"token", tok}})), "application/json");
    });

    // Token -> id: a venue started with only its agent token (no client-side
    // id) resolves its server-assigned id here once, then caches it. The token
    // is the credential; a bad one just 401s.
    svr.Post("/api/whoami", [dataDir](const httplib::Request& req, httplib::Response& res) {
        string h = req.get_header_value("Authorization");
        const string prefix = "Bearer ";
        auto id = (h.rfind(prefix, 0) == 0)
                      ? token::resolveAgent(dataDir.string(), h.substr(prefix.size()))
                      : nullopt;
        if (!id) { res.status = 401; res.set_content("invalid token", "text/plain"); return; }
        res.set_content(dumpSafeS(Json({{"app", *id}})), "application/json");
    });

    // Operator login by code: redeem, mint a SESSION token (os-...), set the
    // same abtoken cookie. The session resolves back to the operator on every
    // request, so revoking the operator kills the session automatically.
    svr.Post("/api/login/code", [dataDir, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (codeGuardBlocked()) { res.status = 429; res.set_content("too many attempts, try later", "text/plain"); return; }
        Json body;
        if (!parseBody(req, res, body)) return;
        auto redeemed = token::redeemCode(dataDir.string(), body.value("code", ""));
        if (!redeemed || redeemed->first != "login") {
            codeGuardFail();
            res.status = 401;
            res.set_content("invalid or expired code", "text/plain");
            return;
        }
        string name = redeemed->second;
        string sess = token::mintSession(dataDir.string(), name);
        if (sess.empty()) { res.status = 500; res.set_content("could not mint session", "text/plain"); return; }
        auto op = token::verifyOperator(dataDir.string(), sess);  // resolve role for the reply
        res.set_header("Set-Cookie", "abtoken=" + sess +
                       "; HttpOnly; SameSite=Lax; Path=/; Max-Age=2592000");
        res.set_content(dumpSafeS(Json({{"name", name}, {"role", op ? op->role : "viewer"}})), "application/json");
    });


    // Dashboard -> agent command bridge. Blocks until the agent replies (or
    // 15s). Restart is handled by the supervisor itself; 'call' relays an MCP
    // tool call to the app (agent-side allowlist applies).
    svr.Post(R"(/api/command/([^/]+))", [requireRole, appVisible](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 2)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
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
            preloadAppLogs(dataDir, id, st);   // seed the log ring on first contact
            st.health = body;
            st.lastSeen = chrono::steady_clock::now();
            st.lastSeenUnix = (int64_t)time(nullptr);
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

    svr.Get(R"(/api/image/([^/]+)/([^/]+))", [requireRole, appVisible](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        string name = req.matches[2];
        if (!appVisible(req, id)) { res.status = 404; return; }
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
        // Seed history before appending new lines, so old lines keep lower seqs.
        preloadAppLogs(dataDir, id, st);
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
    // Which days of stored logs exist for this app (newest first) — feeds the
    // detail panel's date picker so an operator can read past days, not just the
    // in-memory ring (which is only today's recent tail).
    svr.Get(R"(/api/logdays/([^/]+))", [requireRole, appVisible, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        Json dates = Json::array();
        for (auto& f : dailyFiles(dataDir, id, "log-")) {
            string n = f.filename().string();          // log-YYYY-MM-DD.jsonl
            if (n.size() >= 20) dates.push_back(n.substr(4, 10));
        }
        res.set_content(Json({{"dates", dates}}).dump(), "application/json");
    });

    // Bulk export as a ZIP: logs.txt (concatenated daily files, oldest first) +
    // every thumbnail and statusImage JPEG in the range. range = today|month|all.
    svr.Get(R"(/api/logdownload/([^/]+))", [requireRole, appVisible, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        string range = req.has_param("range") ? req.get_param_value("range") : "all";
        int maxDays = range == "today" ? 1 : range == "month" ? 31 : 1000000;
        auto now = chrono::system_clock::now();
        // JPEGs are named YYYYMMDD-HHMMSS.jpg; keep those on/after this cutoff.
        string cutoff = range == "all" ? ""
                      : range == "today" ? fmtTimePoint(now, "%Y%m%d")
                      : fmtTimePoint(now - chrono::hours(24) * 30, "%Y%m%d");
        error_code ec;
        ZipWriter z;

        auto files = dailyFiles(dataDir, id, "log-");   // newest first
        if ((int)files.size() > maxDays) files.resize(maxDays);
        string logtxt;
        for (auto it = files.rbegin(); it != files.rend(); ++it) {  // oldest first
            string date = it->filename().string().substr(4, 10);
            ifstream in(*it);
            string ln;
            while (getline(in, ln)) {
                if (ln.empty()) continue;
                try {
                    Json j = Json::parse(ln);
                    logtxt += date + " " + j.value("at", "") + " [" + j.value("src", "app") +
                              "] " + j.value("text", "") + "\n";
                } catch (...) {}
            }
        }
        z.add("logs.txt", logtxt);

        auto addJpegs = [&](const fs::path& dir, const string& zprefix) {
            for (auto& e : fs::directory_iterator(dir, ec)) {
                string n = e.path().filename().string();
                if (n.size() < 12 || n.compare(n.size() - 4, 4, ".jpg") != 0) continue;
                if (!cutoff.empty() && n.substr(0, 8) < cutoff) continue;
                ifstream in(e.path(), ios::binary);
                string data((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
                if (!data.empty()) z.add(zprefix + n, data);
            }
        };
        addJpegs(dataDir / id / "thumbs", "thumbs/");
        for (auto& nd : fs::directory_iterator(dataDir / id / "images", ec)) {
            if (nd.is_directory(ec))
                addJpegs(nd.path(), "images/" + nd.path().filename().string() + "/");
        }

        res.set_header("Content-Disposition",
                       "attachment; filename=\"" + id + "-export-" + range + ".zip\"");
        res.set_content(z.finish(), "application/zip");
    });

    svr.Get(R"(/api/log/([^/]+))", [requireRole, appVisible, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        // ?date=YYYY-MM-DD reads a whole day straight from disk (a static view);
        // no date = the live in-memory ring via the ?after= cursor.
        if (req.has_param("date")) {
            string date = req.get_param_value("date");
            bool ok = date.size() == 10 && date[4] == '-' && date[7] == '-';
            for (char c : date) if (c != '-' && !isdigit((unsigned char)c)) ok = false;
            if (!ok) { res.status = 400; res.set_content("bad date", "text/plain"); return; }
            ifstream in(dataDir / id / ("log-" + date + ".jsonl"));
            deque<Json> tail;
            string ln;
            while (getline(in, ln)) {
                if (ln.empty()) continue;
                try {
                    Json j = Json::parse(ln);
                    tail.push_back({{"at", j.value("at", "")}, {"src", j.value("src", "app")},
                                    {"text", j.value("text", "")}});
                    if (tail.size() > 8000) tail.pop_front();  // cap: keep the day's latest
                } catch (...) {}
            }
            Json lines = Json::array();
            for (auto& l : tail) lines.push_back(std::move(l));
            res.set_content(Json({{"next", 0}, {"lines", lines},
                                  {"truncated", tail.size() >= 8000}}).dump(), "application/json");
            return;
        }
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
    svr.Get(R"(/api/alert/([^/]+))", [requireRole, appVisible](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
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

    // Operator pressed Clear: acknowledged — the wall badge goes quiet and the
    // in-memory event list is dismissed (so it stays empty on reopen too). The
    // durable JSONL on disk is untouched — that's the audit trail.
    svr.Post(R"(/api/alert/([^/]+)/clear)", [requireRole, appVisible](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 2)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it != g_apps.end()) {
            it->second.unackedAlerts = 0;
            it->second.alerts.clear();
        }
        res.set_content("ok", "text/plain");
    });

    // Recent heartbeat history for the detail-view graphs: tail of today's
    // JSONL as a JSON array (raw line concatenation — every line is already
    // a JSON object). ~1200 entries = 1h at the default 3s poll.
    svr.Get(R"(/api/history/([^/]+))", [dataDir, requireRole, appVisible](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!validAppId(id)) { res.status = 400; return; }
        if (!appVisible(req, id)) { res.status = 404; return; }
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

    svr.Get("/api/apps", [requireRole, currentOp, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        // Resolve the operator once, then hand each app its group and filter to
        // the in-scope set (open mode / unscoped operators see everything).
        auto op = currentOp(req);
        string d = dataDir.string();
        Json groups = token::loadGroups(d);
        Json list = Json::array();
        auto now = chrono::steady_clock::now();
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            string group = (groups.contains(id) && groups[id].is_string())
                               ? groups[id].get<string>() : "";
            if (op && !token::inScope(*op, id, group)) continue;
            // A provisioned venue (agent token / WS connect) can be in the map
            // before its first heartbeat; lastSeen is still zero then, so report
            // it as unreported rather than "last seen <uptime> ago".
            bool reported = st.lastSeen != chrono::steady_clock::time_point{};
            double age = reported ? chrono::duration<double>(now - st.lastSeen).count() : 0.0;
            Json images = Json::object();
            for (auto& [name, slot] : st.images) images[name] = slot.seq;
            list.push_back({{"id", id},
                            {"group", group},
                            {"reported", reported},
                            {"ageSec", age},
                            {"thumbSeq", st.thumbSeq},
                            {"images", images},
                            {"live", g_agents.live(id)},
                            {"hidden", st.hidden},
                            {"alerts", st.unackedAlerts},
                            {"health", st.health}});
        }
        res.set_content(list.dump(), "application/json");
    });

    // Timestamps (YYYYMMDD-HHMMSS) of the stored thumbnails for a day, ascending
    // — feeds the detail view's screenshot scrubber. Default date = today.
    svr.Get(R"(/api/thumbtimes/([^/]+))", [requireRole, appVisible, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        string date = req.has_param("date") ? req.get_param_value("date")
                                            : fmtTimePoint(chrono::system_clock::now(), "%Y-%m-%d");
        string ymd;                                    // YYYYMMDD
        for (char c : date) if (c != '-') ymd += c;
        error_code ec;
        vector<string> times;
        for (auto& e : fs::directory_iterator(dataDir / id / "thumbs", ec)) {
            string n = e.path().filename().string();   // YYYYMMDD-HHMMSS.jpg
            if (n.size() == 19 && n.compare(n.size() - 4, 4, ".jpg") == 0 &&
                n.compare(0, 8, ymd) == 0)
                times.push_back(n.substr(0, 15));
        }
        sort(times.begin(), times.end());
        res.set_content(Json({{"times", times}}).dump(), "application/json");
    });

    svr.Get(R"(/api/thumb/([^/]+))", [requireRole, appVisible, dataDir](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        string id = req.matches[1];
        if (!appVisible(req, id)) { res.status = 404; return; }
        // ?ts=YYYYMMDD-HHMMSS serves a stored historical frame straight from disk
        // (validated to that exact shape so it can't escape the thumbs dir).
        if (req.has_param("ts")) {
            string ts = req.get_param_value("ts");
            bool ok = ts.size() == 15 && ts[8] == '-';
            for (size_t i = 0; i < ts.size(); ++i)
                if (i != 8 && !isdigit((unsigned char)ts[i])) ok = false;
            if (!ok) { res.status = 400; res.set_content("bad ts", "text/plain"); return; }
            ifstream in(dataDir / id / "thumbs" / (ts + ".jpg"), ios::binary);
            if (!in) { res.status = 404; res.set_content("no such frame", "text/plain"); return; }
            string data((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
            res.set_content(data, "image/jpeg");
            return;
        }
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

    // Bring known apps back so the wall survives a restart: registry first
    // (last-seen + health + thumbnail), then every token-holding venue (so a
    // provisioned-but-quiet app still shows, matching the settings Apps list).
    loadAppRegistry(dataDir);
    loadKnownAgents(dataDir);

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

    // Persist the app registry every ~30s (and once at shutdown, below) so
    // offline cards + hidden flags survive a restart without per-heartbeat I/O.
    thread registrar([dataDir]() {
        while (!g_stop) {
            for (int i = 0; i < 300 && !g_stop; ++i)
                this_thread::sleep_for(chrono::milliseconds(100));
            flushAppRegistry(dataDir);
        }
    });

    logNotice("anchorbolt") << "fleet server on http://localhost:" << opt.port
                            << " (ws " << opt.wsPort << ", data " << dataDir.string()
                            << ")";
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
    registrar.join();
    flushAppRegistry(dataDir);   // final snapshot so nothing since the last flush is lost
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
