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

#include <TrussC.h>
#include <impl/httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
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

struct ServeOptions {
    int    port    = 8787;
    string dataDir = "anchorbolt-data";
};

struct AppState {
    Json     health;                                // last heartbeat payload
    chrono::steady_clock::time_point lastSeen{};
    vector<unsigned char> thumb;                    // latest JPEG
    uint64_t thumbSeq = 0;                          // bumped per upload (cache busting)
};

map<string, AppState> g_apps;
mutex g_appsMutex;

atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

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
        if      (a == "-p" || a == "--port") { auto v = next("--port"); if (!v) return false; opt.port = stoi(*v); }
        else if (a == "--data")              { auto v = next("--data"); if (!v) return false; opt.dataDir = *v; }
        else {
            cerr << "anchorbolt serve: unknown option '" << a << "'" << endl;
            return false;
        }
    }
    return true;
}

// Dashboard: one self-contained page, no external assets. Polls /api/apps and
// refreshes each card's thumbnail whenever its upload sequence changes.
const char* kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>anchorbolt</title>
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
          overflow: hidden; }
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
  .meta .stats { color: #7d838e; font-size: 12px; white-space: nowrap; }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
         margin-right: 6px; background: #3fb950; vertical-align: baseline; }
  .stale .dot { background: #f85149; }
  #empty { color: #4a4f59; text-align: center; padding: 80px 20px; }
</style>
</head>
<body>
<header>
  <h1>anchorbolt</h1>
  <span class="sub" id="summary"></span>
</header>
<div id="grid"></div>
<div id="empty" hidden>no apps have reported yet &mdash; run
  <code>anchorbolt start &lt;app&gt; --server &lt;this url&gt;</code></div>
<script>
const STALE_SEC = 10;
const seq = {};   // app id -> last rendered thumbSeq

function fmtUptime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? `${h}h${m}m` : m > 0 ? `${m}m${s % 60}s` : `${s}s`;
}

function card(app) {
  const el = document.createElement('div');
  el.className = 'card';
  el.id = 'app-' + app.id;
  el.innerHTML = `
    <div class="thumbWrap"><span class="none">no thumbnail</span><img hidden></div>
    <div class="meta">
      <span class="name"><span class="dot"></span><span class="label"></span></span>
      <span class="stats"></span>
    </div>`;
  return el;
}

async function refresh() {
  let apps;
  try {
    apps = await (await fetch('/api/apps')).json();
  } catch { return; }

  const grid = document.getElementById('grid');
  document.getElementById('empty').hidden = apps.length > 0;
  const alive = new Set();

  for (const app of apps) {
    alive.add('app-' + app.id);
    let el = document.getElementById('app-' + app.id);
    if (!el) { el = card(app); grid.appendChild(el); }

    const stale = app.ageSec > STALE_SEC;
    el.classList.toggle('stale', stale);
    el.querySelector('.label').textContent = app.id;

    const h = app.health || {};
    const parts = [];
    if (h.fps !== undefined) parts.push(h.fps.toFixed(0) + ' fps');
    if (h.width) parts.push(h.width + 'x' + h.height);
    if (h.uptimeSec !== undefined) parts.push('up ' + fmtUptime(h.uptimeSec));
    if (stale) parts.push('last seen ' + Math.floor(app.ageSec) + 's ago');
    el.querySelector('.stats').textContent = parts.join(' · ');

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

    fs::path dataDir = fs::absolute(opt.dataDir);
    error_code ec;
    fs::create_directories(dataDir, ec);
    if (ec) {
        cerr << "anchorbolt serve: cannot create data dir " << dataDir.string() << endl;
        return 1;
    }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kDashboardHtml, "text/html");
    });

    // Agent push: health JSON. Body = get_health result + {"app": "<id>"}.
    svr.Post("/api/heartbeat", [dataDir](const httplib::Request& req, httplib::Response& res) {
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
    svr.Post(R"(/api/thumb/([^/]+))", [dataDir](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        if (!validAppId(id)) {
            res.status = 400;
            res.set_content("invalid app id", "text/plain");
            return;
        }
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

    svr.Get("/api/apps", [](const httplib::Request&, httplib::Response& res) {
        Json list = Json::array();
        auto now = chrono::steady_clock::now();
        lock_guard<mutex> lock(g_appsMutex);
        for (auto& [id, st] : g_apps) {
            double age = chrono::duration<double>(now - st.lastSeen).count();
            list.push_back({{"id", id},
                            {"ageSec", age},
                            {"thumbSeq", st.thumbSeq},
                            {"health", st.health}});
        }
        res.set_content(list.dump(), "application/json");
    });

    svr.Get(R"(/api/thumb/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
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

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // svr.listen() blocks; a watcher thread turns the signal flag into stop().
    thread stopWatcher([&svr]() {
        while (!g_stop) this_thread::sleep_for(chrono::milliseconds(100));
        svr.stop();
    });

    logNotice("anchorbolt") << "fleet server on http://localhost:" << opt.port
                            << " (data " << dataDir.string() << ")";
    bool ok = svr.listen("0.0.0.0", opt.port);
    bool stoppedBySignal = g_stop.load();
    g_stop = true;
    stopWatcher.join();

    if (!ok && !stoppedBySignal) {
        cerr << "anchorbolt serve: failed to listen on port " << opt.port << endl;
        return 1;
    }
    logNotice("anchorbolt") << "fleet server stopped";
    return 0;
}
