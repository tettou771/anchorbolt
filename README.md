# anchorbolt

TrussC installation ops tool — anchors a running installation to home base.

One binary, two roles (both ends of the protocol live in this repo, so they
can never drift apart):

- **`anchorbolt start`** — kiosk mode: the venue-side supervisor. Launches your TrussC app
  with the ops environment injected (`TRUSSC_MCP`, `TRUSSC_LOG_FILE`), then
  watches it two ways — process exit and hang (silence on the standard
  `tc_get_health` MCP tool) — and restarts it. The app needs **zero code
  changes**.
- **`anchorbolt serve`** — fleet server: live thumbnail wall, log storage,
  webhook notifications, MCP endpoint for AI-driven operations.

Deliberately a separate tool from `trusscli`: the dev CLI builds projects;
anchorbolt babysits them in production. See the TrussC ROADMAP (kiosk / fleet
entries) for the full settled design.

## Status

Early / Phase 0. Working today (macOS / Linux):

```bash
# venue machine — supervise the app and push to the fleet server
anchorbolt start /path/to/bin/myApp.app/Contents/MacOS/myApp \
    --server http://192.168.1.10:8787 -- --fullscreen

# monitoring machine — dashboard at http://localhost:8787/
anchorbolt serve
```

`start` (kiosk mode):

- spawn with env injection and the app's own arguments (everything after
  `--` is passed verbatim), boot grace period
- watchdog: no healthy `tc_get_health` reply for `--watchdog-timeout`
  seconds (wall clock, default 10) → SIGTERM/SIGKILL → restart.
  `--watchdog-timeout 0` = supervise process exit only, which also makes
  anchorbolt useful for binaries without an MCP endpoint
- process-exit detection → restart
- SIGINT/SIGTERM to anchorbolt shuts the app down cleanly too
- logs land in the platform-conventional place by default
  (`~/Library/Logs/anchorbolt/<id>/` on macOS,
  `$XDG_STATE_HOME/anchorbolt/<id>/` on Linux) and are pruned after
  `--log-keep` days (default 30; 0 keeps everything)
- with `--server`: pushes a heartbeat per health poll, a downscaled JPEG
  every `--thumb-interval` sec (default 30, 0 = never — e.g. for venues
  where screenshots must not leave the machine; `--thumb-width` /
  `--thumb-quality` tune size), and every new app-log / supervisor-event
  line. Log push is independent of app health, so while an app hangs the
  server still sees "unresponsive / restarting" — remotely distinguishable
  from a machine that went dark

Flags: see `anchorbolt --help`. Precedence: flags > `ANCHORBOLT_TOKEN` env >
config file > defaults.

### Config file

For launchd/systemd installs, put everything in a JSON file (comments
allowed) instead of a flag string — `anchorbolt start --config venue.json`,
or just `anchorbolt start` next to an `anchorbolt.json`:

```jsonc
{
  // osaka-entrance kiosk
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "args": ["--fullscreen"],
  "id": "osaka-entrance",
  "server": "https://ops.example.com",
  "tokenFile": "demo.token",          // the token itself NEVER goes in here
  "watchdogTimeout": 10,
  "log": { "keepDays": 30 },
  "thumb": { "interval": 30, "width": 512, "quality": 75 }
}
```

A `token` key in the config is refused with a warning — configs get
committed to git; keep the secret in a separate (gitignored) file pointed
to by `tokenFile`, or in the `ANCHORBOLT_TOKEN` env var.

`serve` (fleet server):

- live thumbnail wall at `/` — green/red per app, fps / size / uptime,
  stale marking after 10s of silence
- click a card for the detail view: big live thumbnail, app-published
  status values, time-series graphs (fps / process memory / machine free /
  custom metrics), custom image streams, and a live log panel (app log +
  supervisor events, severity-colored, filterable)
- ingest: `POST /api/heartbeat` (JSON), `POST /api/thumb/<id>` and
  `POST /api/image/<id>/<name>` (raw JPEG), `POST /api/log/<id>` (JSON lines)
- read: `GET /api/apps`, `GET /api/thumb/<id>`, `GET /api/image/<id>/<name>`,
  `GET /api/history/<id>`, `GET /api/log/<id>?after=<seq>`
- DB-free storage under `--data` (default `./anchorbolt-data`): daily
  heartbeat JSONL + timestamp-named JPEGs per app
- **remote control** (agent keeps one outbound WebSocket, NAT-friendly):
  the detail view shows a live/offline chip, a Restart button, and a tool
  console that relays MCP tool calls to the app. Read-only tools
  (`tc_get_*`) relay freely; anything mutating requires the venue operator
  to have started the agent with `--allow-control`

Options: `-p/--port` (HTTP port, default 8787) `--ws-port <n>` (command
channel, default port+1) `--data <dir>`.

## Agent tokens

Tokens are minted on the **server** side and only their SHA-256 hashes are
stored; the venue machine carries the token:

```bash
# on the server
anchorbolt token new osaka-entrance --data ./anchorbolt-data
# -> prints tc-... once. From then on EVERY agent must authenticate.

# on the venue machine
anchorbolt start ./bin/myApp --server https://ops.example.com \
    --id osaka-entrance --token tc-...       # or ANCHORBOLT_TOKEN env
```

`token list` / `token revoke <app-id>` manage them. No tokens registered =
open mode (today's zero-config behavior on trusted networks). Dashboard
viewers are not authenticated yet — put the HTTP port behind a reverse
proxy (Caddy basic-auth, Cloudflare Tunnel) when exposing it.

## Custom app status

Apps can publish their own monitoring data with one line per value — the
supervisor discovers it via MCP `tools/list`, no anchorbolt configuration:

```cpp
void tcApp::setup() {
    mcp::status("scene",         [&] { return sceneName; });     // shown as-is
    mcp::statusGraph("visitors", [&] { return visitorCount; });  // plotted over time
    mcp::statusImage("entranceCam", [&] { return camPixels; });  // e.g. a webcam
}
```

Values ride every heartbeat; images are fetched on the thumbnail interval
(cheap for the app — same two-stage encode as `tc_get_screenshot`). See
`demo/` for a complete example and TrussC's `docs/AI_AUTOMATION.md`
("Publishing Custom Ops Status") for the convention.

Next: Windows support, retention/pruning, sinks (webhook notifications),
approval queue for AI-driven mutating calls, live view, operator auth.

## Build

A standard TrussC project. Requires a TrussC core with `tc_get_health` /
`tc_get_screenshot(width)` / `TRUSSC_LOG_FILE` (v0.7.0+, currently the `feat/kiosk`
branch).

```bash
trusscli update   # regenerate CMakeLists/CMakePresets (they are gitignored)
trusscli build
```
