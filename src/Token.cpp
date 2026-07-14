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

fs::path tokensPath(const string& dataDir) {
    return fs::path(dataDir) / "tokens.json";
}

Json loadTokens(const string& dataDir) {
    ifstream in(tokensPath(dataDir));
    if (!in) return Json::object();
    try {
        return Json::parse(in);
    } catch (...) {
        return Json::object();
    }
}

bool saveTokens(const string& dataDir, const Json& tokens) {
    error_code ec;
    fs::create_directories(dataDir, ec);
    ofstream out(tokensPath(dataDir));
    if (!out) return false;
    out << tokens.dump(2) << "\n";
    return true;
}

// 32 random bytes from the OS CSPRNG, as "tc-<hex>".
string mintToken() {
    random_device rd;  // /dev/urandom-backed on POSIX
    string tok = "tc-";
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        uint32_t v = rd();
        for (int j = 28; j >= 0; j -= 4) tok.push_back(hex[(v >> j) & 0xF]);
    }
    return tok;
}

} // namespace

namespace token {

bool enforcementEnabled(const string& dataDir) {
    Json t = loadTokens(dataDir);
    return t.is_object() && !t.empty();
}

bool verify(const string& dataDir, const string& appId, const string& tok) {
    Json t = loadTokens(dataDir);
    if (!t.contains(appId) || !t[appId].is_string()) return false;
    return t[appId].get<string>() == sha::sha256Hex(tok);
}

} // namespace token

int cmdToken(const vector<string>& args) {
    string dataDir = "anchorbolt-data";
    vector<string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) dataDir = args[++i];
        else rest.push_back(args[i]);
    }

    if (rest.empty()) {
        cerr << "usage: anchorbolt token <new|list|revoke> [app-id] [--data <dir>]" << endl;
        return 1;
    }
    const string& verb = rest[0];

    if (verb == "new") {
        if (rest.size() < 2) {
            cerr << "anchorbolt token new: an app-id is required" << endl;
            return 1;
        }
        const string& appId = rest[1];
        Json tokens = loadTokens(dataDir);
        bool replacing = tokens.contains(appId);
        string tok = mintToken();
        tokens[appId] = sha::sha256Hex(tok);
        if (!saveTokens(dataDir, tokens)) {
            cerr << "anchorbolt token new: cannot write " << tokensPath(dataDir).string() << endl;
            return 1;
        }
        cout << (replacing ? "replaced token for '" : "new token for '") << appId << "':\n\n"
             << "  " << tok << "\n\n"
             << "shown once — only its hash is stored. On the venue machine:\n"
             << "  anchorbolt start <app> --server <url> --id " << appId << " --token " << tok << "\n"
             << "NOTE: with at least one token registered, this server now REQUIRES\n"
             << "a valid token from every agent (open mode is off)." << endl;
        return 0;
    }
    if (verb == "list") {
        Json tokens = loadTokens(dataDir);
        if (tokens.empty()) {
            cout << "no tokens registered (open mode: agents connect without authentication)" << endl;
            return 0;
        }
        for (auto& [app, hash] : tokens.items()) {
            cout << app << "  sha256:" << hash.get<string>().substr(0, 16) << "..." << endl;
        }
        return 0;
    }
    if (verb == "revoke") {
        if (rest.size() < 2) {
            cerr << "anchorbolt token revoke: an app-id is required" << endl;
            return 1;
        }
        Json tokens = loadTokens(dataDir);
        if (!tokens.contains(rest[1])) {
            cerr << "no token registered for '" << rest[1] << "'" << endl;
            return 1;
        }
        tokens.erase(rest[1]);
        saveTokens(dataDir, tokens);
        cout << "revoked '" << rest[1] << "'"
             << (tokens.empty() ? " — no tokens left, server is back to open mode" : "") << endl;
        return 0;
    }

    cerr << "anchorbolt token: unknown verb '" << verb << "'" << endl;
    return 1;
}
