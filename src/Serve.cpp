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
#include <set>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "DataDir.h"
#include "Sink.h"
#include "Version.h"
#include <tcxCurl.h>    // sink test button: one direct delivery, outcome reported

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

namespace {

// Decode standard base64 (agents ship live frames as base64 text — see the
// note in the WS onMessage frame handler for why text, not binary). Skips
// padding and whitespace. Mirrors b64decode() in Start.cpp; core has toBase64
// but no decoder.
vector<unsigned char> b64decode(const string& s) {
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
                              // independent of any app's local --log-keep
    bool   autoApprove = false;  // execute mutating /mcp calls directly (no queue)
    int    approvalTtlSec = 900; // pending approvals expire after this
    int    offlineAfterSec = 120; // heartbeat silence before the offline event
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
    bool     notifiedOffline = false;               // offline event fired (notify sinks)
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

// ---- approval queue -------------------------------------------------------
// Mutating fleet-/mcp/ calls (restart_app, mutating app_call) do not execute
// directly: they enqueue here and run only once a human approves — on the
// dashboard or via `anchorbolt approvals` on the server machine. The /mcp
// surface deliberately has NO approve tool, so an AI cannot approve its own
// request. `serve --auto-approve` restores the old direct-execution behavior.
struct Approval {
    string  app;
    string  action;          // "restart" | "call"
    string  tool;            // for "call"
    Json    args;            // for "call"
    string  requestedBy;     // operator name ("open" when no operators exist)
    int64_t created = 0;     // unix seconds
    int64_t expires = 0;     // unix seconds; past this a pending entry expires
    string  status = "pending";  // pending | approved | denied | expired
    string  decidedBy;
    Json    result;          // MCP-shaped execution result (set on approval)
};
map<string, Approval> g_approvals;   // id ("ap-xxxxxx") -> entry
mutex g_approvalsMutex;
bool g_autoApprove = false;
int  g_approvalTtlSec = 900;

// ---- data-dir layout (v0.8.1) ---------------------------------------------
// config/ = human-set (kept on cleanup), state/ = machine-made (safe to wipe),
// apps/<id>/ = per-app content (logs/screenshots; delete per client). A dir
// without config/ is the legacy flat layout: serve migrates it at startup
// (migrateDataDir); these helpers keep addressing the legacy spots until then
// so the `approvals` CLI works against an un-migrated dir too.
bool newLayout(const fs::path& dataDir) { return fs::exists(dataDir / "config"); }

fs::path appContentDir(const fs::path& dataDir, const string& id) {
    return newLayout(dataDir) ? dataDir / "apps" / id : appContentDir(dataDir, id);
}

fs::path approvalsPath(const fs::path& dataDir) {
    return newLayout(dataDir) ? dataDir / "state" / "approvals.json"
                              : dataDir / "approvals.json";
}

fs::path decisionsDir(const fs::path& dataDir) {
    return newLayout(dataDir) ? dataDir / "state" / "approval-decisions"
                              : dataDir / "approval-decisions";
}

// ---- serve-side notify (sinks) --------------------------------------------
// Same engine as the app-side `sinks` config, configured fleet-wide in ONE
// place: <dataDir>/sinks.json (settings "Notify" tab edits it). Each sink can
// scope to groups / "app:<id>" — an app:<id>-only sink behaves like the same
// sink configured on that app. Events: everything apps push (restart/up/
// down/update/stop/alert) plus serve-only approval / offline / online.
shared_ptr<Notifier> g_serveNotifier;
mutex g_serveNotifierMutex;

fs::path sinksPath(const fs::path& dataDir) {
    return newLayout(dataDir) ? dataDir / "config" / "notify.json"
                              : dataDir / "sinks.json";
}

// (Re)build the notifier from sinks.json. Returns how many sinks are armed.
int loadServeSinks(const fs::path& dataDir) {
    Json arr = Json::array();
    {
        ifstream in(sinksPath(dataDir));
        if (in) {
            try {
                Json j = Json::parse(in);
                if (j.is_array()) arr = j;
                else if (j.is_object() && j["sinks"].is_array()) arr = j["sinks"];
            } catch (...) {
                logWarning("anchorbolt") << "sinks.json is not valid JSON; notify disabled";
            }
        }
    }
    auto cfgs = parseSinks(arr, dataDir);
    // uptime-kuma needs one monitor per app: allow it only when scoped to
    // exactly one app (then it beats while THAT app's heartbeats are fresh —
    // parity with an app-side sink). Broader scopes have no sound "healthy" semantics here.
    for (auto it = cfgs.begin(); it != cfgs.end();) {
        if (it->heartbeat &&
            !(it->scope.size() == 1 && it->scope[0].rfind("app:", 0) == 0)) {
            logWarning("anchorbolt") << "sink '" << it->name
                << "' skipped: uptime-kuma on the server needs scope [\"app:<id>\"]";
            it = cfgs.erase(it);
        } else {
            ++it;
        }
    }
    int armed = (int)cfgs.size();
    shared_ptr<Notifier> fresh =
        cfgs.empty() ? nullptr : make_shared<Notifier>(std::move(cfgs), "fleet");
    shared_ptr<Notifier> old;
    {
        lock_guard<mutex> lock(g_serveNotifierMutex);
        old = std::exchange(g_serveNotifier, fresh);
    }
    if (old) old->flushAndStop();
    return armed;
}

// Fire a fleet-level event into the sinks (no-op when none configured).
void serveNotify(const fs::path& dataDir, const string& app,
                 const string& event, const string& msg) {
    shared_ptr<Notifier> n;
    {
        lock_guard<mutex> lock(g_serveNotifierMutex);
        n = g_serveNotifier;
    }
    if (n) n->notify(event, msg, app, token::groupOf(dataDir.string(), app));
}

// Persist the queue (caller holds g_approvalsMutex). Decided entries are kept
// so `get_approval` / the CLI can report the outcome; pruneData ages the file's
// entries out with everything else by created date.
void flushApprovals(const fs::path& dataDir) {
    Json all = Json::object();
    for (auto& [id, a] : g_approvals) {
        Json e = {{"app", a.app},       {"action", a.action},
                  {"requestedBy", a.requestedBy},
                  {"created", a.created}, {"expires", a.expires},
                  {"status", a.status}};
        if (!a.tool.empty()) e["tool"] = a.tool;
        if (!a.args.is_null() && !a.args.empty()) e["args"] = a.args;
        if (!a.decidedBy.empty()) e["decidedBy"] = a.decidedBy;
        if (!a.result.is_null()) e["result"] = a.result;
        all[id] = e;
    }
    ofstream out(approvalsPath(dataDir));
    out << all.dump(2) << "\n";
}

void loadApprovals(const fs::path& dataDir) {
    ifstream in(approvalsPath(dataDir));
    if (!in) return;
    Json all;
    try { all = Json::parse(in); } catch (...) { return; }
    if (!all.is_object()) return;
    lock_guard<mutex> lock(g_approvalsMutex);
    for (auto& [id, e] : all.items()) {
        if (!e.is_object()) continue;
        Approval a;
        a.app         = e.value("app", "");
        a.action      = e.value("action", "");
        a.tool        = e.value("tool", "");
        a.args        = e.value("args", Json::object());
        a.requestedBy = e.value("requestedBy", "");
        a.created     = e.value("created", (int64_t)0);
        a.expires     = e.value("expires", (int64_t)0);
        a.status      = e.value("status", "pending");
        a.decidedBy   = e.value("decidedBy", "");
        if (e.contains("result")) a.result = e["result"];
        g_approvals[id] = std::move(a);
    }
}

// After a serve restart the in-memory log ring is empty, but the agent keeps its
// own cursor and won't re-ship history — so the detail panel would show nothing
// until fresh lines arrive. On an app's first contact this session, seed the ring
// from the tail of its most recent on-disk log-YYYY-MM-DD.jsonl. Done per-app on
// contact (not for every dir at boot) so long-gone apps aren't resurrected.
// Caller holds g_appsMutex.
void preloadAppLogs(const fs::path& dataDir, const string& id, AppState& st) {
    if (st.preloaded) return;
    st.preloaded = true;
    error_code ec;
    fs::path latest;
    string latestName;
    for (auto& e : fs::directory_iterator(appContentDir(dataDir, id), ec)) {
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

// The health cache: id -> {lastSeen (unix), health}. Persisting it lets the
// wall survive a serve restart — apps come back as offline cards (with their
// last screenshot + last-seen) so history stays browsable. Pure machine state
// (state/apps-health.json); the admin-set hidden flag lives in the config
// roster instead. Written on a timer / at shutdown, never per heartbeat.
fs::path appsRegistryPath(const fs::path& dataDir) {
    return newLayout(dataDir) ? dataDir / "state" / "apps-health.json"
                              : dataDir / "apps.json";
}

void flushAppRegistry(const fs::path& dataDir) {
    Json reg = Json::object();
    {
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            if (st.lastSeenUnix == 0) continue;  // never reported
            reg[id] = {{"lastSeen", st.lastSeenUnix}, {"health", st.health}};
        }
    }
    error_code ec;
    fs::create_directories(appsRegistryPath(dataDir).parent_path(), ec);
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
    // An app whose token was revoked can never report again — don't resurrect
    // it as an un-fulfillable card. Keep it only if it still holds a token, or
    // it was explicitly hidden (a kept-but-off-wall app; hidden is config now).
    set<string> agentIds, hiddenIds;
    for (auto& a : token::listAgents(dataDir.string()))
        agentIds.insert(a.value("id", ""));
    for (auto& h : token::hiddenApps(dataDir.string()))
        hiddenIds.insert(h.get<string>());
    lock_guard<mutex> lock(g_appsMutex);
    for (auto& [id, e] : reg.items()) {
        if (!e.is_object()) continue;
        bool hidden = hiddenIds.count(id) > 0;
        if (!hidden && !agentIds.count(id)) continue;   // revoked & not hidden -> drop
        auto& st = g_apps[id];
        st.lastSeenUnix = e.value("lastSeen", (int64_t)0);
        st.hidden = hidden;
        st.health = e.value("health", Json::object());
        int64_t age = nowUnix - st.lastSeenUnix;
        if (st.lastSeenUnix > 0 && age >= 0)
            st.lastSeen = nowSteady - chrono::seconds(age);   // real offline duration
        // Newest stored thumbnail (YYYYMMDD-HHMMSS.jpg) for the offline card.
        fs::path latest; string latestName;
        for (auto& f : fs::directory_iterator(appContentDir(dataDir, id) / "thumbs", ec)) {
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

// Every provisioned app (an agent token in the roster) is a known app, even
// if it hasn't reported to this build yet — so the wall matches the settings
// Apps list. Seed a g_apps entry for each; ones the registry already filled keep
// their state. For the rest, recover last-seen + the last screenshot from the
// app's own stored thumbnails (a known app, not a directory scan) so it shows
// as an offline card; an app with no history stays "waiting for first report".
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
        for (auto& f : fs::directory_iterator(appContentDir(dataDir, id) / "thumbs", ec)) {
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
    // Hidden is admin-set config: stamp it onto (or create) the entries.
    for (auto& h : token::hiddenApps(dataDir.string()))
        g_apps[h.get<string>()].hidden = true;
}

// One-shot layout migration to config/ + state/ + apps/ (v0.8.1). Idempotent:
// keyed on the config/ dir existing. Runs ONLY here in serve — the CLI reads
// whichever layout is present but never migrates, so it can't race an old
// serve still writing the flat files.
void migrateDataDir(const fs::path& dataDir) {
    if (newLayout(dataDir)) return;
    error_code ec;
    if (!fs::exists(dataDir)) return;   // fresh dir: created lazily in new layout
    fs::create_directories(dataDir / "state", ec);
    fs::create_directories(dataDir / "apps", ec);

    // Merge tokens.json + groups.json + apps.json(hidden) -> config/apps.json,
    // and strip hidden out of the health cache on its way to state/.
    Json apps = Json::object();
    auto load = [&](const char* name) -> Json {
        ifstream in(dataDir / name);
        if (!in) return Json::object();
        try { Json j = Json::parse(in); return j.is_object() ? j : Json::object(); }
        catch (...) { return Json::object(); }
    };
    Json tokens = load("tokens.json");
    for (auto& [id, h] : tokens.items()) if (h.is_string()) apps[id]["token"] = h;
    Json groups = load("groups.json");
    for (auto& [id, g] : groups.items())
        if (g.is_string() && !g.get<string>().empty()) apps[id]["group"] = g;
    Json reg = load("apps.json");
    Json health = Json::object();
    for (auto& [id, e] : reg.items()) {
        if (!e.is_object()) continue;
        if (e.value("hidden", false)) apps[id]["hidden"] = true;
        if (e.value("lastSeen", (int64_t)0) != 0)
            health[id] = {{"lastSeen", e["lastSeen"]}, {"health", e.value("health", Json::object())}};
    }
    // Everything below config/ marks the layout as migrated — write it LAST-ish
    // but before moves that depend on the gate; order here: create config now.
    fs::create_directories(dataDir / "config", ec);
    { ofstream out(dataDir / "config" / "apps.json"); out << apps.dump(2) << "\n"; }
    { ofstream out(dataDir / "state" / "apps-health.json"); out << health.dump() << "\n"; }
    fs::remove(dataDir / "tokens.json", ec);
    fs::remove(dataDir / "groups.json", ec);
    fs::remove(dataDir / "apps.json", ec);

    auto mv = [&](const char* from, const fs::path& to) {
        if (fs::exists(dataDir / from)) fs::rename(dataDir / from, to, ec);
    };
    mv("operators.json", dataDir / "config" / "operators.json");
    mv("shares.json",    dataDir / "config" / "shares.json");
    mv("sinks.json",     dataDir / "config" / "notify.json");
    mv("sessions.json",  dataDir / "state" / "sessions.json");
    mv("codes.json",     dataDir / "state" / "codes.json");
    mv("approvals.json", dataDir / "state" / "approvals.json");
    mv("approval-decisions", dataDir / "state" / "approval-decisions");

    // Every remaining root directory is per-app content -> apps/<id>/.
    int moved = 0;
    for (auto& e : fs::directory_iterator(dataDir, ec)) {
        if (!e.is_directory()) continue;
        string n = e.path().filename().string();
        if (n == "config" || n == "state" || n == "apps") continue;
        fs::rename(e.path(), dataDir / "apps" / n, ec);
        if (!ec) ++moved;
    }
    logNotice("anchorbolt") << "data dir migrated to config/ + state/ + apps/ ("
                            << moved << " app dir(s) moved)";
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
// Independent policy from any app's local --log-keep (deletions never
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
    fs::path contentRoot = newLayout(dataDir) ? dataDir / "apps" : dataDir;
    for (auto& appDir : fs::directory_iterator(contentRoot, ec)) {
        error_code e2;
        if (!appDir.is_directory(e2)) continue;  // legacy layout: skips *.json
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
        else if (a == "--auto-approve")      { opt.autoApprove = true; }
        else if (a == "--approval-ttl")      { auto v = next("--approval-ttl"); if (!v) return false; opt.approvalTtlSec = stoi(*v); }
        else if (a == "--offline-after")     { auto v = next("--offline-after"); if (!v) return false; opt.offlineAfterSec = stoi(*v); }
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

// Format an agent-relay reply as an MCP result. The agent folds app image
// blocks into {data, mimeType, ...}; unfold back into a real image block so
// the AI sees the picture. (Shared by app_call and approved-call execution.)
Json mcpFromRelay(const Json& r, const string& failMsg) {
    if (!r.value("ok", false)) return mcpError(r.value("error", failMsg));
    Json result = r["result"];
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

// ---- approval queue mechanics ---------------------------------------------

string newApprovalId() {
    static mt19937_64 rng(random_device{}());
    static const char* hex = "0123456789abcdef";
    uniform_int_distribution<int> d(0, 15);
    string id = "ap-";
    for (int i = 0; i < 6; i++) id += hex[d(rng)];
    return id;
}

// Run an approved entry's action over the agent relay. MCP-shaped result.
Json execApproval(const Approval& a) {
    if (a.action == "restart") {
        Json r = g_agents.command(a.app, Json{{"action", "restart"}});
        return r.value("ok", false) ? mcpJson(r["result"])
                                    : mcpError(r.value("error", "restart failed"));
    }
    Json r = g_agents.command(a.app, Json{{"action", "call"},
                                          {"tool", a.tool},
                                          {"args", a.args}});
    return mcpFromRelay(r, "tool call failed");
}

// Apply a human decision (dashboard or CLI). On approve, executes the action
// and stores its result on the entry; the execution runs outside the lock —
// the agent relay can take seconds — with status already "approved", so
// concurrent deciders are rejected by the status check.
bool decideApproval(const fs::path& dataDir, const string& id, bool approve,
                    const string& by, string* err) {
    Approval copy;
    {
        lock_guard<mutex> lock(g_approvalsMutex);
        auto it = g_approvals.find(id);
        if (it == g_approvals.end()) { *err = "unknown approval id"; return false; }
        auto& a = it->second;
        if (a.status != "pending") { *err = "already " + a.status; return false; }
        if (time(nullptr) > a.expires) {
            a.status = "expired";
            flushApprovals(dataDir);
            *err = "expired";
            return false;
        }
        a.status = approve ? "approved" : "denied";
        a.decidedBy = by;
        if (!approve) { flushApprovals(dataDir); return true; }
        copy = a;
    }
    logNotice("anchorbolt") << "approval " << id << " approved by " << by
                            << " -> executing " << copy.action << " on " << copy.app;
    Json result = execApproval(copy);
    {
        lock_guard<mutex> lock(g_approvalsMutex);
        auto it = g_approvals.find(id);
        if (it != g_approvals.end()) it->second.result = result;
        flushApprovals(dataDir);
    }
    return true;
}

// Public view of one entry (no args echo — they can be large).
Json approvalJson(const string& id, const Approval& a) {
    int64_t now = time(nullptr);
    Json e = {{"id", id}, {"app", a.app}, {"action", a.action},
              {"requestedBy", a.requestedBy}, {"status", a.status},
              {"ageSec", now - a.created},
              {"expiresInSec", a.status == "pending" ? a.expires - now : 0}};
    if (!a.tool.empty()) e["tool"] = a.tool;
    if (!a.decidedBy.empty()) e["decidedBy"] = a.decidedBy;
    return e;
}

// Queue a mutating call and give a human a short window to decide before the
// MCP call returns: many approvals happen within seconds of the request when
// someone is at the dashboard. Undecided after ~20s -> a pending ticket the
// caller polls with get_approval.
Json enqueueApproval(const fs::path& dataDir, Approval a) {
    a.created = time(nullptr);
    a.expires = a.created + g_approvalTtlSec;
    string id;
    {
        lock_guard<mutex> lock(g_approvalsMutex);
        do { id = newApprovalId(); } while (g_approvals.count(id));
        g_approvals[id] = a;
        flushApprovals(dataDir);
    }
    logNotice("anchorbolt") << "approval " << id << " queued: " << a.action
                            << " on " << a.app << " (by " << a.requestedBy << ")";
    serveNotify(dataDir, a.app, "approval",
                (a.action == "call" ? "call " + a.tool : a.action) +
                " requested by " + a.requestedBy + " (" + id +
                ") — approve on the dashboard or `anchorbolt approvals`");
    for (int i = 0; i < 40 && !g_stop; i++) {
        this_thread::sleep_for(chrono::milliseconds(500));
        lock_guard<mutex> lock(g_approvalsMutex);
        auto it = g_approvals.find(id);
        if (it == g_approvals.end()) break;
        const Approval& e = it->second;
        if (e.status == "denied")
            return mcpError("denied by " + e.decidedBy);
        if (e.status == "expired") break;
        // approved: the result lands right after execution finishes.
        if (e.status == "approved" && !e.result.is_null()) return e.result;
    }
    lock_guard<mutex> lock(g_approvalsMutex);
    auto it = g_approvals.find(id);
    Json ticket = (it != g_approvals.end()) ? approvalJson(id, it->second)
                                            : Json{{"id", id}, {"status", "pending"}};
    ticket["note"] = "awaiting human approval (dashboard or `anchorbolt approvals`); "
                     "poll get_approval with this id";
    return mcpJson(ticket);
}

// Newest-first list of an app's daily JSONL files with the given prefix.
vector<fs::path> dailyFiles(const fs::path& dataDir, const string& app, const string& prefix) {
    vector<fs::path> out;
    error_code ec;
    for (auto& e : fs::directory_iterator(appContentDir(dataDir, app), ec)) {
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
        "Restart the app via its supervisor (operator role). Mutating: queues for human approval — "
        "the call waits ~20s for a decision, then returns a pending ticket to poll with get_approval.",
        Json{{"app", appArg["app"]}}, Json::array({"app"})));
    tools.push_back(tool("get_approval",
        "Status of a queued mutating call by ticket id. Returns the execution result once approved, "
        "an error if denied/expired, or the pending ticket while a human is deciding.",
        Json{{"id", {{"type", "string"}, {"description", "Ticket id (ap-xxxxxx) from the pending response"}}}},
        Json::array({"id"})));
    tools.push_back(tool("app_list_tools",
        "List the app's OWN MCP tools via the live agent passthrough (tc_* standard tools plus any custom ones the app registered).",
        Json{{"app", appArg["app"]}}, Json::array({"app"})));
    tools.push_back(tool("app_call",
        "Call one of the app's own MCP tools through the agent passthrough. Read-only tc_get_* tools relay instantly; anything else needs the operator role AND queues for human approval (waits ~20s, then returns a ticket for get_approval). Image results come back as images.",
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

    // Poll a queued mutating call by ticket id (no app arg — the id names it).
    if (name == "get_approval") {
        string id = args.value("id", "");
        lock_guard<mutex> lock(g_approvalsMutex);
        auto it = g_approvals.find(id);
        if (it == g_approvals.end()) return mcpError("unknown approval id '" + id + "'");
        const Approval& a = it->second;
        if (a.status == "approved" && !a.result.is_null()) return a.result;
        if (a.status == "denied") return mcpError("denied by " + a.decidedBy);
        if (a.status == "expired") return mcpError("approval expired undecided");
        return mcpJson(approvalJson(id, a));
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
        for (auto& e : fs::directory_iterator(appContentDir(dataDir, app) / "thumbs", ec)) {
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
        for (auto& e : fs::directory_iterator(appContentDir(dataDir, app) / "thumbs", ec)) {
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
        ifstream in(appContentDir(dataDir, app) / "thumbs" / pick, ios::binary);
        vector<unsigned char> jpg((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
        if (jpg.empty()) return mcpError("could not read " + pick);
        return mcpImage(jpg, Json{{"app", app}, {"at", pick.substr(0, 15)}});
    }

    if (name == "restart_app") {
        if (rank < 2) return mcpError("restart_app needs the operator role");
        if (!g_autoApprove)
            return enqueueApproval(dataDir, Approval{app, "restart", "", Json::object(),
                                                     op ? op->name : "open"});
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
        // App-side, a mutating tool exists only if the app registered it.
        if (toolName.rfind("tc_get_", 0) != 0 && rank < 2) {
            return mcpError("'" + toolName + "' is not read-only; it needs the operator role");
        }
        Json callArgs = args.value("args", Json::object());
        if (toolName.rfind("tc_get_", 0) != 0 && !g_autoApprove)
            return enqueueApproval(dataDir, Approval{app, "call", toolName, callArgs,
                                                     op ? op->name : "open"});
        Json r = g_agents.command(app, Json{{"action", "call"},
                                            {"tool", toolName},
                                            {"args", callArgs}});
        return mcpFromRelay(r, "tool call failed");
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
  /* Group tag: plain muted text ("[test] Name") reading as part of the name —
     same font and size as the name (inherited), only the color dims. A boxed
     capsule read as a clickable button, which it is not. */
  .gbadge { color: #7d838e; flex: none; margin-right: 6px; }
  #dGroup { font-size: 16px; font-weight: bold; }  /* match the h2 beside it */
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
  /* Header capsules share one box so their heights line up exactly. */
  #addDevice, #shareBadge, #apprBtn, #gearBtn, #logoutBtn { background: none; border: 1px solid #2a2e36;
      color: #7d838e; border-radius: 5px; font-size: 11px; height: 24px;
      padding: 0 10px; box-sizing: border-box; display: inline-flex;
      align-items: center; gap: 5px; cursor: pointer; }
  #addDevice:hover, #shareBadge:hover, #apprBtn:hover, #gearBtn:hover, #logoutBtn:hover { color: #d4d7dd; }
  #addDevice[hidden], #shareBadge[hidden], #apprBtn[hidden], #gearBtn[hidden], #logoutBtn[hidden] { display: none; }
  #shareBadge { color: #8fd0ff; border-color: #2c495f; }
  #apprBtn { color: #e3b341; border-color: #55482a; }
  #apprPanel { position: fixed; top: 52px; right: 16px; width: 440px;
      max-height: 60vh; overflow-y: auto; background: #16181d;
      border: 1px solid #2a2e36; border-radius: 10px; padding: 4px 0;
      box-shadow: 0 10px 40px rgba(0,0,0,.5); z-index: 60; }
  #apprPanel[hidden] { display: none; }
  .apRow { display: flex; align-items: center; gap: 8px; padding: 8px 14px;
           font-size: 12px; }
  .apRow + .apRow { border-top: 1px solid #21252d; }
  .apRow .what { flex: 1; min-width: 0; overflow: hidden;
                 text-overflow: ellipsis; white-space: nowrap; }
  .apRow .who, .apRow .st { color: #7d838e; flex: none; }
  .apRow button { border-radius: 5px; font-size: 11px; padding: 3px 10px;
                  cursor: pointer; flex: none; }
  .apRow .ok { background: #1a2f22; color: #4ecb71; border: 1px solid #2b4a35; }
  .apRow .no { background: #33251a; color: #e0a06a; border: 1px solid #55402c; }
  .apRow.done { opacity: .55; }
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
  #dScrubTime { font: 11px ui-monospace, Menlo, monospace; color: #9aa3b2;
                flex: none; min-width: 108px; text-align: right;
                background: #191c22; border: 1px solid #2a2e36; border-radius: 5px;
                padding: 2px 7px; }
  #dScrubTime.live { color: #3fb950; border-color: #2b4a35; }
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
  /* View actions (safe): Screenshot, Live — blue (see #dLiveBtn below). */
  #dShot { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
           border-radius: 6px; font-size: 12px; padding: 4px 12px;
           cursor: pointer; flex: none; }
  #dShot:hover { background: #223449; }
  #dShot:disabled { opacity: .45; cursor: default; }
  #dShot[hidden] { display: none; }
  /* Mutating actions (all risky): Update, Roll back, Restart — orange. */
  #dUpdate, #dRollback, #dRestart {
              background: #33251a; color: #e0a06a; border: 1px solid #55402c;
              border-radius: 6px; font-size: 12px; padding: 4px 12px;
              cursor: pointer; flex: none; }
  #dUpdate:hover, #dRollback:hover, #dRestart:hover { background: #40301f; }
  #dUpdate:disabled, #dRollback:disabled, #dRestart:disabled { opacity: .45; cursor: default; }
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
  /* touch-action:none on the img blocks the browser's gesture handling
     (scroll/pinch) so a touchscreen drag reaches the remote-control JS
     instead of panning the page. Only in ctl mode so passive viewing on
     mobile keeps its native scroll. */
  #dLiveStage.ctl #dLiveImg { cursor: crosshair; touch-action: none; }
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

  /* ---- header gear (admin only): shared capsule box above, bigger glyph ---- */
  #gearBtn { font-size: 14px; }

  /* ---- settings overlay (admin) ---- */
  #settings { position: fixed; inset: 0; background: rgba(10,11,14,.75);
              display: flex; align-items: flex-start; justify-content: center;
              padding: 4vh 16px; overflow-y: auto; z-index: 20; }
  #settings[hidden] { display: none; }
  #sPanel { background: #1b1e24; border: 1px solid #323844; border-radius: 12px;
            width: min(960px, 100%); margin-bottom: 4vh; }
  .sHead { display: flex; align-items: center; gap: 16px; padding: 14px 18px;
           border-bottom: 1px solid #2a2e36; }
  .sHead h2 { margin: 0; font-size: 18px; }
  .sHead #sClose { margin-left: auto; }
  .sTabs { display: flex; gap: 4px; }
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
  .sTable td.acts { overflow: visible; white-space: nowrap; text-align: right; }
  .sTable input, .sTable select, .sRow input, .sRow select {
      background: #191c22; border: 1px solid #2a2e36; border-radius: 5px;
      color: #d4d7dd; font-size: 12px; padding: 3px 8px; }
  /* Round delete: a filled bright-orange disc with a geometric cross drawn by
     the two pseudo-element bars (rotated ±45°) — pixel-centered and truly
     orthogonal, unlike a font's ×. */
  .sXBtn { width: 17px; height: 17px; border-radius: 50%; background: #c9524a;
           border: none; cursor: pointer; padding: 0; flex: none;
           position: relative; vertical-align: middle; }
  .sXBtn::before, .sXBtn::after { content: ''; position: absolute;
           top: 50%; left: 50%; width: 9px; height: 2px; border-radius: 1px;
           background: #1a1c22; }
  .sXBtn::before { transform: translate(-50%, -50%) rotate(45deg); }
  .sXBtn::after  { transform: translate(-50%, -50%) rotate(-45deg); }
  .sXBtn:hover { background: #de6157; }
  .sTable td.acts button + button { margin-left: 6px; }
  /* Footer row = the "create new" entry, aligned to the same columns. */
  .sTable tfoot td { padding: 10px 8px 4px; border-top: 1px solid #2a2e36; }
  .sBtn { background: #1c2a3a; color: #79b8ff; border: 1px solid #2c405a;
          border-radius: 5px; font-size: 12px; padding: 3px 10px; cursor: pointer; }
  .sBtn:hover { background: #223449; }
  .sBtn.danger { background: #2a1c1c; color: #ff8a80; border-color: #5a2c2c; }
  .sBtn.danger:hover { background: #3a2222; }
  .sRow { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-top: 10px; }
  .sNote { color: #7d838e; font-size: 12px; margin-top: 6px; }
  .sHelpLink { color: #79b8ff; font-size: 12px; text-decoration: none; }
  .sHelpLink:hover { text-decoration: underline; }
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
  <button id="addDevice" hidden title="get a 6-digit code to sign in as yourself on another device (valid 10 min, single use)">Add device</button>
  <button id="shareBadge" hidden title="you opened a share link — your own login is untouched; click to return to it">shared view &times;</button>
  <button id="apprBtn" hidden title="queued AI calls awaiting approval">approvals <b id="apprN"></b></button>
  <button id="gearBtn" hidden title="settings">&#9881;</button>
  <button id="logoutBtn" hidden>logout</button>
</header>
<div id="apprPanel" hidden></div>
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
    <div class="sHead">
      <h2>Settings</h2>
      <div class="sTabs">
        <button class="sTab active" data-pane="pApps">Apps</button>
        <button class="sTab" data-pane="pOps">Operators</button>
        <button class="sTab" data-pane="pNotify">Notify</button>
      </div>
      <span style="flex:1"></span>
      <button id="sClose" title="close" style="background:none;border:none;color:#7d838e;font-size:22px;cursor:pointer;line-height:1">&times;</button>
    </div>

    <div id="pApps" class="sPane">
      <table class="sTable">
        <colgroup><col style="width:40%"><col style="width:18%"><col style="width:12%"><col></colgroup>
        <thead><tr><th>app</th><th>group</th><th>token</th><th></th></tr></thead>
        <tbody id="sApps"></tbody>
        <tfoot><tr>
          <td><input id="sPairApp" placeholder="app-id for a new pairing code" style="width:70%"><button
            class="sBtn" id="sPairBtn" style="margin-left:6px">Create</button></td>
          <td></td><td></td><td></td>
        </tr></tfoot>
      </table>
      <div class="sNote">A pairing code lets an app's machine run
        <code>anchorbolt start --pair &lt;code&gt;</code> to fetch its token — no
        <code>tc-...</code> string is copied by hand.</div>
      <div class="sReveal" id="sPairOut" hidden></div>
    </div>

    <div id="pOps" class="sPane" hidden>
      <table class="sTable">
        <colgroup><col style="width:20%"><col style="width:13%"><col style="width:31%"><col style="width:14%"><col></colgroup>
        <thead><tr><th>name</th><th>role</th><th>scope</th><th>created</th><th></th></tr></thead>
        <tbody id="sOps"></tbody>
        <tfoot><tr>
          <td><input id="sOpName" placeholder="new operator name" style="width:92%"></td>
          <td><select id="sOpRole" style="width:96%"><option value="viewer">viewer</option><option value="operator">operator</option><option value="admin">admin</option></select></td>
          <td><input id="sOpScope" placeholder="scope — blank = all" style="width:62%"><button
            class="sBtn" id="sOpAdd" style="margin-left:6px">Create</button></td>
          <td></td><td></td>
        </tr></tfoot>
      </table>
      <div class="sReveal" id="sOpToken" hidden></div>

      <h4 class="sSub">Share links &mdash; read-only, no login</h4>
      <table class="sTable">
        <colgroup><col style="width:46%"><col style="width:22%"><col style="width:16%"><col></colgroup>
        <thead><tr><th>scope</th><th>expires</th><th>created</th><th></th></tr></thead>
        <tbody id="sShares"></tbody>
        <tfoot><tr>
          <td><input id="sShareScope" placeholder="scope — blank = all" style="width:92%"></td>
          <td><input id="sShareDays" type="number" min="0" placeholder="days (0 = never)" style="width:55%"><button
            class="sBtn" id="sShareAdd" style="margin-left:6px">Create</button></td>
          <td></td><td></td>
        </tr></tfoot>
      </table>
      <div class="sReveal" id="sShareOut" hidden></div>
    </div>

    <div id="pNotify" class="sPane" hidden>
      <table class="sTable">
        <colgroup><col style="width:13%"><col style="width:41%"><col style="width:15%"><col style="width:15%"><col></colgroup>
        <thead><tr><th>preset</th><th>webhook url</th><th>events</th><th>scope</th><th></th></tr></thead>
        <tbody id="sSinks"></tbody>
      </table>
      <div class="sRow">
        <button class="sBtn" id="sSinkAdd">Add</button>
        <button class="sBtn" id="sSinkSave" hidden>Save all</button>
        <span style="flex:1"></span>
        <a class="sHelpLink" href="/help/notify" target="_blank" rel="noopener">How to get a webhook URL &nearr;</a>
      </div>
      <div class="sNote">Fleet-wide notifications, one place for every app.
        <b>events</b>: comma list of restart, up, down, update, stop, alert,
        approval, offline, online — blank = all. <b>scope</b>: groups /
        <code>app:&lt;id&gt;</code> like operator scope — blank = every app.
        An app's own <code>sinks</code> config keeps working independently.</div>
      <div class="sReveal" id="sSinkOut" hidden></div>
    </div>
  </div>
</div>

<div id="detail" hidden>
  <div id="dPanel">
    <div class="dhead">
      <div class="dTitleRow">
        <span class="dot" id="dDot"></span>
        <span id="dGroup" class="gbadge" hidden></span>
        <h2 id="dTitle"></h2>
        <button id="dClose" title="close">&times;</button>
      </div>
      <div class="dBtnRow" id="dBtns" hidden>
        <button id="dShot" title="grab a screenshot now and download it">Screenshot</button>
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
document.getElementById('addDevice').addEventListener('click', async () => {
  const btn = document.getElementById('addDevice');
  try {
    const j = await (await fetch('/api/my/login-code', { method: 'POST' })).json();
    btn.textContent = 'code ' + j.code + ' · 10 min';
    setTimeout(() => { btn.textContent = 'Add device'; }, 60000);
  } catch {
    btn.textContent = 'failed';
    setTimeout(() => { btn.textContent = 'Add device'; }, 3000);
  }
});

document.getElementById('shareBadge').addEventListener('click', async () => {
  await fetch('/api/share/exit', { method: 'POST' });
  location.reload();   // back to the login that was underneath
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
  // Share overlay with a real login underneath: offer the way back.
  if (me.share && me.hasLogin) document.getElementById('shareBadge').hidden = false;
  // Self-service device add: any real login, but not through a share overlay.
  if (!me.open && !me.share) document.getElementById('addDevice').hidden = false;
}

)HTML"
R"HTML(// ---- approvals: queued AI mutating calls (fleet /mcp) --------------------
// The badge appears when anything is pending; the panel lists pending entries
// with Approve/Deny (operator+; the server enforces role + scope regardless)
// and recently decided ones for context.
let apprOpen = false;
async function pollApprovals() {
  if (myRole === 'viewer') return;
  let list = [];
  try {
    const r = await fetch('/api/approvals');
    if (!r.ok) return;
    list = await r.json();
  } catch { return; }
  const pend = list.filter(a => a.status === 'pending');
  document.getElementById('apprN').textContent = pend.length;
  // Keep the capsule while the panel is open even if the queue just drained.
  document.getElementById('apprBtn').hidden = pend.length === 0 && !apprOpen;
  if (apprOpen) renderApprovals(list);
}

function renderApprovals(list) {
  const box = document.getElementById('apprPanel');
  box.replaceChildren();
  if (!list.length) {
    box.appendChild(Object.assign(document.createElement('div'),
                                  {className: 'apRow', textContent: 'no approvals'}));
    return;
  }
  list.sort((a, b) => (a.status === 'pending' ? 0 : 1) - (b.status === 'pending' ? 0 : 1)
                      || a.ageSec - b.ageSec);
  for (const a of list) {
    const row = document.createElement('div');
    row.className = 'apRow' + (a.status === 'pending' ? '' : ' done');
    const act = a.action === 'call' ? 'call ' + (a.tool || '?') : a.action;
    row.innerHTML = '<span class="what"></span><span class="who"></span>';
    row.querySelector('.what').textContent = a.app + ' · ' + act;
    row.querySelector('.who').textContent = a.requestedBy + ' · ' + fmtAgo(a.ageSec) + ' ago';
    if (a.status === 'pending') {
      const ok = Object.assign(document.createElement('button'), {className: 'ok', textContent: 'Approve'});
      const no = Object.assign(document.createElement('button'), {className: 'no', textContent: 'Deny'});
      ok.addEventListener('click', () => decideApprovalUi(a.id, true));
      no.addEventListener('click', () => decideApprovalUi(a.id, false));
      row.append(ok, no);
    } else {
      row.appendChild(Object.assign(document.createElement('span'), {className: 'st',
        textContent: a.status + (a.decidedBy ? ' by ' + a.decidedBy : '')}));
    }
    box.appendChild(row);
  }
}

async function decideApprovalUi(id, approve) {
  try {
    await fetch('/api/approvals/decide', {method: 'POST', body: JSON.stringify({id, approve})});
  } catch {}
  pollApprovals();
}

document.getElementById('apprBtn').addEventListener('click', () => {
  apprOpen = !apprOpen;
  document.getElementById('apprPanel').hidden = !apprOpen;
  if (apprOpen) pollApprovals();
});
setInterval(pollApprovals, 3000);
pollApprovals();
)HTML"
R"HTML(

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
// The slider is spaced by TIME, not by frame count, so a gap with no screenshots
// reads as a gap (the image just doesn't change through it) instead of looking
// like a normal interval where nothing happened.
let thumbLive = true;    // false while a past frame is pinned by the slider
let thumbTimes = [];     // YYYYMMDD-HHMMSS ascending
let thumbEpochs = [];    // ms, parallel to thumbTimes
let scrubDate = '';      // '' = today (slider's max = live); else a past day (static)
let scrubPlay = null;

function tsEpoch(ts) {
  return new Date(+ts.slice(0, 4), +ts.slice(4, 6) - 1, +ts.slice(6, 8),
                  +ts.slice(9, 11), +ts.slice(11, 13), +ts.slice(13, 15)).getTime();
}
function frameAt(t) {                       // newest frame at or before time t; -1 if none
  let idx = -1;
  for (let i = 0; i < thumbEpochs.length && thumbEpochs[i] <= t; i++) idx = i;
  return idx;
}
function showFrame(i) {
  if (i < 0 || i >= thumbTimes.length) return;
  const img = document.querySelector('#dThumbWrap img');
  img.src = '/api/thumb/' + encodeURIComponent(detailId) + '?ts=' + thumbTimes[i];
  img.hidden = false;
}

// date '' = today (newest = live); a YYYY-MM-DD past day is all historical.
async function loadThumbTimes(id, date = '') {
  scrubDate = date;
  thumbTimes = [];
  const q = date ? '?date=' + encodeURIComponent(date) : '';
  try { thumbTimes = (await (await fetch('/api/thumbtimes/' + encodeURIComponent(id) + q)).json()).times || []; } catch {}
  thumbEpochs = thumbTimes.map(tsEpoch);
  const sc = document.getElementById('dScrub');
  const has = thumbTimes.length > 0;
  document.getElementById('dScrubWrap').hidden = !has;
  if (has) {
    sc.min = thumbEpochs[0];
    sc.max = thumbEpochs[thumbEpochs.length - 1];
    sc.step = Math.max(1000, Math.floor((sc.max - sc.min) / 800));
    sc.value = sc.max;                      // newest
  }
  if (date) { thumbLive = false; if (has) showFrame(thumbTimes.length - 1); }
  else { thumbLive = true; }
  updateScrubLabel();
}

function updateScrubLabel() {
  const sc = document.getElementById('dScrub');
  const badge = document.getElementById('dScrubTime');
  if (!scrubDate && +sc.value >= +sc.max) { badge.textContent = 'live'; badge.classList.add('live'); return; }
  badge.classList.remove('live');
  const ts = thumbTimes[frameAt(+sc.value)] || thumbTimes[0];
  const day = scrubDate || 'today';
  badge.textContent = ts ? day + ' ' + ts.slice(9, 11) + ':' + ts.slice(11, 13) : day;
}

function stopScrubPlay() {
  if (scrubPlay) { clearInterval(scrubPlay); scrubPlay = null; }
  const b = document.getElementById('dScrubPlay');
  if (b) b.innerHTML = '&#9654;';
}

document.getElementById('dScrub').addEventListener('input', () => {
  const sc = document.getElementById('dScrub');
  const t = +sc.value;
  if (!scrubDate && t >= +sc.max) {         // today's newest = live
    thumbLive = true;
    dThumbSeq = -1;                          // force a live refresh next poll
  } else {
    thumbLive = false;
    showFrame(frameAt(t));
  }
  updateScrubLabel();
});

document.getElementById('dScrubPlay').addEventListener('click', () => {
  if (scrubPlay) { stopScrubPlay(); return; }
  if (thumbTimes.length < 2) return;
  document.getElementById('dScrubPlay').innerHTML = '&#10073;&#10073;';   // pause glyph
  const sc = document.getElementById('dScrub');
  // Timelapse steps frame-by-frame (skipping gaps), moving the time slider to
  // each frame's timestamp; loops back to the first at the end.
  let i = Math.max(0, frameAt(+sc.value));
  scrubPlay = setInterval(() => {
    i = (i + 1) % thumbTimes.length;
    sc.value = thumbEpochs[i];
    sc.dispatchEvent(new Event('input'));
  }, 200);                                             // ~5 fps timelapse
});

function fmtUptime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? `${h}h${m}m` : m > 0 ? `${m}m${s % 60}s` : `${s}s`;
}

// Coarse "how long ago" — sec/min/hours/days/weeks/months/years, one unit.
function fmtAgo(s) {
  s = Math.floor(s);
  if (s < 60) return s + ' sec';
  const m = Math.floor(s / 60);        if (m < 60) return m + (m === 1 ? ' min' : ' mins');
  const h = Math.floor(m / 60);        if (h < 24) return h + (h === 1 ? ' hour' : ' hours');
  const d = Math.floor(h / 24);        if (d < 7)  return d + (d === 1 ? ' day' : ' days');
  if (d < 30) { const w = Math.floor(d / 7);   return w + (w === 1 ? ' week' : ' weeks'); }
  if (d < 365) { const mo = Math.floor(d / 30); return mo + (mo === 1 ? ' month' : ' months'); }
  const y = Math.floor(d / 365);       return y + (y === 1 ? ' year' : ' years');
}

// Absolute wall-clock of the last report (from ageSec), e.g. 2026-07-18 11:00:00.
function lastSeenStamp(ageSec) {
  const d = new Date(Date.now() - ageSec * 1000);
  const p = n => String(n).padStart(2, '0');
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' '
       + p(d.getHours()) + ':' + p(d.getMinutes());
}

// Detail view: the absolute last-seen matters (until when was it visible), so
// show the timestamp with the relative age in parens.
function statsLine(app) {
  if (!app.reported) return 'waiting for first report';
  const h = app.health || {};
  const parts = [];
  if (h.fps !== undefined) parts.push(h.fps.toFixed(0) + ' fps');
  if (h.width) parts.push(h.width + 'x' + h.height);
  if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
  if (app.ageSec > STALE_SEC)
    parts.push('last seen ' + lastSeenStamp(app.ageSec) + ' (' + fmtAgo(app.ageSec) + ' ago)');
  return parts.join(' · ');
}

// Wall cards show only uptime + freshness; fps/size live in the detail view.
function wallStats(app) {
  if (!app.reported) return 'waiting for first report';
  const h = app.health || {};
  const parts = [];
  if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
  if (app.ageSec > STALE_SEC) parts.push('last seen ' + fmtAgo(app.ageSec) + ' ago');
  return parts.join(' · ');
}

function card(app) {
  const el = document.createElement('div');
  el.className = 'card';
  el.id = 'app-' + app.id;
  el.innerHTML = `
    <div class="thumbWrap"><span class="none">no thumbnail</span><img hidden>
      <div class="offlay" hidden><span class="who"></span></div></div>
    <div class="meta">
      <span class="name"><span class="dot"></span><span class="gbadge" hidden></span><span class="label"></span><span class="abadge" hidden></span></span>
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
  const dg = document.getElementById('dGroup');
  dg.hidden = !app.group;
  if (app.group) dg.textContent = '[' + app.group + ']';
  // The action buttons ride the command channel, so they only make sense when
  // the agent's WS is connected and the operator can act. Update/Rollback show
  // only if the app allows update (it advertises this); Restart is always
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

function triggerDownload(href, filename) {
  const a = document.createElement('a');
  a.href = href; a.download = filename;
  document.body.appendChild(a); a.click(); a.remove();
}

// Compact timestamp for filenames, e.g. 20260718-110000.
function fileStamp() {
  const d = new Date(), p = n => String(n).padStart(2, '0');
  return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-'
       + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
}

// Instant screenshot -> download. The button lives in the live-gated row, so it
// relays tc_get_screenshot for a fresh full-res PNG; on any relay failure it
// falls back to the last thumbnail on the server, so a click always yields a file.
document.getElementById('dShot').addEventListener('click', async () => {
  if (!detailId) return;
  const id = detailId;
  const btn = document.getElementById('dShot');
  const app = lastApps.find(a => a.id === id);
  btn.disabled = true;
  btn.textContent = 'Grabbing...';
  try {
    if (app && app.live) {
      const r = await sendCommand(id, { action: 'call', tool: 'tc_get_screenshot', args: { format: 'png' } });
      if (r.ok && r.result && r.result.data && r.result.mimeType) {
        const ext = r.result.mimeType.indexOf('jpeg') >= 0 ? 'jpg' : 'png';
        triggerDownload('data:' + r.result.mimeType + ';base64,' + r.result.data,
                        id + '-' + fileStamp() + '.' + ext);
        return;
      }
    }
    // fallback: latest stored thumbnail (JPEG); cache-bust so it's the newest
    triggerDownload('/api/thumb/' + encodeURIComponent(id) + '?dl=' + Date.now(),
                    id + '-' + fileStamp() + '-thumb.jpg');
  } finally {
    if (detailId === id) { btn.disabled = false; btn.textContent = 'Screenshot'; }
  }
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
  // app advertises caps.control). myRole===null is open mode (allowed).
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

// DOM MouseEvent.button (0:L, 1:M, 2:R) → sokol/tc convention (0:L, 1:R, 2:M).
function tcButton(domBtn) { return [0, 2, 1][domBtn] ?? 0; }

let liveDown = null;   // in-progress press: { start, sx, sy, pressSent, button }
const liveImg = document.getElementById('dLiveImg');

// Pointer events (not mouse events) so a touchscreen drag also becomes a drag.
// setPointerCapture on the img keeps pointermove flowing even when the finger
// leaves the frame — so window-level listeners aren't needed for completion.
liveImg.addEventListener('pointerdown', e => {
  if (!ctlOn()) return;
  e.preventDefault();
  liveImg.setPointerCapture(e.pointerId);
  liveDown = { start: liveToApp(e), sx: e.clientX, sy: e.clientY,
               pressSent: false, button: tcButton(e.button),
               pointerId: e.pointerId };
});
// Throttled to 50ms. Unthrottled, a fast drag flooded the command channel
// (one HTTP POST per event) and stalled the live-view IMG refresh.
let lastDragSent = 0;
liveImg.addEventListener('pointermove', e => {
  if (!liveDown || e.pointerId !== liveDown.pointerId) return;
  if (!liveDown.pressSent) {
    // Click vs drag: only once the pointer travels a few px is it a drag
    // (press at the origin, then move); a still press+release stays a click.
    if (Math.abs(e.clientX - liveDown.sx) < 3 && Math.abs(e.clientY - liveDown.sy) < 3) return;
    liveDown.pressSent = true;
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_press',
                            args: { x: liveDown.start.x, y: liveDown.start.y, button: liveDown.button } });
  }
  const now = Date.now();
  if (now - lastDragSent < 50) return;
  lastDragSent = now;
  const p = liveToApp(e);
  // button on tc_mouse_move promotes it to a drag on the app side; without it,
  // the app's mouseDragged handler never fires (only mouseMoved does).
  sendCommand(detailId, { action: 'call', tool: 'tc_mouse_move',
                          args: { x: p.x, y: p.y, button: liveDown.button } });
});
// Hover passthrough: mouse/pen movement without a button. Throttled to ~20/s
// — the command channel is one HTTP POST per event, so unthrottled hover
// would flood it. Touch has no true hover; drags are handled above and
// non-drag touch movement doesn't fire pointermove.
let lastHoverSent = 0;
liveImg.addEventListener('pointermove', e => {
  if (!ctlOn() || liveDown || e.pointerType === 'touch') return;
  const now = Date.now();
  if (now - lastHoverSent < 50) return;
  lastHoverSent = now;
  const p = liveToApp(e);
  sendCommand(detailId, { action: 'call', tool: 'tc_mouse_move', args: { x: p.x, y: p.y } });
});

function endPointer(e) {
  if (!liveDown || e.pointerId !== liveDown.pointerId) return;
  const down = liveDown;
  liveDown = null;
  if (down.pressSent) {
    const p = liveToApp(e);
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_release',
                            args: { x: p.x, y: p.y, button: down.button } });
  } else {
    sendCommand(detailId, { action: 'call', tool: 'tc_mouse_click',
                            args: { x: down.start.x, y: down.start.y, button: down.button } });
  }
}
liveImg.addEventListener('pointerup', endPointer);
liveImg.addEventListener('pointercancel', endPointer);

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
    const gb = el.querySelector('.gbadge');
    gb.hidden = !app.group;
    if (app.group) gb.textContent = '[' + app.group + ']';
    el.querySelector('.stats').textContent = wallStats(app);
    // Offline: the last screenshot is blurred (CSS) with a text overlay on top.
    const off = el.querySelector('.offlay');
    off.hidden = !offline;
    if (offline) off.querySelector('.who').textContent = app.id;   // name only; last-seen is in the meta below
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
  for (const id of ['pApps', 'pOps', 'pNotify']) byId(id).hidden = (id !== pane);
}
for (const t of document.querySelectorAll('.sTab'))
  t.addEventListener('click', () => showSettingsPane(t.dataset.pane));

async function openSettings() {
  byId('settings').hidden = false;
  byId('sOpToken').hidden = true;
  byId('sPairOut').hidden = true;
  byId('sShareOut').hidden = true;
  byId('sSinkOut').hidden = true;
  showSettingsPane('pApps');
  loadSettingsApps();
  loadSettingsOps();
  loadSettingsShares();
  loadSettingsSinks();
}

)HTML"
R"HTML(// ---- Notify tab: fleet-wide webhook sinks (sinks.json) --------------------
// Rows are edited in place; Save all posts the whole array and re-arms the
// notifier. Test fires the ROW AS EDITED (not as saved) so a URL can be
// verified before committing.
let sinkRows = [];   // plain config objects backing the table

// The working copy is saved per row: any edit marks the row dirty and reveals
// a Save button beside Test; deletes confirm and persist immediately. "Save
// all" only appears while something is dirty.
function markSinkDirty(cfg) { cfg._dirty = true; renderSinkButtons(); }

function cleanSinkRows() {
  return sinkRows.map(c => { const o = { ...c }; delete o._dirty; return o; });
}

async function saveSinks(note) {
  const out = byId('sSinkOut');
  out.hidden = false;
  try {
    const r = await (await stPost('/api/admin/sinks', cleanSinkRows())).json();
    sinkRows.forEach(c => delete c._dirty);
    out.textContent = (note || 'saved') + ' — ' + r.armed + ' sink(s) armed';
    renderSinkRows();
  } catch {
    out.textContent = 'save failed';
  }
}

function sinkRowEl(cfg, idx) {
  const tr = document.createElement('tr');
  const presets = ['slack', 'discord', 'ntfy', 'uptime-kuma', ''];
  const sel = document.createElement('select');
  sel.style.width = '100%';   // fit the cell: an overflowing select makes the
                              // td paint its text-overflow ellipsis as a stray dot
  sel.replaceChildren(...presets.map(p =>
    new Option(p === '' ? 'generic' : p, p, false, (cfg.preset || '') === p)));
  sel.addEventListener('change', () => {
    cfg.preset = sel.value;
    if (cfg.preset) delete cfg.body;   // a stale body would override the preset
    cfg._dirty = true;
    renderSinkRows();
  });
  const url = Object.assign(document.createElement('input'),
                            {value: cfg.url || '', placeholder: 'https://hooks...'});
  url.style.width = '95%';
  url.addEventListener('input', () => { cfg.url = url.value.trim(); markSinkDirty(cfg); });
  const ev = Object.assign(document.createElement('input'),
                           {value: (cfg.events || []).join(','), placeholder: 'all'});
  ev.style.width = '90%';
  ev.addEventListener('input', () => {
    cfg.events = ev.value.split(',').map(s => s.trim()).filter(Boolean);
    if (!cfg.events.length) delete cfg.events;
    markSinkDirty(cfg);
  });
  const sc = Object.assign(document.createElement('input'),
                           {value: (cfg.scope || []).join(','), placeholder: 'all apps'});
  sc.style.width = '90%';
  sc.addEventListener('input', () => {
    cfg.scope = sc.value.split(',').map(s => s.trim()).filter(Boolean);
    if (!cfg.scope.length) delete cfg.scope;
    markSinkDirty(cfg);
  });
  const save = Object.assign(document.createElement('button'),
                             {className: 'sBtn sinkSave', textContent: 'Save'});
  save.hidden = !cfg._dirty;
  save.addEventListener('click', () => saveSinks());
  const test = Object.assign(document.createElement('button'),
                             {className: 'sBtn', textContent: 'Test'});
  test.addEventListener('click', async () => {
    test.disabled = true; test.textContent = '...';
    let r;
    try { r = await (await stPost('/api/admin/sinks/test', cfg)).json(); }
    catch { r = { ok: false, error: 'request failed' }; }
    test.disabled = false; test.textContent = 'Test';
    const out = byId('sSinkOut');
    out.hidden = false;
    out.textContent = r.ok ? 'test delivered (' + (r.status || '?') + ')'
                           : 'test FAILED: ' + (r.error || 'unknown');
  });
  const del = Object.assign(document.createElement('button'),
                            {className: 'sXBtn', title: 'remove this notification'});
  del.addEventListener('click', () => {
    const what = (cfg.preset || 'generic') + (cfg.url ? ' → ' + cfg.url : '');
    if (!confirm('Remove this notification?\n' + what)) return;
    sinkRows.splice(idx, 1);
    saveSinks('removed');   // deletes persist immediately
  });
  for (const el of [sel, url, ev, sc]) {
    const td = document.createElement('td');
    td.appendChild(el);
    tr.appendChild(td);
  }
  const acts = document.createElement('td');
  acts.className = 'acts';
  acts.append(save, test, del);
  tr.appendChild(acts);
  return tr;
}

// Re-sync only the per-row Save buttons + "Save all" visibility (cheap; the
// full renderSinkRows would drop input focus mid-typing).
function renderSinkButtons() {
  const rows = byId('sSinks').querySelectorAll('tr');
  let i = 0;
  for (const cfg of sinkRows) {
    const btn = rows[i] && rows[i].querySelector('.sinkSave');
    if (btn) btn.hidden = !cfg._dirty;
    i += cfg.preset ? 1 : 2;   // generic rows have a body sub-row
  }
  byId('sSinkSave').hidden = !sinkRows.some(c => c._dirty);
}

// A generic sink gets a second row: the JSON body template with the four
// placeholders. Presets keep their body to themselves (that's the point).
function sinkBodyRowEl(cfg) {
  const tr = document.createElement('tr');
  const td = document.createElement('td');
  td.colSpan = 5;
  const body = Object.assign(document.createElement('input'), {
    value: cfg.body || '',
    placeholder: '{"app":"{{app}}","event":"{{event}}","msg":"{{msg}}","time":"{{time}}"}  (blank = this default)',
  });
  body.style.width = '97%';
  body.style.font = '11px ui-monospace, Menlo, monospace';
  body.addEventListener('input', () => {
    cfg.body = body.value.trim();
    if (!cfg.body) delete cfg.body;
    markSinkDirty(cfg);
  });
  td.appendChild(body);
  tr.appendChild(td);
  return tr;
}

function renderSinkRows() {
  const tb = byId('sSinks');
  if (!sinkRows.length) {
    tb.replaceChildren(stMsg(5, 'no notifications yet — Add one and Test it'));
    byId('sSinkSave').hidden = true;
    return;
  }
  tb.replaceChildren(...sinkRows.flatMap((cfg, i) => {
    const rows = [sinkRowEl(cfg, i)];
    if (!cfg.preset) rows.push(sinkBodyRowEl(cfg));
    return rows;
  }));
  byId('sSinkSave').hidden = !sinkRows.some(c => c._dirty);
}

async function loadSettingsSinks() {
  try { sinkRows = await (await fetch('/api/admin/sinks')).json(); } catch { sinkRows = []; }
  if (!Array.isArray(sinkRows)) sinkRows = [];
  renderSinkRows();
}

byId('sSinkAdd').addEventListener('click', () => {
  sinkRows.push({ preset: 'slack', url: '', _dirty: true });
  renderSinkRows();
});

byId('sSinkSave').addEventListener('click', () => saveSinks());
)HTML"
R"HTML(

// One "Apps" table over every app-id this server knows: the union of apps
// that have reported and app-ids that hold an agent token (an app can be
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
    hb.className = 'sBtn'; hb.textContent = a.hidden ? 'Show' : 'Hide';
    hb.addEventListener('click', async () => {
      await stPost('/api/admin/app/hide', { app: a.id, hidden: !a.hidden });
      loadSettingsApps();
    });
    tdX.append(hb);
    if (a.hash !== null) {
      const rv = document.createElement('button');
      rv.className = 'sXBtn'; rv.title = 'Revoke this token';
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
    sset.className = 'sBtn'; sset.textContent = 'Set'; sset.style.marginLeft = '6px';
    sset.addEventListener('click', async () => {
      await stPost('/api/admin/operator/scope', { name: o.name, scope: si.value.trim() });
      sset.textContent = 'Set!'; setTimeout(() => sset.textContent = 'Set', 1200);
    });
    // Plain cell (not .acts): the input starts at the column's left edge, in
    // line with the SCOPE header and the tfoot's new-entry field.
    const tdS = document.createElement('td'); tdS.append(si, sset);
    const lc = document.createElement('button');
    lc.className = 'sBtn'; lc.textContent = 'Login code';
    lc.addEventListener('click', async () => {
      const r = await stPost('/api/admin/login-code', { name: o.name });
      if (r.ok) { const j = await r.json();
        stReveal('sOpToken', 'login code for ' + o.name + ': ' + j.code + '  (valid 10 min)'); }
    });
    const rv = document.createElement('button');
    rv.className = 'sXBtn'; rv.title = 'Revoke this operator';
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
    rv.className = 'sXBtn'; rv.title = 'Revoke this share link';
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
      + 'on the app machine: anchorbolt start --pair ' + j.code + ' --server <this server url>');
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

// Webhook setup guide behind /help/notify — kept deliberately short: each
// service is three steps ending in "paste the URL". A Slack-specific connect
// button was considered and rejected: webhook creation happens inside Slack's
// own admin UI either way, so a button can't shorten anything.
const char* kNotifyHelpHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AnchorBolt — webhook setup</title>
<style>
  body { background: #16181d; color: #d4d7dd; margin: 0 auto; max-width: 720px;
         padding: 32px 20px 64px;
         font: 14px/1.7 -apple-system, "Segoe UI", Roboto, sans-serif; }
  h1 { font-size: 20px; letter-spacing: .03em; }
  h2 { font-size: 16px; margin-top: 32px; border-bottom: 1px solid #2a2e36;
       padding-bottom: 6px; }
  ol { padding-left: 22px; }
  li { margin: 6px 0; }
  code { background: #23262d; border-radius: 4px; padding: 1px 6px;
         font: 12px ui-monospace, "SF Mono", Menlo, monospace; }
  a { color: #79b8ff; }
  .muted { color: #7d838e; font-size: 13px; }
</style>
</head>
<body>
<h1>Getting a webhook URL</h1>
<p>Every notify sink boils down to one thing: a URL AnchorBolt can POST to.
Create it in the service below, paste it into the <b>webhook url</b> field on
the Notify tab, hit <b>Test</b>, then <b>Save all</b>.</p>

<h2>Slack</h2>
<ol>
  <li>Open <a href="https://api.slack.com/apps" target="_blank" rel="noopener">api.slack.com/apps</a>
      &rarr; <b>Create New App</b> &rarr; From scratch, pick your workspace.</li>
  <li>In the app: <b>Incoming Webhooks</b> &rarr; toggle <b>On</b> &rarr;
      <b>Add New Webhook to Workspace</b> &rarr; choose the channel.</li>
  <li>Copy the generated <code>https://hooks.slack.com/services/...</code> URL.</li>
</ol>

<h2>Discord</h2>
<ol>
  <li>Server settings of the target channel &rarr; <b>Integrations</b> &rarr;
      <b>Webhooks</b> &rarr; <b>New Webhook</b>.</li>
  <li>Pick the channel, then <b>Copy Webhook URL</b>
      (<code>https://discord.com/api/webhooks/...</code>).</li>
</ol>

<h2>ntfy (no account needed)</h2>
<ol>
  <li>Pick a topic name nobody would guess, e.g. <code>myfleet-x7k2q9</code>.</li>
  <li>The URL is just <code>https://ntfy.sh/&lt;topic&gt;</code>.</li>
  <li>Subscribe to the same topic in the ntfy phone app / web app.</li>
</ol>

<h2>Uptime Kuma</h2>
<ol>
  <li>Add a monitor of type <b>Push</b>; Kuma shows a push URL
      (<code>.../api/push/&lt;token&gt;</code>). Paste that.</li>
  <li>Scope the sink to exactly one app (<code>app:&lt;id&gt;</code>) — Kuma
      gets pinged while that app's heartbeats stay fresh and alerts when
      they stop.</li>
</ol>

<p class="muted">The webhook URL is a secret: anyone holding it can post to
your channel. It is stored on the server (sinks.json in the data directory)
and shown only on the admin Notify tab.</p>
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
    // valid token for its app id. Secure by default — an app reaches the fleet
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
    auto cookieNamed = [](const httplib::Request& req, const char* name) -> string {
        string c = req.get_header_value("Cookie");
        string key = string(name) + "=";
        size_t pos = c.find(key);
        if (pos == string::npos) return "";
        pos += key.size();
        size_t end = c.find(';', pos);
        return c.substr(pos, end == string::npos ? string::npos : end - pos);
    };
    // Two cookies: abtoken is the real login (persistent), abshare is a share
    // session (browser-session cookie) that OVERLAYS it while present — so
    // opening a share link shows the shared view without forgetting who you
    // are; exiting the share (or closing the browser) restores the login.
    auto cookieToken = [cookieNamed, dataDir](const httplib::Request& req) -> string {
        string sh = cookieNamed(req, "abshare");
        if (!sh.empty() && token::verifyOperator(dataDir.string(), sh)) return sh;
        return cookieNamed(req, "abtoken");
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
            auto jpg = b64decode(data);
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
        // Share tokens land in their own SESSION cookie so a share link never
        // overwrites the real login (see cookieToken); everything else is the
        // persistent login cookie.
        if (tok.rfind("sh-", 0) == 0) {
            res.set_header("Set-Cookie", "abshare=" + tok +
                           "; HttpOnly; SameSite=Lax; Path=/");
        } else {
            res.set_header("Set-Cookie", "abtoken=" + tok +
                           "; HttpOnly; SameSite=Lax; Path=/; Max-Age=2592000");
        }
        res.set_content(Json({{"name", op->name}, {"role", op->role}}).dump(),
                        "application/json");
    });

    // Self-service device add: a logged-in operator mints a login code for
    // THEMSELVES (another session, same identity — no admin needed, no op-
    // token ever travels through a chat). Share overlays can't: "share" is a
    // pseudo-identity with no operator record to log into.
    svr.Post("/api/my/login-code", [dataDir, cookieToken](const httplib::Request& req, httplib::Response& res) {
        string d = dataDir.string();
        auto op = token::verifyOperator(d, cookieToken(req));
        if (!op) { res.status = 401; res.set_content("unauthorized", "text/plain"); return; }
        if (op->name == "share") { res.status = 403; res.set_content("forbidden", "text/plain"); return; }
        string code = token::mintCode(d, "login", op->name, 600);
        res.set_content(Json({{"code", code}, {"expiresSec", 600}}).dump(), "application/json");
    });

    // Drop only the share overlay: back to whoever was logged in underneath.
    svr.Post("/api/share/exit", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", "abshare=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
        res.set_content("ok", "text/plain");
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
                               {"serverInfo", {{"name", "anchorbolt"}, {"version", kAnchorboltVersion}}}};
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
        res.set_header("Set-Cookie", "abshare=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
        res.set_content("ok", "text/plain");
    });

    // Who am I? Drives the login overlay and role-based UI hiding.
    svr.Get("/api/me", [dataDir, cookieToken, cookieNamed](const httplib::Request& req, httplib::Response& res) {
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
        Json me = {{"open", false}, {"name", op->name}, {"role", op->role}};
        // Viewing through a share overlay? Say so, and whether a real login is
        // waiting underneath (drives the "shared view — exit" capsule).
        string sh = cookieNamed(req, "abshare");
        if (!sh.empty() && token::verifyOperator(d, sh)) {
            me["share"] = true;
            me["hasLogin"] = token::verifyOperator(d, cookieNamed(req, "abtoken")).has_value();
        }
        res.set_content(me.dump(), "application/json");
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
        bool hidden = body.value("hidden", true);
        // hidden is admin-set -> persisted in the config roster, mirrored here.
        token::setHidden(dataDir.string(), app, hidden);
        { lock_guard<mutex> lock(g_appsMutex); g_apps[app].hidden = hidden; }
        res.set_content("ok", "text/plain");
    });

    // Approvals: pending + recently decided mutating calls. Any logged-in role
    // may look (the dashboard badge), scope-filtered like everything else.
    svr.Get("/api/approvals", [dataDir, requireRole, currentOp](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 1)) return;
        auto op = currentOp(req);
        string d = dataDir.string();
        int64_t now = time(nullptr);
        Json list = Json::array();
        lock_guard<mutex> lock(g_approvalsMutex);
        for (auto& [id, a] : g_approvals) {
            if (op && !token::inScope(*op, a.app, token::groupOf(d, a.app))) continue;
            // decided entries linger 10 min for context, then drop off the list
            if (a.status != "pending" && now - a.created > 600) continue;
            list.push_back(approvalJson(id, a));
        }
        res.set_content(list.dump(), "application/json");
    });

    // Human decision from the dashboard: operator+ whose scope covers the app.
    svr.Post("/api/approvals/decide", [dataDir, requireRole, currentOp, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 2)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        string id = body.value("id", "");
        bool approve = body.value("approve", false);
        auto op = currentOp(req);
        {
            lock_guard<mutex> lock(g_approvalsMutex);
            auto it = g_approvals.find(id);
            if (it == g_approvals.end()) { res.status = 404; res.set_content("unknown approval id", "text/plain"); return; }
            if (op && !token::inScope(*op, it->second.app,
                                      token::groupOf(dataDir.string(), it->second.app))) {
                res.status = 404; res.set_content("unknown approval id", "text/plain"); return;
            }
        }
        string err;
        if (!decideApproval(dataDir, id, approve, op ? op->name : "operator", &err)) {
            res.status = 409;
            res.set_content(err, "text/plain");
            return;
        }
        Json out = {{"ok", true}};
        {
            lock_guard<mutex> lock(g_approvalsMutex);
            auto it = g_approvals.find(id);
            if (it != g_approvals.end()) out["approval"] = approvalJson(id, it->second);
        }
        res.set_content(out.dump(), "application/json");
    });

    // Notify sinks (settings "Notify" tab): the raw sinks.json array. Admin
    // only — webhook URLs are secrets, and admins are the ones who manage them.
    svr.Get("/api/admin/sinks", [dataDir, requireRole](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json arr = Json::array();
        ifstream in(sinksPath(dataDir));
        if (in) {
            try {
                Json j = Json::parse(in);
                if (j.is_array()) arr = j;
                else if (j.is_object() && j["sinks"].is_array()) arr = j["sinks"];
            } catch (...) {}
        }
        res.set_content(arr.dump(), "application/json");
    });

    // Replace the whole sink list and re-arm the notifier immediately.
    svr.Post("/api/admin/sinks", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        if (!body.is_array()) { res.status = 400; res.set_content("expected a JSON array", "text/plain"); return; }
        {
            ofstream out(sinksPath(dataDir));
            out << body.dump(2) << "\n";
        }
        int n = loadServeSinks(dataDir);
        res.set_content(Json{{"ok", true}, {"armed", n}}.dump(), "application/json");
    });

    // Fire a "test" event through ONE sink config (as posted, not as saved) so
    // a URL can be verified before committing it. Synchronous: waits for the
    // delivery attempt and reports whether it went out.
    svr.Post("/api/admin/sinks/test", [dataDir, requireRole, parseBody](const httplib::Request& req, httplib::Response& res) {
        if (!requireRole(req, res, 3)) return;
        Json body;
        if (!parseBody(req, res, body)) return;
        auto cfgs = parseSinks(Json::array({body}), dataDir);
        if (cfgs.empty()) { res.status = 400; res.set_content("invalid sink (missing url?)", "text/plain"); return; }
        const SinkConfig& c = cfgs[0];
        // One direct, synchronous delivery so the outcome can be REPORTED —
        // the whole point of a test button. Heartbeat (kuma) sinks get one
        // ?status=up ping; event sinks get a rendered "test" event.
        tcx::curl::HttpClient cli;
        cli.setBaseUrl(c.url);
        cli.setTimeout(5);
        cli.setFollowRedirects(true);
        tcx::curl::HttpResponse r;
        if (c.heartbeat) {
            string sep = c.url.find('?') == string::npos ? "?" : "&";
            r = cli.request(c.method, sep + "status=up&msg=test", "", c.contentType);
        } else {
            string tmplBody = c.bodyTemplate;
            bool js = c.contentType.find("json") != string::npos;
            auto sub = [&](const char* key, const string& v) {
                string esc = v;
                if (js) {
                    string o;
                    for (char ch : esc) {
                        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
                        else if (ch == '\n') o += "\\n";
                        else o += ch;
                    }
                    esc = o;
                }
                for (size_t pos = 0; (pos = tmplBody.find(key, pos)) != string::npos;
                     pos += esc.size())
                    tmplBody.replace(pos, strlen(key), esc);
            };
            sub("{{app}}", "test");
            sub("{{event}}", "test");
            sub("{{msg}}", "test notification from AnchorBolt");
            sub("{{time}}", getTimestampString("%Y-%m-%dT%H:%M:%S"));
            r = cli.request(c.method, "", tmplBody, c.contentType);
        }
        res.set_content(Json{{"ok", r.ok()},
                            {"status", r.statusCode},
                            {"error", r.ok() ? "" : (r.statusCode ? "HTTP " + to_string(r.statusCode) : r.error)}}.dump(),
                        "application/json");
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
        string id = body.value("id", "");
        bool ok = token::revokeAgent(dataDir.string(), id);
        if (!ok) { res.status = 404; res.set_content("no such agent", "text/plain"); return; }
        // Revoke = decommission: drop it from the wall now (it can't report
        // again) unless it was deliberately hidden. Persist the removal.
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto it = g_apps.find(id);
            if (it != g_apps.end() && !it->second.hidden) g_apps.erase(it);
        }
        flushAppRegistry(dataDir);
        res.set_content("ok", "text/plain");
    });

    // Mint a single-use pairing code for an app. Its supervisor redeems it
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
        logNotice("anchorbolt") << "paired app '" << app << "' via code";
        res.set_content(dumpSafeS(Json({{"app", app}, {"token", tok}})), "application/json");
    });

    // Token -> id: an app started with only its agent token (no client-side
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

    // Webhook setup guide, linked from the settings Notify tab (opens in a new
    // tab). Static documentation, no secrets — served without auth.
    svr.Get("/help/notify", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kNotifyHelpHtml, "text/html");
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
        fs::path dir = appContentDir(dataDir, id);
        error_code ec;
        fs::create_directories(dir, ec);
        Json line = {{"at", getTimestampString("%Y-%m-%dT%H:%M:%S")}, {"health", body}};
        ofstream out(dir / ("heartbeat-" + getTimestampString("%Y-%m-%d") + ".jsonl"), ios::app);
        out << line.dump() << "\n";
        // Tell the agent whether we currently hold its command-channel WS. The
        // heartbeat is HTTP (independent of the WS), so it still reaches us when
        // the WS is half-open — e.g. after a serve restart or a dropped tunnel,
        // where the agent's socket looks Open but we have no record of it. The
        // agent reconnects on wsConnected=false, restoring live/control.
        res.set_content(Json{{"ok", true}, {"wsConnected", g_agents.live(id)}}.dump(),
                        "application/json");
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
        // Dedup: a static screen re-encodes to byte-identical JPEG (deterministic
        // encoder, no EXIF). Skip storing a frame equal to the last one — the
        // stored frames become change points, and the scrubber's "newest frame
        // at or before t" holds the last one across the gap, which is exactly
        // what was on screen. Liveness is untouched (heartbeats are a separate
        // stream). On a server restart the first push always stores (memory is
        // empty), costing at most one redundant frame.
        bool dup = false;
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto& st = g_apps[id];
            dup = st.thumb.size() == req.body.size() &&
                  memcmp(st.thumb.data(), req.body.data(), req.body.size()) == 0;
            if (!dup) {
                st.thumb.assign(req.body.begin(), req.body.end());
                ++st.thumbSeq;
            }
        }
        if (!dup) {
            fs::path dir = appContentDir(dataDir, id) / "thumbs";
            error_code ec;
            fs::create_directories(dir, ec);
            ofstream out(dir / (getTimestampString("%Y%m%d-%H%M%S") + ".jpg"), ios::binary);
            out.write(req.body.data(), (streamsize)req.body.size());
        }
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
        // Dedup per image name (same rule as thumbnails above).
        bool dup = false;
        {
            lock_guard<mutex> lock(g_appsMutex);
            auto& slot = g_apps[id].images[name];
            dup = slot.jpeg.size() == req.body.size() &&
                  memcmp(slot.jpeg.data(), req.body.data(), req.body.size()) == 0;
            if (!dup) {
                slot.jpeg.assign(req.body.begin(), req.body.end());
                ++slot.seq;
            }
        }
        if (!dup) {
            fs::path dir = appContentDir(dataDir, id) / "images" / name;
            error_code ec;
            fs::create_directories(dir, ec);
            ofstream out(dir / (getTimestampString("%Y%m%d-%H%M%S") + ".jpg"), ios::binary);
            out.write(req.body.data(), (streamsize)req.body.size());
        }
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
        fs::path dir = appContentDir(dataDir, id);
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
        addJpegs(appContentDir(dataDir, id) / "thumbs", "thumbs/");
        for (auto& nd : fs::directory_iterator(appContentDir(dataDir, id) / "images", ec)) {
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
            ifstream in(appContentDir(dataDir, id) / ("log-" + date + ".jsonl"));
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
        fs::path dir = appContentDir(dataDir, id);
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
            // Fan out to serve-side sinks too (fleet-wide notify config); the
            // app's own sinks fire independently — both configured = both
            // deliver, by design.
            serveNotify(dataDir, id, ev, line.text);
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
        ifstream in(appContentDir(dataDir, id) / ("heartbeat-" + getTimestampString("%Y-%m-%d") + ".jsonl"));
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
            // A provisioned app (agent token / WS connect) can be in the map
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
        for (auto& e : fs::directory_iterator(appContentDir(dataDir, id) / "thumbs", ec)) {
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
            ifstream in(appContentDir(dataDir, id) / "thumbs" / (ts + ".jpg"), ios::binary);
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
    // (last-seen + health + thumbnail), then every token-holding app (so a
    // provisioned-but-quiet app still shows, matching the settings Apps list).
    migrateDataDir(dataDir);            // legacy flat layout -> config/state/apps
    {
        error_code ec;                  // fresh dir: start in the new layout
        fs::create_directories(dataDir / "config", ec);
        fs::create_directories(dataDir / "state", ec);
        fs::create_directories(dataDir / "apps", ec);
    }
    loadAppRegistry(dataDir);
    loadKnownAgents(dataDir);
    loadApprovals(dataDir);
    g_autoApprove = opt.autoApprove;
    g_approvalTtlSec = opt.approvalTtlSec;
    // Runtime pointer for CLI verbs (token, approvals): where the live serve
    // keeps its data. Best-effort; left behind on unclean exit, which is fine —
    // consumers only trust it if the directory still exists.
    datadir::writeServePointer(dataDir, opt.port);
    if (int n = loadServeSinks(dataDir))
        logNotice("anchorbolt") << "notify: " << n << " sink(s) armed (sinks.json)";

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

    // Offline/online watcher for the notify sinks: an app that stops
    // heartbeating can't report its own death — only serve can see the
    // silence. First pass only baselines (a serve restart must not re-announce
    // every long-gone app); transitions after that fire offline / online.
    // Fresh apps also feed per-app uptime-kuma sinks.
    thread offlineWatcher([dataDir, opt]() {
        bool baseline = true;
        while (!g_stop) {
            vector<pair<string, int>> events;   // (id, 0=offline 1=online 2=tick)
            {
                lock_guard<mutex> lock(g_appsMutex);
                auto now = chrono::steady_clock::now();
                for (auto& [id, st] : g_apps) {
                    bool reported = st.lastSeen != chrono::steady_clock::time_point{};
                    if (!reported) continue;
                    bool offline =
                        now - st.lastSeen > chrono::seconds(opt.offlineAfterSec);
                    if (baseline) {
                        st.notifiedOffline = offline;
                    } else if (offline != st.notifiedOffline) {
                        st.notifiedOffline = offline;
                        events.push_back({id, offline ? 0 : 1});
                    }
                    if (!offline) events.push_back({id, 2});
                }
            }
            baseline = false;
            for (auto& [id, kind] : events) {
                if (kind == 0) {
                    serveNotify(dataDir, id, "offline",
                                "no heartbeat for " + to_string(opt.offlineAfterSec) + "s");
                } else if (kind == 1) {
                    serveNotify(dataDir, id, "online", "heartbeat resumed");
                } else {
                    shared_ptr<Notifier> n;
                    {
                        lock_guard<mutex> lk(g_serveNotifierMutex);
                        n = g_serveNotifier;
                    }
                    if (n) n->healthyTick(id);
                }
            }
            for (int i = 0; i < 100 && !g_stop; ++i)
                this_thread::sleep_for(chrono::milliseconds(100));
        }
    });

    // Approval sweeper: expire overdue pending entries, and pick up decisions
    // the `anchorbolt approvals` CLI drops as one file per decision under
    // approval-decisions/ (single writer per file — no shared-file races with
    // our own approvals.json rewrites).
    thread approvalSweeper([dataDir]() {
        fs::path decDir = decisionsDir(dataDir);
        while (!g_stop) {
            for (int i = 0; i < 20 && !g_stop; ++i)
                this_thread::sleep_for(chrono::milliseconds(100));
            error_code ec;
            for (auto& e : fs::directory_iterator(decDir, ec)) {
                if (e.path().extension() != ".json") continue;
                string id = e.path().stem().string();
                Json d;
                try { ifstream in(e.path()); d = Json::parse(in); } catch (...) {}
                fs::remove(e.path(), ec);
                if (!d.is_object()) continue;
                string err;
                decideApproval(dataDir, id, d.value("decision", "") == "approve",
                               d.value("by", "cli"), &err);
            }
            bool dirty = false;
            {
                lock_guard<mutex> lock(g_approvalsMutex);
                int64_t now = time(nullptr);
                for (auto& [id, a] : g_approvals) {
                    if (a.status == "pending" && now > a.expires) {
                        a.status = "expired";
                        dirty = true;
                    }
                }
                if (dirty) flushApprovals(dataDir);
            }
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
    approvalSweeper.join();
    offlineWatcher.join();
    {
        shared_ptr<Notifier> n;
        {
            lock_guard<mutex> lock(g_serveNotifierMutex);
            n = std::exchange(g_serveNotifier, nullptr);
        }
        if (n) n->flushAndStop();   // queued events still get out on shutdown
    }
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

// ---------------------------------------------------------------------------
// `anchorbolt approvals` — server-machine CLI over the same data directory.
// list reads approvals.json; approve/deny drop a one-file-per-decision intent
// under approval-decisions/ that the running serve's sweeper applies (so the
// execution — which needs the agent WS — always happens inside serve), then
// poll for the outcome. Ids accept unique prefixes ("ap-3f" or just "3f");
// with exactly one pending entry the id can be omitted entirely.
int cmdApprovals(const std::vector<std::string>& args) {
    string dataDir;
    vector<string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) dataDir = args[++i];
        else rest.push_back(args[i]);
    }
    // One resolver for every CLI verb (see DataDir.h): --data >
    // ./anchorbolt-data > the running serve's runtime pointer.
    {
        string err;
        dataDir = datadir::resolveDataDir(dataDir, &err);
        if (dataDir.empty()) { cerr << err << endl; return 1; }
    }
    if (!fs::exists(dataDir)) {
        // A missing dir would silently read as "no pending approvals"; say so.
        cerr << "data directory not found: " << dataDir << endl;
        return 1;
    }
    auto load = [&]() -> Json {
        ifstream in(approvalsPath(fs::path(dataDir)));
        if (!in) return Json::object();
        try { return Json::parse(in); } catch (...) { return Json::object(); }
    };
    auto fmtAge = [](int64_t s) -> string {
        if (s < 0) s = 0;
        if (s < 60) return to_string(s) + "s";
        if (s < 3600) return to_string(s / 60) + "m";
        return to_string(s / 3600) + "h";
    };
    auto actionOf = [](const Json& e) {
        string a = e.value("action", "");
        return a == "call" ? "call " + e.value("tool", "?") : a;
    };
    auto resultText = [](const Json& e) -> string {
        if (!e.contains("result") || !e["result"].is_object()) return "";
        const Json& r = e["result"];
        for (auto& c : r.value("content", Json::array()))
            if (c.value("type", "") == "text") return c.value("text", "");
        return "";
    };

    Json all = load();
    string verb = rest.empty() ? "list" : rest[0];

    if (verb == "list") {
        int64_t now = time(nullptr);
        vector<pair<string, Json>> rows;
        for (auto& [id, e] : all.items()) {
            string st = e.value("status", "");
            // pending always; decided entries only for 10 min of context
            if (st != "pending" && now - e.value("created", (int64_t)0) > 600) continue;
            rows.push_back({id, e});
        }
        sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
            return a.second.value("created", (int64_t)0) < b.second.value("created", (int64_t)0);
        });
        if (rows.empty()) { cout << "no pending approvals" << endl; return 0; }
        printf("%-11s %-5s %-24s %-28s %-10s %s\n",
               "ID", "AGE", "APP", "ACTION", "BY", "STATUS");
        for (auto& [id, e] : rows) {
            string st = e.value("status", "");
            string tail = st == "pending"
                ? "expires " + fmtAge(e.value("expires", (int64_t)0) - now)
                : st + (e.contains("decidedBy") ? " by " + e["decidedBy"].get<string>() : "");
            printf("%-11s %-5s %-24s %-28s %-10s %s\n", id.c_str(),
                   fmtAge(now - e.value("created", (int64_t)0)).c_str(),
                   e.value("app", "").c_str(), actionOf(e).c_str(),
                   e.value("requestedBy", "").c_str(), tail.c_str());
        }
        return 0;
    }

    if (verb == "approve" || verb == "deny") {
        // Resolve the target among pending entries: unique prefix, or the only one.
        string want = rest.size() > 1 ? rest[1] : "";
        vector<string> hits;
        for (auto& [id, e] : all.items()) {
            if (e.value("status", "") != "pending") continue;
            if (want.empty() || id.rfind(want, 0) == 0 ||
                id.rfind("ap-" + want, 0) == 0) hits.push_back(id);
        }
        if (hits.empty()) {
            cerr << (want.empty() ? "no pending approvals" : "no pending approval matches '" + want + "'") << endl;
            return 1;
        }
        if (hits.size() > 1) {
            cerr << "ambiguous: " << hits.size() << " pending approvals match";
            if (!want.empty()) cerr << " '" << want << "'";
            cerr << " — run 'anchorbolt approvals list'" << endl;
            return 1;
        }
        string id = hits[0];
        Json entry = all[id];
        fs::path decDir = decisionsDir(fs::path(dataDir));
        error_code ec;
        fs::create_directories(decDir, ec);
        {
            ofstream out(decDir / (id + ".json"));
            if (!out) { cerr << "cannot write to " << decDir.string() << endl; return 1; }
            out << Json{{"decision", verb == "approve" ? "approve" : "deny"},
                        {"by", "cli"}}.dump() << "\n";
        }
        cout << (verb == "approve" ? "approving " : "denying ") << id << ": "
             << actionOf(entry) << " on " << entry.value("app", "")
             << " (requested by " << entry.value("requestedBy", "") << ")" << endl;
        // Wait for the running serve to pick the decision up and (on approve)
        // execute; the sweeper polls every 2s.
        for (int i = 0; i < 15; i++) {
            this_thread::sleep_for(chrono::seconds(1));
            Json cur = load();
            if (!cur.contains(id)) continue;
            string st = cur[id].value("status", "");
            if (st == "pending") continue;
            cout << st;
            if (st == "approved") {
                string txt = resultText(cur[id]);
                if (!txt.empty()) cout << " — " << txt;
            }
            cout << endl;
            return 0;
        }
        cerr << "decision written but serve did not pick it up (is `anchorbolt serve` running with --data "
             << dataDir << "?)" << endl;
        return 1;
    }

    cerr << "usage: anchorbolt approvals [list | approve [id] | deny [id]] [--data <dir>]" << endl;
    return 1;
}
