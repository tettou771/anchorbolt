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
        "  anchorbolt start <app-binary> [options] [-- app-args...]\n"
        "                                            kiosk mode: supervise an app on this machine\n"
        "  anchorbolt serve [options]                fleet server: dashboard + ingest + command channel\n"
        "  anchorbolt token <new|list|revoke> [app-id] [--data <dir>]\n"
        "                                            manage agent tokens (server side)\n"
        "\n"
        "START OPTIONS  (flags > ANCHORBOLT_TOKEN env > config file > defaults)\n"
        "  --config <file>    JSON config, comments allowed (auto-loads ./anchorbolt.json).\n"
        "                     Keys: app, args, id, cwd, server, port, wsPort, allowControl,\n"
        "                     grace, watchdogTimeout, log{dir,keepDays}, thumb{interval,width,\n"
        "                     quality}, tokenFile. A 'token' key is refused — keep secrets in\n"
        "                     tokenFile or the env, not in a config that lands in git.\n"
        "  --cwd <dir>        working directory for the app (default: the binary's directory)\n"
        "  --port <n>         MCP port injected as TRUSSC_MCP_PORT (default 47777)\n"
        "  --watchdog-timeout <sec>  unresponsive this long -> restart (default 10;\n"
        "                     0 = supervise process exit only, for apps without MCP)\n"
        "  --grace <sec>      boot grace period before the watchdog arms (default 15)\n"
        "  --log-dir <dir>    log destination (default: ~/Library/Logs/anchorbolt/<id> on\n"
        "                     macOS, $XDG_STATE_HOME/anchorbolt/<id> on Linux)\n"
        "  --log-keep <days>  delete our log files older than this (default 30; 0 = keep all)\n"
        "  --server <url>     fleet server to push to (e.g. http://192.168.1.10:8787)\n"
        "  --id <name>        app id on the fleet server (default: binary name)\n"
        "  --token <tok>      agent token minted by the server\n"
        "  --ws-port <n>      server command-channel port (default: server port + 1)\n"
        "  --allow-control    let the server relay MUTATING tools to the app\n"
        "                     (input injection, node writes, custom tools; default: read-only)\n"
        "  --thumb-interval <sec>  screenshot push interval (default 30; 0 = never)\n"
        "  --thumb-width <px>      screenshot width (default 512)\n"
        "  --thumb-quality <1-100> JPEG quality (default 75)\n"
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
