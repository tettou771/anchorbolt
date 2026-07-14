// =============================================================================
// anchorbolt — TrussC installation ops tool
//   start: kiosk mode — venue-side supervisor (spawn app, watchdog, thumbnails, sinks)
//   serve: fleet server (dashboard, storage, WS hub)
// One binary, both ends of the protocol. Uses TrussC as a library (no window).
// =============================================================================

#include <TrussC.h>
#include "Start.h"
#include "Serve.h"
#include "Token.h"

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
        "  anchorbolt serve [options]                fleet server: dashboard + ingest + command channel\n"
        "  anchorbolt token <new|list|revoke> [app-id] [--data <dir>]\n"
        "                                            manage agent tokens (server side)\n"
        "\n"
        "START OPTIONS\n"
        "  --cwd <dir>        working directory for the app (default: the binary's directory)\n"
        "  --port <n>         MCP port injected as TRUSSC_MCP_PORT (default 47777)\n"
        "  --interval <sec>   get_health poll interval (default 3)\n"
        "  --grace <sec>      boot grace period before health misses count (default 15)\n"
        "  --misses <n>       consecutive failed polls that trigger a restart (default 3)\n"
        "  --log-dir <dir>    app log destination, injected as TRUSSC_LOG_FILE\n"
        "                     (default ./anchorbolt-logs)\n"
        "  --server <url>     fleet server to push to (e.g. http://192.168.1.10:8787)\n"
        "  --id <name>        app id on the fleet server (default: binary name)\n"
        "  --token <tok>      agent token minted by the server (or ANCHORBOLT_TOKEN)\n"
        "  --ws-port <n>      server command-channel port (default: server port + 1)\n"
        "  --allow-control    let the server relay MUTATING tools to the app\n"
        "                     (input injection, node writes, custom tools; default: read-only)\n"
        "  --thumb-interval <sec>  thumbnail push interval (default 30)\n"
        "\n"
        "SERVE OPTIONS\n"
        "  -p, --port <n>     HTTP port (default 8787)\n"
        "  --ws-port <n>      agent command-channel port (default: port + 1)\n"
        "  --data <dir>       storage directory for heartbeats/thumbnails\n"
        "                     (default ./anchorbolt-data)\n";
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
        return cmdServe(vector<string>(args.begin() + 1, args.end()));
    }
    if (cmd == "token") {
        return cmdToken(vector<string>(args.begin() + 1, args.end()));
    }

    cerr << "anchorbolt: unknown command '" << cmd << "'\n" << endl;
    printHelp();
    return 1;
}
