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

struct ServeOptions {
    int    port    = 8787;
    string dataDir = "anchorbolt-data";
};

struct ImageSlot {
    vector<unsigned char> jpeg;
    uint64_t seq = 0;
};

struct AppState {
    Json     health;                                // last heartbeat payload
    chrono::steady_clock::time_point lastSeen{};
    vector<unsigned char> thumb;                    // latest JPEG
    uint64_t thumbSeq = 0;                          // bumped per upload (cache busting)
    map<string, ImageSlot> images;                  // custom statusImage uploads
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
// Clicking a card opens a detail view: live thumbnail, app-published status
// values, time-series graphs (fps / memory / mode=graph customs, fed by
// /api/history), and custom statusImage streams.
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
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
         margin-right: 6px; background: #3fb950; vertical-align: baseline;
         flex: none; }
  .stale .dot, .dot.bad { background: #f85149; }
  #empty { color: #4a4f59; text-align: center; padding: 80px 20px; }

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
  .graph canvas { width: 100%; height: 72px; display: block; }
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

<div id="detail" hidden>
  <div id="dPanel">
    <div class="dhead">
      <span class="dot" id="dDot"></span>
      <h2 id="dTitle"></h2>
      <span class="stats" id="dStats"></span>
      <button id="dClose" title="close">&times;</button>
    </div>
    <div class="dbody">
      <div id="dThumbWrap"><img hidden></div>
      <div id="dValues"></div>
      <div id="dGraphs"></div>
      <div id="dImages"></div>
    </div>
  </div>
</div>

<script>
const STALE_SEC = 10;
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
      <span class="name"><span class="dot"></span><span class="label"></span></span>
      <span class="stats"></span>
    </div>`;
  el.addEventListener('click', () => openDetail(app.id));
  return el;
}

// ---- detail view ----

function openDetail(id) {
  detailId = id;
  dThumbSeq = -1;
  document.getElementById('dImages').replaceChildren();
  document.getElementById('detail').hidden = false;
  renderDetail();
}

function closeDetail() {
  detailId = null;
  document.getElementById('detail').hidden = true;
}
document.getElementById('dClose').addEventListener('click', closeDetail);
document.getElementById('detail').addEventListener('click', e => {
  if (e.target === document.getElementById('detail')) closeDetail();
});
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeDetail(); });

function drawGraph(canvas, pts, color) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const valid = pts.filter(p => p != null && isFinite(p));
  if (!valid.length) return {};
  let min = Math.min(...valid), max = Math.max(...valid);
  if (max - min < 1e-9) { max += 1; min -= 1; }
  const pad = (max - min) * 0.12; min -= pad; max += pad;
  ctx.strokeStyle = color; ctx.lineWidth = 1.5;
  ctx.beginPath();
  let started = false;
  const n = Math.max(pts.length - 1, 1);
  pts.forEach((p, i) => {
    if (p == null || !isFinite(p)) { started = false; return; }
    const x = i / n * w;
    const y = h - 3 - (p - min) / (max - min) * (h - 6);
    started ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    started = true;
  });
  ctx.stroke();
  return { min, max };
}

async function renderDetail() {
  if (!detailId) return;
  const app = lastApps.find(a => a.id === detailId);
  if (!app) return;

  const stale = app.ageSec > STALE_SEC;
  document.getElementById('dDot').classList.toggle('bad', stale);
  document.getElementById('dTitle').textContent = app.id;
  const h = app.health || {};
  const extra = [];
  if (h.version) extra.push(h.version);
  if (h.memoryBytes) extra.push((h.memoryBytes / 1048576).toFixed(1) + ' MB');
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

  // Graphs: fps + memory + every mode=graph custom value
  let hist = [];
  try {
    hist = await (await fetch('/api/history/' + encodeURIComponent(app.id))).json();
  } catch { return; }
  if (detailId !== app.id) return;   // closed/switched while fetching

  const graphs = [
    { label: 'fps',       color: '#58a6ff', get: e => e.health && e.health.fps },
    { label: 'memory MB', color: '#d29922', get: e => e.health && e.health.memoryBytes / 1048576 },
  ];
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

  const span = hist.length >= 2
    ? hist[0].at.slice(11, 16) + ' - ' + hist[hist.length - 1].at.slice(11, 16) : '';
  const cont = document.getElementById('dGraphs');
  cont.replaceChildren(...graphs.map(g => {
    const el = document.createElement('div');
    el.className = 'graph';
    el.innerHTML = `<div class="glabel"><span></span><span class="gval"></span></div><canvas></canvas>`;
    el.querySelector('.glabel span').textContent = g.label + (span ? '  (' + span + ')' : '');
    return el;
  }));
  graphs.forEach((g, i) => {
    const el = cont.children[i];
    const pts = hist.map(g.get);
    drawGraph(el.querySelector('canvas'), pts, g.color);
    const last = [...pts].reverse().find(p => p != null && isFinite(p));
    el.querySelector('.gval').textContent = last != null ? (+last).toFixed(1) : '-';
  });
}

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
    apps = await (await fetch('/api/apps')).json();
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

    // Agent push: custom statusImage as raw JPEG (same shape as thumbnails,
    // but keyed by app + image name).
    svr.Post(R"(/api/image/([^/]+)/([^/]+))", [dataDir](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        string name = req.matches[2];
        if (!validAppId(id) || !validAppId(name)) {
            res.status = 400;
            res.set_content("invalid app or image id", "text/plain");
            return;
        }
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

    svr.Get(R"(/api/image/([^/]+)/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        string name = req.matches[2];
        lock_guard<mutex> lock(g_appsMutex);
        auto it = g_apps.find(id);
        if (it == g_apps.end()) { res.status = 404; return; }
        auto img = it->second.images.find(name);
        if (img == it->second.images.end() || img->second.jpeg.empty()) { res.status = 404; return; }
        res.set_content((const char*)img->second.jpeg.data(), img->second.jpeg.size(), "image/jpeg");
    });

    // Recent heartbeat history for the detail-view graphs: tail of today's
    // JSONL as a JSON array (raw line concatenation — every line is already
    // a JSON object). ~1200 entries = 1h at the default 3s poll.
    svr.Get(R"(/api/history/([^/]+))", [dataDir](const httplib::Request& req, httplib::Response& res) {
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

    svr.Get("/api/apps", [](const httplib::Request&, httplib::Response& res) {
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
