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
#include "Version.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

// Short help by default; the full reference is behind --help --verbose so the
// everyday flags aren't buried under watchdog/thumbnail/sink/update tuning.
static void printHelp(bool verbose) {
    if (!verbose) {
        cout <<
            "AnchorBolt - TrussC installation ops tool (command: anchorbolt)\n"
            "\n"
            "USAGE\n"
            "  anchorbolt start [-p <path>] [options]   supervise an app on this machine (venue)\n"
            "  anchorbolt serve [options]               fleet server: dashboard + ingest + control\n"
            "  anchorbolt token agent|operator ...      mint/revoke tokens (server side)\n"
            "\n"
            "COMMON START OPTIONS\n"
            "  -p <path>          app to supervise: project dir, .app bundle, or binary (default: cwd)\n"
            "  --server <url>     fleet server to join. Needs a token: the first run prompts for a\n"
            "                     6-digit pairing code (or pass --pair <code> / --token <tc-...>).\n"
            "                     The id is server-assigned — there is no --id.\n"
            "  --config <file>    JSON config, comments allowed (auto-loads ./anchorbolt.json)\n"
            "  --generate-config  print a commented config template to stdout and exit\n"
            "\n"
            "COMMON SERVE OPTIONS\n"
            "  --data <dir>       storage directory (default ./anchorbolt-data)\n"
            "  --port <n>         HTTP port (default 54722)\n"
            "\n"
            "  anchorbolt approvals [list|approve [id]|deny [id]]   decide queued AI calls\n"
            "\n"
            "Run 'anchorbolt --help --verbose' for every option (watchdog, thumbnails,\n"
            "remote update, command-channel routing over a tunnel, webhook sinks, ...).\n";
        return;
    }
    cout <<
        "AnchorBolt - TrussC installation ops tool (command: anchorbolt)\n"
        "\n"
        "USAGE\n"
        "  anchorbolt start [-p <path>] [options] [-- app-args...]\n"
        "                                            kiosk mode: supervise an app on this machine\n"
        "  anchorbolt serve [options]                fleet server: dashboard + ingest + command channel\n"
        "  anchorbolt token agent new|revoke <app-id>      [--data <dir>]\n"
        "  anchorbolt token operator new <name> --role viewer|operator|admin [--scope a,app:b]\n"
        "  anchorbolt token operator revoke <name>\n"
        "  anchorbolt token renew <name>             re-mint an operator's token, keeping its\n"
        "                                            role + scope (recovery / rotation)\n"
        "  anchorbolt token list                     mint/revoke tokens (server side)\n"
        "\n"
        "START OPTIONS  (flags > ANCHORBOLT_TOKEN env > config file > defaults)\n"
        "  -p <path>          app to supervise: project dir, .app bundle, or binary (default: cwd)\n"
        "  --generate-config  print a commented config template to stdout and exit\n"
        "  --config <file>    JSON config, comments allowed (auto-loads ./anchorbolt.json).\n"
        "                     Keys: app, args, cwd, server, port, wsPort, denyUpdate,\n"
        "                     update, wsUrl, grace, watchdogTimeout,\n"
        "                     log{dir,keepDays}, thumb{interval,width,quality}, tokenFile,\n"
        "                     sinks (webhook notifications, see below). A 'token' key is\n"
        "                     refused — keep secrets in tokenFile or the env, not in a\n"
        "                     config that lands in git.\n"
        "                     sinks: [{preset: slack|discord|ntfy|uptime-kuma, url|urlEnv|\n"
        "                     urlFile, events?: [restart,up,down,update,stop,alert], body?,\n"
        "                     method?, contentType?, interval?}] — ONE templated HTTP\n"
        "                     engine ({{app}} {{event}} {{msg}} {{time}}); presets prefill\n"
        "                     it. uptime-kuma = heartbeat mode (pings while healthy).\n"
        "  --cwd <dir>        working directory for the app (default: the binary's directory)\n"
        "  --port <n>         MCP port injected as TRUSSC_MCP_PORT (default 47777)\n"
        "  --watchdog-timeout <sec>  unresponsive this long -> restart (default 10;\n"
        "                     0 = supervise process exit only, for apps without MCP)\n"
        "  --grace <sec>      boot grace before the hang-watchdog arms (default 120; only\n"
        "                     the first health reply waits — a crash/exit still restarts now)\n"
        "  --log-dir <dir>    log destination (default: ~/Library/Logs/anchorbolt/<id> on\n"
        "                     macOS, $XDG_STATE_HOME/anchorbolt/<id> on Linux)\n"
        "  --log-keep <days>  delete our log files older than this (default 30; 0 = keep all)\n"
        "  --server <url>     fleet server to push to (e.g. http://192.168.1.10:54722).\n"
        "                     Joining a fleet needs a token (--pair or --token); the id is\n"
        "                     assigned by the server, not set here.\n"
        "  --token <tok>      agent token minted by the server\n"
        "  --token-file <path>  read the token from a file (keeps it out of ps/plists)\n"
        "  --pair <code>      redeem a 6-digit pairing code at --server for this app's\n"
        "                     id + token, then persist them (0600) to anchorbolt.token.json\n"
        "                     in the platform state dir — keyed by binary name, never in a\n"
        "                     repo — so later plain runs reuse them\n"
        "  --ws-port <n>      server command-channel port (default: server port + 1)\n"
        "  --ws-url <url>     explicit ws(s):// for the command channel, bypassing the\n"
        "                     host:port+1 guess — needed behind a reverse proxy / tunnel\n"
        "                     routing a path to the hub (e.g. wss://host/ws). Enables the\n"
        "                     Restart/Update buttons, live view and remote control there.\n"
        "                     Remote control (input injection) needs no flag — it works\n"
        "                     only if the app registered debugger tools. Restart and\n"
        "                     update are operator-gated on the server; viewers get neither.\n"
        "  --deny-update      refuse remote update/rollback for this app. Update is allowed\n"
        "                     by default (operators trigger the pipeline: git pull --ff-only,\n"
        "                     trusscli update, trusscli build — run in the project dir while\n"
        "                     the old app keeps running; restart only on success, auto-rollback\n"
        "                     to <binary>.prev if the new binary fails its first run; if the\n"
        "                     pull brings nothing new the rest is skipped. Override the pipeline\n"
        "                     via the config 'update' array — e.g. prepend \"trusscli upgrade\"\n"
        "                     to update TrussC itself; custom pipelines always run fully.)\n"
        "  --thumb-interval <sec>  screenshot push interval (default 30; 0 = never)\n"
        "  --thumb-width <px>      screenshot width (default 512)\n"
        "  --thumb-quality <1-100> JPEG quality (default 75)\n"
        "\n"
        "SERVE OPTIONS\n"
        "  --port <n>         HTTP port (default 54722 — 'truss' on the number row)\n"
        "  --ws-port <n>      agent command-channel port (default: port + 1)\n"
        "  --data <dir>       storage directory for heartbeats/thumbnails\n"
        "                     (default ./anchorbolt-data)\n"
        "  --keep-days <n>    delete stored logs/heartbeats/images older than this\n"
        "                     (default 90; 0 = keep forever). Independent of the\n"
        "                     app-side --log-keep. Thumbnails older than 24h are\n"
        "                     additionally thinned to one per hour.\n"
        "  --auto-approve     execute mutating /mcp calls (restart_app, mutating\n"
        "                     app_call) immediately. Default: they queue for human\n"
        "                     approval — dashboard Approvals badge, or on this machine\n"
        "                     'anchorbolt approvals list|approve [id]|deny [id]'\n"
        "                     (unique id prefixes ok; id optional when one pending).\n"
        "  --approval-ttl <sec>  pending approvals expire after this (default 900)\n"
        "  --offline-after <sec> heartbeat silence before an app counts as offline\n"
        "                     for notify sinks (default 120)\n"
        "\n"
        "  Fleet-wide notifications: <data>/sinks.json (edited on the dashboard's\n"
        "  Notify tab; same engine + presets as the venue-side 'sinks' config, plus\n"
        "  per-sink 'scope' [groups / app:<id>] and serve-only events approval /\n"
        "  offline / online).\n";
}

int main(int argc, char* argv[]) {
    vector<string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printHelp(false);
        return 1;
    }
    const string& cmd = args[0];
    if (cmd == "-h" || cmd == "--help" || cmd == "help" || cmd == "--help-all") {
        bool verbose = cmd == "--help-all" ||
                       any_of(args.begin(), args.end(),
                              [](const string& a) { return a == "--verbose" || a == "-v"; });
        printHelp(verbose);
        return 0;
    }
    if (cmd == "-v" || cmd == "--version" || cmd == "version") {
        cout << "AnchorBolt " << kAnchorboltVersion
             << " (TrussC " << tc::getVersion() << ")" << endl;
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
    if (cmd == "approvals") {
        return cmdApprovals(vector<string>(args.begin() + 1, args.end()));
    }

    cerr << "anchorbolt: unknown command '" << cmd << "'\n" << endl;
    printHelp(false);
    return 1;
}
