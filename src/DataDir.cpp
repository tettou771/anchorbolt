#include "DataDir.h"

#include <TrussC.h>

#include <fstream>

#if defined(_WIN32)
#include <windows.h>    // GetCurrentProcessId
#else
#include <unistd.h>     // getpid
#endif

using namespace std;
using namespace tc;
namespace fs = std::filesystem;

namespace datadir {

fs::path stateBase() {
#if defined(_WIN32)
    const char* lad = getenv("LOCALAPPDATA");
    return fs::path(lad ? lad : ".") / "anchorbolt";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Logs" / "anchorbolt";
#else
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_STATE_HOME");
    fs::path base = xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "state";
    return base / "anchorbolt";
#endif
}

fs::path servePointerPath() { return stateBase() / "serve.json"; }

void writeServePointer(const fs::path& dataDir, int port) {
    error_code ec;
    fs::create_directories(stateBase(), ec);
    ofstream out(servePointerPath());
    if (!out) return;
    out << Json{{"dataDir", dataDir.string()}, {"port", port},
                {"pid", (int64_t)
#if defined(_WIN32)
                 GetCurrentProcessId()
#else
                 getpid()
#endif
                }}.dump(2) << "\n";
}

string resolveDataDir(const string& explicitDir, string* err) {
    if (!explicitDir.empty()) return explicitDir;
    if (fs::exists("anchorbolt-data")) return "anchorbolt-data";
    ifstream in(servePointerPath());
    if (in) {
        try {
            Json p = Json::parse(in);
            string d = p.value("dataDir", "");
            if (!d.empty() && fs::exists(d)) return d;
        } catch (...) {}
    }
    if (err) {
        *err = "no data directory found: no ./anchorbolt-data here and no running serve\n"
               "pointer (" + servePointerPath().string() + ").\n"
               "Pass --data <dir> (a serve started with it writes the pointer for next time).";
    }
    return "";
}

} // namespace datadir
