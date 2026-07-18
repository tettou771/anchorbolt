#include "Token.h"
#include "Sha.h"

#include <TrussC.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

namespace {

Json loadRegistry(const fs::path& path) {
    ifstream in(path);
    if (!in) return Json::object();
    try {
        return Json::parse(in);
    } catch (...) {
        return Json::object();
    }
}

bool saveRegistry(const string& dataDir, const fs::path& path, const Json& j) {
    error_code ec;
    fs::create_directories(dataDir, ec);
    ofstream out(path);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return true;
}

fs::path agentsPath(const string& dataDir)   { return fs::path(dataDir) / "tokens.json"; }
fs::path operatorsPath(const string& dataDir){ return fs::path(dataDir) / "operators.json"; }
fs::path groupsPath(const string& dataDir)   { return fs::path(dataDir) / "groups.json"; }
fs::path sessionsPath(const string& dataDir) { return fs::path(dataDir) / "sessions.json"; }
fs::path codesPath(const string& dataDir)    { return fs::path(dataDir) / "codes.json"; }
fs::path sharesPath(const string& dataDir)   { return fs::path(dataDir) / "shares.json"; }

// 32 random bytes from the OS CSPRNG, as "<prefix><hex>".
string mintToken(const char* prefix) {
    random_device rd;  // /dev/urandom-backed on POSIX
    string tok = prefix;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        uint32_t v = rd();
        for (int j = 28; j >= 0; j -= 4) tok.push_back(hex[(v >> j) & 0xF]);
    }
    return tok;
}

bool validRole(const string& r) {
    return r == "viewer" || r == "operator" || r == "admin";
}

// Read a "scope" array out of a stored entry into a vector<string>.
vector<string> scopeVec(const Json& entry) {
    vector<string> out;
    if (entry.is_object() && entry.contains("scope") && entry["scope"].is_array()) {
        for (auto& s : entry["scope"]) if (s.is_string()) out.push_back(s.get<string>());
    }
    return out;
}

int usage() {
    cerr <<
        "usage: anchorbolt token agent new <app-id>        [--data <dir>]\n"
        "       anchorbolt token agent revoke <app-id>     [--data <dir>]\n"
        "       anchorbolt token operator new <name> --role viewer|operator|admin\n"
        "                          [--scope <g1,app:id,...>] [--data <dir>]\n"
        "       anchorbolt token operator revoke <name>    [--data <dir>]\n"
        "       anchorbolt token list                      [--data <dir>]" << endl;
    return 1;
}

// A scope word that means "unrestricted": "all" (any case) or "*".
bool isGlobalScopeWord(const string& s) {
    if (s == "*") return true;
    if (s.size() != 3) return false;
    return (s[0] == 'a' || s[0] == 'A') && (s[1] == 'l' || s[1] == 'L') &&
           (s[2] == 'l' || s[2] == 'L');
}

// Split a comma-separated --scope value into trimmed, non-empty entries.
vector<string> parseScopeArg(const string& csv) {
    vector<string> out;
    string cur;
    for (char c : csv) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else if (c != ' ') cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    // "all" (any case) or "*" means unrestricted — the same as no --scope. Treat
    // it as a global scope (empty) instead of a literal group named "all", which
    // would match no app and hide everything.
    for (const auto& s : out) if (isGlobalScopeWord(s)) return {};
    return out;
}

} // namespace

namespace token {

// --- agent tokens ---

bool verify(const string& dataDir, const string& appId, const string& tok) {
    Json t = loadRegistry(agentsPath(dataDir));
    if (!t.contains(appId) || !t[appId].is_string()) return false;
    return t[appId].get<string>() == sha::sha256Hex(tok);
}

optional<string> resolveAgent(const string& dataDir, const string& tok) {
    if (tok.empty()) return nullopt;
    Json t = loadRegistry(agentsPath(dataDir));
    string hash = sha::sha256Hex(tok);
    for (auto& [app, h] : t.items())
        if (h.is_string() && h.get<string>() == hash) return app;
    return nullopt;
}

string mintAgent(const string& dataDir, const string& appId) {
    Json tokens = loadRegistry(agentsPath(dataDir));
    string tok = mintToken("tc-");
    tokens[appId] = sha::sha256Hex(tok);
    if (!saveRegistry(dataDir, agentsPath(dataDir), tokens)) return "";
    return tok;
}

bool revokeAgent(const string& dataDir, const string& appId) {
    Json tokens = loadRegistry(agentsPath(dataDir));
    if (!tokens.contains(appId)) return false;
    tokens.erase(appId);
    saveRegistry(dataDir, agentsPath(dataDir), tokens);
    return true;
}

Json listAgents(const string& dataDir) {
    Json t = loadRegistry(agentsPath(dataDir));
    Json out = Json::array();
    for (auto& [app, hash] : t.items()) {
        string h = hash.is_string() ? hash.get<string>() : "";
        out.push_back({{"id", app}, {"hashPrefix", h.substr(0, 12)}});
    }
    return out;
}

// --- operator tokens ---

bool operatorsEnabled(const string& dataDir) {
    Json t = loadRegistry(operatorsPath(dataDir));
    return t.is_object() && !t.empty();
}

optional<Operator> verifyOperator(const string& dataDir, const string& tok) {
    if (tok.empty()) return nullopt;
    Json ops = loadRegistry(operatorsPath(dataDir));
    string hash = sha::sha256Hex(tok);
    // Direct operator token.
    for (auto& [name, entry] : ops.items()) {
        if (entry.is_object() && entry.value("hash", "") == hash) {
            return Operator{name, entry.value("role", "viewer"), scopeVec(entry)};
        }
    }
    // Login-code session token: resolve the session hash back to a live
    // operator. A revoked operator leaves the session orphaned — treat it as
    // dead and prune it lazily so revocation stays instant here too.
    Json sessions = loadRegistry(sessionsPath(dataDir));
    if (sessions.contains(hash) && sessions[hash].is_object()) {
        string name = sessions[hash].value("name", "");
        if (ops.contains(name) && ops[name].is_object()) {
            return Operator{name, ops[name].value("role", "viewer"), scopeVec(ops[name])};
        }
        sessions.erase(hash);
        saveRegistry(dataDir, sessionsPath(dataDir), sessions);
    }
    // Share link: a viewer-scoped token with an expiry, delivered as a URL. Not
    // a named human, so it lives in its own registry; expired ones are pruned.
    Json shares = loadRegistry(sharesPath(dataDir));
    if (shares.contains(hash) && shares[hash].is_object()) {
        int64_t exp = shares[hash].value("expires", (int64_t)0);
        if (exp == 0 || (int64_t)time(nullptr) < exp)
            return Operator{"share", "viewer", scopeVec(shares[hash])};
        shares.erase(hash);
        saveRegistry(dataDir, sharesPath(dataDir), shares);
    }
    return nullopt;
}

// Mint a share link: a viewer-scoped token with an optional expiry (ttlSec=0 =
// never expires). Returns the plaintext token (embedded in a URL, shown once).
string mintShare(const string& dataDir, const vector<string>& scope, int ttlSec) {
    Json shares = loadRegistry(sharesPath(dataDir));
    int64_t now = (int64_t)time(nullptr);
    string tok = mintToken("sh-");
    Json e = {{"expires", ttlSec > 0 ? now + ttlSec : 0},
              {"created", getTimestampString("%Y-%m-%d")}};
    if (!scope.empty()) e["scope"] = scope;
    shares[sha::sha256Hex(tok)] = e;
    if (!saveRegistry(dataDir, sharesPath(dataDir), shares)) return "";
    return tok;
}

// Active shares for the settings UI (never the token — only a hash prefix to
// revoke by). Prunes expired ones as a side effect.
Json listShares(const string& dataDir) {
    Json shares = loadRegistry(sharesPath(dataDir));
    int64_t now = (int64_t)time(nullptr);
    Json out = Json::array();
    bool changed = false;
    for (auto it = shares.begin(); it != shares.end();) {
        int64_t exp = it->value("expires", (int64_t)0);
        if (exp != 0 && now >= exp) { it = shares.erase(it); changed = true; continue; }
        out.push_back({{"id", it.key().substr(0, 12)},
                       {"scope", scopeVec(*it)},
                       {"expires", exp},
                       {"created", it->value("created", "")}});
        ++it;
    }
    if (changed) saveRegistry(dataDir, sharesPath(dataDir), shares);
    return out;
}

bool revokeShare(const string& dataDir, const string& hashPrefix) {
    Json shares = loadRegistry(sharesPath(dataDir));
    for (auto it = shares.begin(); it != shares.end(); ++it) {
        if (it.key().rfind(hashPrefix, 0) == 0) {
            shares.erase(it);
            saveRegistry(dataDir, sharesPath(dataDir), shares);
            return true;
        }
    }
    return false;
}

int roleRank(const string& role) {
    if (role == "admin") return 3;
    if (role == "operator") return 2;
    if (role == "viewer") return 1;
    return 0;
}

bool inScope(const Operator& op, const string& appId, const string& group) {
    if (op.scope.empty()) return true;  // unscoped operator sees everything
    for (const auto& s : op.scope) {
        if (isGlobalScopeWord(s)) return true;  // "all"/"*" stored on an old token
        if (!group.empty() && s == group) return true;
        if (s == "app:" + appId) return true;
    }
    return false;
}

string mintOperator(const string& dataDir, const string& name,
                    const string& role, const vector<string>& scope) {
    if (!validRole(role)) return "";
    Json ops = loadRegistry(operatorsPath(dataDir));
    string tok = mintToken("op-");
    Json entry = {{"role", role},
                  {"hash", sha::sha256Hex(tok)},
                  {"created", getTimestampString("%Y-%m-%d")}};
    if (!scope.empty()) entry["scope"] = scope;
    // Preserve an existing scope when replacing without an explicit new one.
    else if (ops.contains(name) && ops[name].is_object() && ops[name].contains("scope"))
        entry["scope"] = ops[name]["scope"];
    ops[name] = entry;
    if (!saveRegistry(dataDir, operatorsPath(dataDir), ops)) return "";
    return tok;
}

bool revokeOperator(const string& dataDir, const string& name) {
    Json ops = loadRegistry(operatorsPath(dataDir));
    if (!ops.contains(name)) return false;
    ops.erase(name);
    saveRegistry(dataDir, operatorsPath(dataDir), ops);
    return true;
}

// Re-mint an existing operator's token, keeping its role + scope unchanged (so a
// recovery can't accidentally change the role). Empty if the operator is unknown.
string renewOperator(const string& dataDir, const string& name) {
    Json ops = loadRegistry(operatorsPath(dataDir));
    if (!ops.contains(name) || !ops[name].is_object()) return "";
    return mintOperator(dataDir, name, ops[name].value("role", "viewer"), scopeVec(ops[name]));
}

bool setOperatorScope(const string& dataDir, const string& name,
                      const vector<string>& scope) {
    Json ops = loadRegistry(operatorsPath(dataDir));
    if (!ops.contains(name) || !ops[name].is_object()) return false;
    if (scope.empty()) ops[name].erase("scope");
    else ops[name]["scope"] = scope;
    return saveRegistry(dataDir, operatorsPath(dataDir), ops);
}

Json listOperators(const string& dataDir) {
    Json ops = loadRegistry(operatorsPath(dataDir));
    Json out = Json::array();
    for (auto& [name, e] : ops.items()) {
        out.push_back({{"name", name},
                       {"role", e.value("role", "viewer")},
                       {"created", e.value("created", "")},
                       {"scope", scopeVec(e)}});
    }
    return out;
}

// --- app groups ---

Json loadGroups(const string& dataDir) {
    Json g = loadRegistry(groupsPath(dataDir));
    return g.is_object() ? g : Json::object();
}

string groupOf(const string& dataDir, const string& appId) {
    Json g = loadGroups(dataDir);
    if (g.contains(appId) && g[appId].is_string()) return g[appId].get<string>();
    return "";
}

bool setGroup(const string& dataDir, const string& appId, const string& group) {
    Json g = loadGroups(dataDir);
    if (group.empty()) g.erase(appId);
    else g[appId] = group;
    return saveRegistry(dataDir, groupsPath(dataDir), g);
}

// --- sessions ---

string mintSession(const string& dataDir, const string& name) {
    Json sessions = loadRegistry(sessionsPath(dataDir));
    string tok = mintToken("os-");
    sessions[sha::sha256Hex(tok)] = {{"name", name},
                                     {"created", getTimestampString("%Y-%m-%dT%H:%M:%S")}};
    if (!saveRegistry(dataDir, sessionsPath(dataDir), sessions)) return "";
    return tok;
}

// --- 6-digit codes ---

// Drop expired entries in-place; returns whether anything changed.
static bool pruneCodes(Json& codes, int64_t now) {
    bool changed = false;
    for (auto it = codes.begin(); it != codes.end();) {
        if (!it->is_object() || it->value("expires", (int64_t)0) < now) {
            it = codes.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

string mintCode(const string& dataDir, const string& kind,
                const string& subject, int ttlSec) {
    Json codes = loadRegistry(codesPath(dataDir));
    int64_t now = (int64_t)time(nullptr);
    pruneCodes(codes, now);
    random_device rd;
    string code;
    do {
        char buf[8];
        snprintf(buf, sizeof(buf), "%06u", (unsigned)(rd() % 1000000u));
        code = buf;
    } while (codes.contains(code));
    codes[code] = {{"kind", kind}, {"subject", subject}, {"expires", now + ttlSec}};
    saveRegistry(dataDir, codesPath(dataDir), codes);
    return code;
}

optional<pair<string, string>> redeemCode(const string& dataDir, const string& code) {
    Json codes = loadRegistry(codesPath(dataDir));
    int64_t now = (int64_t)time(nullptr);
    optional<pair<string, string>> result;
    bool changed = pruneCodes(codes, now);
    auto it = codes.find(code);
    if (it != codes.end()) {
        // Single-use: consume regardless of validity so a leaked code dies.
        if (it->is_object() && it->value("expires", (int64_t)0) >= now) {
            result = make_pair(it->value("kind", ""), it->value("subject", ""));
        }
        codes.erase(it);
        changed = true;
    }
    if (changed) saveRegistry(dataDir, codesPath(dataDir), codes);
    return result;
}

} // namespace token

int cmdToken(const vector<string>& args) {
    string dataDir = "anchorbolt-data";
    string role;
    string scopeArg;
    vector<string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) dataDir = args[++i];
        else if (args[i] == "--role" && i + 1 < args.size()) role = args[++i];
        else if (args[i] == "--scope" && i + 1 < args.size()) scopeArg = args[++i];
        else rest.push_back(args[i]);
    }
    if (rest.empty()) return usage();

    // token renew <operator-name> — re-mint keeping role + scope.
    if (rest[0] == "renew") {
        if (rest.size() < 2) {
            cerr << "usage: anchorbolt token renew <operator-name>" << endl;
            return 1;
        }
        string tok = token::renewOperator(dataDir, rest[1]);
        if (tok.empty()) {
            cerr << "no operator named '" << rest[1] << "'. Create one with:\n"
                 << "  anchorbolt token operator new " << rest[1]
                 << " --role viewer|operator|admin" << endl;
            return 1;
        }
        cout << "renewed operator '" << rest[1] << "' (role + scope unchanged):\n\n  "
             << tok << "\n\nshown once — paste it into the dashboard login." << endl;
        return 0;
    }

    // token list — both classes in one view.
    if (rest[0] == "list") {
        Json agents = token::listAgents(dataDir);
        Json ops = token::listOperators(dataDir);
        if (agents.empty() && ops.empty()) {
            cout << "no tokens registered (full open mode: agents push and the\n"
                    "dashboard opens without authentication)" << endl;
            return 0;
        }
        if (!agents.empty()) {
            cout << "agents (publish-only):" << endl;
            for (auto& a : agents) {
                cout << "  " << a.value("id", "?")
                     << "  sha256:" << a.value("hashPrefix", "") << "..." << endl;
            }
        } else {
            cout << "agents: none (ingest is open)" << endl;
        }
        if (!ops.empty()) {
            cout << "operators (dashboard login):" << endl;
            for (auto& e : ops) {
                cout << "  " << e.value("name", "?") << "  " << e.value("role", "?");
                auto sc = e.value("scope", Json::array());
                if (!sc.empty()) {
                    cout << "  scope=";
                    for (size_t i = 0; i < sc.size(); ++i)
                        cout << (i ? "," : "") << sc[i].get<string>();
                } else {
                    cout << "  scope=all";
                }
                cout << "  since " << e.value("created", "?") << endl;
            }
        } else {
            cout << "operators: none (dashboard is open)" << endl;
        }
        return 0;
    }

    if (rest.size() < 3) return usage();
    const string& type = rest[0];
    const string& verb = rest[1];
    const string& id = rest[2];

    if (type == "agent") {
        if (verb == "new") {
            Json before = token::listAgents(dataDir);
            bool replacing = false;
            for (auto& a : before) if (a.value("id", "") == id) replacing = true;
            string tok = token::mintAgent(dataDir, id);
            if (tok.empty()) {
                cerr << "anchorbolt token: cannot write agent registry" << endl;
                return 1;
            }
            cout << (replacing ? "replaced agent token for '" : "new agent token for '")
                 << id << "':\n\n  " << tok << "\n\n"
                 << "shown once — only its hash is stored. On the venue machine:\n"
                 << "  anchorbolt start <app> --server <url> --token " << tok << "\n"
                 << "the id ('" << id << "') is derived from the token — no --id needed." << endl;
            return 0;
        }
        if (verb == "revoke") {
            if (!token::revokeAgent(dataDir, id)) {
                cerr << "no agent token registered for '" << id << "'" << endl;
                return 1;
            }
            cout << "revoked agent '" << id << "'"
                 << (token::listAgents(dataDir).empty()
                        ? " — no agent tokens left, ingest is back to open mode" : "")
                 << endl;
            return 0;
        }
        return usage();
    }

    if (type == "operator") {
        if (verb == "new") {
            if (!validRole(role)) {
                cerr << "anchorbolt token operator new: --role viewer|operator|admin is required" << endl;
                return 1;
            }
            vector<string> scope = parseScopeArg(scopeArg);
            Json before = token::listOperators(dataDir);
            bool replacing = false;
            for (auto& e : before) if (e.value("name", "") == id) replacing = true;
            string tok = token::mintOperator(dataDir, id, role, scope);
            if (tok.empty()) {
                cerr << "anchorbolt token: cannot write operator registry" << endl;
                return 1;
            }
            cout << (replacing ? "replaced operator '" : "new operator '")
                 << id << "' (" << role;
            if (!scope.empty()) {
                cout << ", scope ";
                for (size_t i = 0; i < scope.size(); ++i) cout << (i ? "," : "") << scope[i];
            }
            cout << "):\n\n  " << tok << "\n\n"
                 << "shown once — only its hash is stored. Paste it into the dashboard\n"
                 << "login. NOTE: with at least one operator registered, the dashboard\n"
                 << "now REQUIRES login (viewing open mode is off)." << endl;
            return 0;
        }
        if (verb == "revoke") {
            if (!token::revokeOperator(dataDir, id)) {
                cerr << "no operator named '" << id << "'" << endl;
                return 1;
            }
            cout << "revoked operator '" << id << "'"
                 << (token::listOperators(dataDir).empty()
                        ? " — no operators left, dashboard is back to open mode" : "")
                 << endl;
            return 0;
        }
        return usage();
    }

    return usage();
}
