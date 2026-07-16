# AnchorBolt

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
AnchorBolt babysits them in production. See the TrussC ROADMAP (kiosk / fleet
entries) for the full settled design.

## Status

Early / Phase 0. Working today (macOS / Linux):

```bash
# venue machine — inside the TrussC project directory:
anchorbolt start --server http://192.168.1.10:54722

# monitoring machine — dashboard at http://localhost:54722/
anchorbolt serve
```

`start` (kiosk mode):

- app resolution mirrors trusscli: bare `anchorbolt start` treats the
  current directory as a TrussC project (`bin/<name>.app` / `bin/<name>`);
  `-p <path>` points at a project directory, a `.app` bundle, or a binary
- spawn with env injection and the app's own arguments (everything after
  `--` is passed verbatim: `anchorbolt start -p demo -- --venue osaka`),
  boot grace period
- watchdog: no healthy `tc_get_health` reply for `--watchdog-timeout`
  seconds (wall clock, default 10) → SIGTERM/SIGKILL → restart.
  `--watchdog-timeout 0` = supervise process exit only, which also makes
  AnchorBolt useful for binaries without an MCP endpoint
- process-exit detection → restart
- SIGINT/SIGTERM to AnchorBolt shuts the app down cleanly too
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
- **offline-proof log delivery**: the local daily files are the spool, and
  a persisted cursor (`push-cursor.json`, flushed lazily — SD-card
  friendly) advances only when the server confirms receipt. Hours of
  venue-network outage, an AnchorBolt restart, even a machine reboot — the
  backlog ships on reconnect. At-least-once semantics: the server dedups
  retries, and files not yet delivered are protected from local pruning
- **remote update** (opt-in via `--allow-update` — it is remote code
  execution by definition): the dashboard's Update button runs a pipeline
  on the venue machine (default `git pull --ff-only` → `trusscli update` →
  `trusscli build`; override with the config `update` array — prepend
  `"trusscli upgrade"` to also pull TrussC itself) **while the old binary
  keeps running**, so a failed build never takes the installation down.
  Build output streams live into the dashboard log panel. On success the
  app restarts onto the new binary; the previous one is kept as
  `<binary>.prev`, and if the new binary doesn't become healthy on its
  first run it is rolled back automatically. A Roll back button restores
  `.prev` manually. If the pull brings nothing new, the remaining steps
  are skipped and the app is left untouched — pressing Update "just in
  case" is free (custom pipelines always run fully and restart; a retry
  after a failed attempt also runs fully, so transient build failures
  stay retryable). The agent also reports the project's git commit on
  every heartbeat, so the wall shows which venue runs which version

- **notification sinks** (config `sinks` array): push supervisor events
  (`restart` / `up` / `down` / `update` / `stop`, plus `alert` — messages the app itself raises with `mcp::alert("IR camera disconnected!")`, drained from the standard `tc_get_alerts` tool) to the outside world —
  ONE templated HTTP engine, no per-service adapters. Presets prefill it:
  `slack`, `discord`, `ntfy` (event messages) and `uptime-kuma` (heartbeat
  mode: pings while the app is healthy, so Kuma alerts on silence). Generic
  sinks take `method` / `contentType` / `body` with `{{app}}` `{{event}}`
  `{{msg}}` `{{time}}` variables (JSON-escaped automatically). Webhook URLs
  are secrets — use `urlFile` (gitignored file next to the config) or
  `urlEnv` instead of an inline `url`. Delivery is at-least-once with an
  in-memory queue per sink: a venue-network outage holds events until it
  heals; 4xx responses drop the event (a bad webhook must not retry
  forever). `events` filters what a sink receives (default: everything).

```jsonc
"sinks": [
  { "preset": "slack",   "urlFile": "slack.url" },
  { "preset": "ntfy",    "url": "https://ntfy.sh/my-venue-alerts" },
  { "preset": "uptime-kuma", "url": "https://kuma.example.com/api/push/KEY" },
  { "url": "https://example.com/hook", "events": ["restart", "down"],
    "body": "{\"venue\":\"osaka\",\"what\":\"{{event}}: {{msg}}\"}" }
]
```

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
- retention (`--keep-days`, default 90, 0 = forever, independent of any
  venue's local policy — deletions never propagate): stored JSONL and
  images older than the cutoff are pruned hourly; thumbnails older than
  24h are additionally thinned to one per hour
- **remote control** (agent keeps one outbound WebSocket, NAT-friendly):
  the detail view shows a live/offline chip, Update / Roll back / Restart
  buttons, and a tool console that relays MCP tool calls to the app.
  Read-only tools (`tc_get_*`) relay freely; anything mutating requires
  the venue operator to have started the agent with `--allow-control`
  (updates: `--allow-update`)

Options: `--port` (HTTP port, default 54722 — "truss" typed on the QWERTY number row; ws = port+1) `--ws-port <n>` (command
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
supervisor discovers it via MCP `tools/list`, no AnchorBolt configuration:

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
