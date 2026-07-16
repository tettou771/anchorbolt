#include "Token.h"
#include "Sha.h"

#include <TrussC.h>

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

fs::path agentsPath(const string& dataDir) {
    return fs::path(dataDir) / "tokens.json";
}

fs::path operatorsPath(const string& dataDir) {
    return fs::path(dataDir) / "operators.json";
}

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

int usage() {
    cerr <<
        "usage: anchorbolt token agent new <app-id>        [--data <dir>]\n"
        "       anchorbolt token agent revoke <app-id>     [--data <dir>]\n"
        "       anchorbolt token operator new <name> --role viewer|operator|admin\n"
        "       anchorbolt token operator revoke <name>    [--data <dir>]\n"
        "       anchorbolt token list                      [--data <dir>]" << endl;
    return 1;
}

} // namespace

namespace token {

bool enforcementEnabled(const string& dataDir) {
    Json t = loadRegistry(agentsPath(dataDir));
    return t.is_object() && !t.empty();
}

bool verify(const string& dataDir, const string& appId, const string& tok) {
    Json t = loadRegistry(agentsPath(dataDir));
    if (!t.contains(appId) || !t[appId].is_string()) return false;
    return t[appId].get<string>() == sha::sha256Hex(tok);
}

bool operatorsEnabled(const string& dataDir) {
    Json t = loadRegistry(operatorsPath(dataDir));
    return t.is_object() && !t.empty();
}

optional<Operator> verifyOperator(const string& dataDir, const string& tok) {
    if (tok.empty()) return nullopt;
    Json t = loadRegistry(operatorsPath(dataDir));
    string hash = sha::sha256Hex(tok);
    for (auto& [name, entry] : t.items()) {
        if (entry.is_object() && entry.value("hash", "") == hash) {
            return Operator{name, entry.value("role", "viewer")};
        }
    }
    return nullopt;
}

int roleRank(const string& role) {
    if (role == "admin") return 3;
    if (role == "operator") return 2;
    if (role == "viewer") return 1;
    return 0;
}

} // namespace token

int cmdToken(const vector<string>& args) {
    string dataDir = "anchorbolt-data";
    string role;
    vector<string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) dataDir = args[++i];
        else if (args[i] == "--role" && i + 1 < args.size()) role = args[++i];
        else rest.push_back(args[i]);
    }
    if (rest.empty()) return usage();

    // token list — both classes in one view.
    if (rest[0] == "list") {
        Json agents = loadRegistry(agentsPath(dataDir));
        Json ops = loadRegistry(operatorsPath(dataDir));
        if (agents.empty() && ops.empty()) {
            cout << "no tokens registered (full open mode: agents push and the\n"
                    "dashboard opens without authentication)" << endl;
            return 0;
        }
        if (!agents.empty()) {
            cout << "agents (publish-only):" << endl;
            for (auto& [app, hash] : agents.items()) {
                cout << "  " << app << "  sha256:" << hash.get<string>().substr(0, 16) << "..." << endl;
            }
        } else {
            cout << "agents: none (ingest is open)" << endl;
        }
        if (!ops.empty()) {
            cout << "operators (dashboard login):" << endl;
            for (auto& [name, e] : ops.items()) {
                cout << "  " << name << "  " << e.value("role", "?")
                     << "  since " << e.value("created", "?") << endl;
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
            Json tokens = loadRegistry(agentsPath(dataDir));
            bool replacing = tokens.contains(id);
            string tok = mintToken("tc-");
            tokens[id] = sha::sha256Hex(tok);
            if (!saveRegistry(dataDir, agentsPath(dataDir), tokens)) {
                cerr << "anchorbolt token: cannot write " << agentsPath(dataDir).string() << endl;
                return 1;
            }
            cout << (replacing ? "replaced agent token for '" : "new agent token for '")
                 << id << "':\n\n  " << tok << "\n\n"
                 << "shown once — only its hash is stored. On the venue machine:\n"
                 << "  anchorbolt start <app> --server <url> --id " << id << " --token " << tok << "\n"
                 << "NOTE: with at least one agent token registered, this server now\n"
                 << "REQUIRES a valid token from every agent (ingest open mode is off)." << endl;
            return 0;
        }
        if (verb == "revoke") {
            Json tokens = loadRegistry(agentsPath(dataDir));
            if (!tokens.contains(id)) {
                cerr << "no agent token registered for '" << id << "'" << endl;
                return 1;
            }
            tokens.erase(id);
            saveRegistry(dataDir, agentsPath(dataDir), tokens);
            cout << "revoked agent '" << id << "'"
                 << (tokens.empty() ? " — no agent tokens left, ingest is back to open mode" : "")
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
            Json ops = loadRegistry(operatorsPath(dataDir));
            bool replacing = ops.contains(id);
            string tok = mintToken("op-");
            ops[id] = Json{{"role", role},
                           {"hash", sha::sha256Hex(tok)},
                           {"created", getTimestampString("%Y-%m-%d")}};
            if (!saveRegistry(dataDir, operatorsPath(dataDir), ops)) {
                cerr << "anchorbolt token: cannot write " << operatorsPath(dataDir).string() << endl;
                return 1;
            }
            cout << (replacing ? "replaced operator '" : "new operator '")
                 << id << "' (" << role << "):\n\n  " << tok << "\n\n"
                 << "shown once — only its hash is stored. Paste it into the dashboard\n"
                 << "login. NOTE: with at least one operator registered, the dashboard\n"
                 << "now REQUIRES login (viewing open mode is off)." << endl;
            return 0;
        }
        if (verb == "revoke") {
            Json ops = loadRegistry(operatorsPath(dataDir));
            if (!ops.contains(id)) {
                cerr << "no operator named '" << id << "'" << endl;
                return 1;
            }
            ops.erase(id);
            saveRegistry(dataDir, operatorsPath(dataDir), ops);
            cout << "revoked operator '" << id << "'"
                 << (ops.empty() ? " — no operators left, dashboard is back to open mode" : "")
                 << endl;
            return 0;
        }
        return usage();
    }

    return usage();
}
