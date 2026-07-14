// =============================================================================
// anchorbolt — TrussC installation ops tool
//   start: kiosk mode — venue-side supervisor (spawn app, watchdog, thumbnails, sinks)
//   serve: fleet server (dashboard, storage, WS hub)
// One binary, both ends of the protocol. Uses TrussC as a library (no window).
// =============================================================================

#include <TrussC.h>
#include "Start.h"

#include <iostream>
#include <string>
#include <vector>

using namespace std;

static void printHelp() {
    cout <<
        "anchorbolt - TrussC installation ops tool\n"
        "\n"
        "USAGE\n"
        "  anchorbolt start <app-binary> [options]   kiosk mode: supervise an app on this machine\n"
        "  anchorbolt serve                          fleet server (not implemented yet)\n"
        "  anchorbolt reset-admin                    recover the server admin password (not implemented yet)\n"
        "\n"
        "START OPTIONS\n"
        "  --cwd <dir>        working directory for the app (default: the binary's directory)\n"
        "  --port <n>         MCP port injected as TRUSSC_MCP_PORT (default 47777)\n"
        "  --interval <sec>   get_health poll interval (default 3)\n"
        "  --grace <sec>      boot grace period before health misses count (default 15)\n"
        "  --misses <n>       consecutive failed polls that trigger a restart (default 3)\n"
        "  --log-dir <dir>    app log destination, injected as TRUSSC_LOG_FILE\n"
        "                     (default ./anchorbolt-logs)\n";
}

int main(int argc, char* argv[]) {
    vector<string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printHelp();
        return 1;
    }
    const string& cmd = args[0];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        printHelp();
        return 0;
    }
    if (cmd == "-v" || cmd == "--version" || cmd == "version") {
        cout << "anchorbolt 0.0.1 (TrussC " << tc::getVersion() << ")" << endl;
        return 0;
    }
    if (cmd == "start") {
        return cmdStart(vector<string>(args.begin() + 1, args.end()));
    }
    if (cmd == "serve") {
        cerr << "anchorbolt serve: not implemented yet (Phase 0 in progress)" << endl;
        return 1;
    }
    if (cmd == "reset-admin") {
        cerr << "anchorbolt reset-admin: not implemented yet" << endl;
        return 1;
    }

    cerr << "anchorbolt: unknown command '" << cmd << "'\n" << endl;
    printHelp();
    return 1;
}
