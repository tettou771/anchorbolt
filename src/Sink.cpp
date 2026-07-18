#include "Sink.h"

#include <tcxCurl.h>

#include <fstream>

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

shared_ptr<Notifier> g_notifier;

namespace {

// JSON-string-escape a substituted value (no surrounding quotes) so an app
// name or log line with quotes can't break a JSON body template.
string jsonEscape(const string& v) {
    string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

void replaceAll(string& s, const string& key, const string& val) {
    size_t pos = 0;
    while ((pos = s.find(key, pos)) != string::npos) {
        s.replace(pos, key.size(), val);
        pos += val.size();
    }
}

string hostOf(const string& url) {
    size_t start = url.find("://");
    start = (start == string::npos) ? 0 : start + 3;
    size_t end = url.find_first_of("/:", start);
    return url.substr(start, end == string::npos ? string::npos : end - start);
}

} // namespace

bool sinkCovers(const SinkConfig& cfg, const string& app, const string& group) {
    if (cfg.scope.empty()) return true;
    for (const auto& s : cfg.scope) {
        if (s == "all" || s == "All" || s == "ALL" || s == "*") return true;
        if (s == "app:" + app) return true;
        if (!group.empty() && s == group) return true;
    }
    return false;
}

vector<SinkConfig> parseSinks(const Json& arr, const fs::path& configDir) {
    vector<SinkConfig> out;
    if (!arr.is_array()) return out;
    for (auto& j : arr) {
        if (!j.is_object()) continue;
        SinkConfig c;
        string preset = j.value("preset", "");

        // The webhook URL is effectively a secret (anyone holding it can post
        // to the channel) — same escape hatches as the agent token: inline
        // url, urlEnv (env var name), or urlFile (path next to the config).
        c.url = j.value("url", "");
        if (c.url.empty() && j.contains("urlEnv")) {
            if (const char* v = getenv(j["urlEnv"].get<string>().c_str())) c.url = v;
        }
        if (c.url.empty() && j.contains("urlFile")) {
            fs::path f = configDir / fs::path(j["urlFile"].get<string>());
            ifstream in(f);
            if (in) getline(in, c.url);
            while (!c.url.empty() && (c.url.back() == '\r' || c.url.back() == ' '))
                c.url.pop_back();
        }
        if (c.url.empty()) {
            logWarning("anchorbolt") << "sink skipped: no url (preset '" << preset << "')";
            continue;
        }

        if (preset == "slack") {
            c.bodyTemplate = R"({"text":"[{{app}}] {{event}}: {{msg}}"})";
        } else if (preset == "discord") {
            c.bodyTemplate = R"({"content":"[{{app}}] {{event}}: {{msg}}"})";
        } else if (preset == "ntfy") {
            c.contentType = "text/plain";
            c.bodyTemplate = "[{{app}}] {{event}}: {{msg}}";
        } else if (preset == "uptime-kuma") {
            c.heartbeat = true;
            c.method = "GET";
        } else if (!preset.empty()) {
            logWarning("anchorbolt") << "sink preset '" << preset
                                     << "' unknown; treating as generic";
        }
        // Explicit keys override whatever the preset filled in.
        c.method      = j.value("method", c.method);
        c.contentType = j.value("contentType", c.contentType);
        c.bodyTemplate = j.value("body", c.bodyTemplate);
        c.intervalSec = j.value("interval", c.intervalSec);
        if (j.contains("events") && j["events"].is_array()) {
            for (auto& e : j["events"]) c.events.push_back(e.get<string>());
        }
        if (j.contains("scope") && j["scope"].is_array()) {
            for (auto& s : j["scope"]) if (s.is_string()) c.scope.push_back(s.get<string>());
        }
        if (!c.heartbeat && c.bodyTemplate.empty()) {
            c.bodyTemplate =
                R"({"app":"{{app}}","event":"{{event}}","msg":"{{msg}}","time":"{{time}}"})";
        }
        c.name = preset.empty() ? hostOf(c.url) : preset;
        out.push_back(std::move(c));
    }
    return out;
}

Notifier::Notifier(vector<SinkConfig> sinks, string appId) : appId_(std::move(appId)) {
    for (auto& s : sinks) sinks_.push_back(SinkState{std::move(s)});
    worker_ = thread(&Notifier::worker, this);
}

Notifier::~Notifier() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Notifier::notify(const string& event, const string& msg) {
    notify(event, msg, "", "");
}

void Notifier::notify(const string& event, const string& msg,
                      const string& app, const string& group) {
    Pending p{event, msg, getTimestampString("%Y-%m-%dT%H:%M:%S"), app};
    lock_guard<mutex> lock(mutex_);
    for (auto& s : sinks_) {
        if (s.cfg.heartbeat || !wants(s, event)) continue;
        if (!app.empty() && !sinkCovers(s.cfg, app, group)) continue;
        s.queue.push_back(p);
        if (s.queue.size() > 100) s.queue.pop_front();  // bounded; oldest goes
    }
    cv_.notify_all();
}

void Notifier::healthyTick() {
    lock_guard<mutex> lock(mutex_);
    lastHealthy_ = chrono::steady_clock::now();
}

void Notifier::healthyTick(const string& app) {
    lock_guard<mutex> lock(mutex_);
    for (auto& s : sinks_) {
        if (s.cfg.heartbeat && s.cfg.scope.size() == 1 &&
            s.cfg.scope[0] == "app:" + app) {
            s.lastHealthyScoped = chrono::steady_clock::now();
        }
    }
}

bool Notifier::wants(const SinkState& s, const string& event) const {
    if (s.cfg.events.empty()) return true;
    for (auto& e : s.cfg.events) {
        if (e == event) return true;
    }
    return false;
}

// One delivery attempt. true = done with this event (delivered, or 4xx =
// permanently rejected and dropped); false = retry later (network / 5xx).
bool Notifier::send(SinkState& s, const Pending& p) {
    string body = s.cfg.bodyTemplate;
    bool json = s.cfg.contentType.find("json") != string::npos;
    auto sub = [&](const char* key, const string& v) {
        replaceAll(body, key, json ? jsonEscape(v) : v);
    };
    sub("{{app}}", p.app.empty() ? appId_ : p.app);
    sub("{{event}}", p.event);
    sub("{{msg}}", p.msg);
    sub("{{time}}", p.time);

    tcx::curl::HttpClient cli;
    cli.setBaseUrl(s.cfg.url);
    cli.setTimeout(5);
    cli.setFollowRedirects(true);
    auto res = cli.request(s.cfg.method, "", body, s.cfg.contentType);

    if (res.ok()) {
        if (s.failing) {
            s.failing = false;
            logNotice("anchorbolt") << "sink '" << s.cfg.name << "' reachable again";
        }
        return true;
    }
    if (res.statusCode >= 400 && res.statusCode < 500) {
        // The service understood us and said no — retrying can't fix a bad
        // webhook URL or a malformed template. Drop THIS event, keep the sink.
        logError("anchorbolt") << "sink '" << s.cfg.name << "' rejected event ("
                               << res.statusCode << "); dropped: " << p.event;
        return true;
    }
    if (!s.failing) {
        s.failing = true;
        logWarning("anchorbolt") << "sink '" << s.cfg.name << "' unreachable ("
                                 << (res.statusCode ? to_string(res.statusCode) : res.error)
                                 << "); will retry";
    }
    return false;
}

// uptime-kuma push: GET <url>?status=up — the monitor alerts when these stop.
void Notifier::beat(SinkState& s) {
    tcx::curl::HttpClient cli;
    cli.setBaseUrl(s.cfg.url);
    cli.setTimeout(5);
    cli.setFollowRedirects(true);
    string sep = s.cfg.url.find('?') == string::npos ? "?" : "&";
    auto res = cli.request(s.cfg.method, sep + "status=up&msg=ok", "", s.cfg.contentType);
    if (res.ok() && s.failing) {
        s.failing = false;
        logNotice("anchorbolt") << "sink '" << s.cfg.name << "' reachable again";
    } else if (!res.ok() && !s.failing) {
        s.failing = true;
        logWarning("anchorbolt") << "sink '" << s.cfg.name << "' unreachable; will retry";
    }
}

void Notifier::worker() {
    while (!stop_) {
        {
            unique_lock<mutex> lock(mutex_);
            cv_.wait_for(lock, chrono::seconds(2));
        }
        if (stop_) break;
        auto now = chrono::steady_clock::now();
        for (size_t i = 0; i < sinks_.size(); ++i) {
            // Snapshot under the lock, send without it (HTTP can take seconds).
            while (!stop_) {
                Pending p;
                {
                    lock_guard<mutex> lock(mutex_);
                    auto& s = sinks_[i];
                    if (s.queue.empty()) break;
                    if (s.failing && now - s.lastAttempt < chrono::seconds(15)) break;
                    p = s.queue.front();
                }
                sinks_[i].lastAttempt = now;
                if (send(sinks_[i], p)) {
                    lock_guard<mutex> lock(mutex_);
                    auto& q = sinks_[i].queue;
                    if (!q.empty()) q.pop_front();
                } else {
                    break;  // sink down; keep the queue, retry next round
                }
            }
            // Heartbeat sinks: ping while the app is actually healthy.
            auto& s = sinks_[i];
            if (s.cfg.heartbeat && !stop_) {
                bool healthyNow;
                {
                    lock_guard<mutex> lock(mutex_);
                    // Venue path feeds the global tick; serve's per-app kuma
                    // sinks feed the scoped one. Either being fresh counts.
                    healthyNow = (lastHealthy_.time_since_epoch().count() != 0 &&
                                  now - lastHealthy_ < chrono::seconds(30)) ||
                                 (s.lastHealthyScoped.time_since_epoch().count() != 0 &&
                                  now - s.lastHealthyScoped < chrono::seconds(30));
                }
                if (healthyNow && now - s.lastBeat >= chrono::seconds(s.cfg.intervalSec)) {
                    s.lastBeat = now;
                    beat(s);
                }
            }
        }
    }
}

void Notifier::flushAndStop() {
    // Stop the worker FIRST (otherwise it could race this flush into a double
    // delivery), then drain synchronously — single-threaded, no locks needed.
    // A dead sink costs at most one 5s timeout here.
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (auto& s : sinks_) {
        while (!s.queue.empty()) {
            if (!send(s, s.queue.front())) break;
            s.queue.pop_front();
        }
    }
}
