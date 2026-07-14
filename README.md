# anchorbolt

TrussC installation ops tool — anchors a running installation to home base.

One binary, two roles (both ends of the protocol live in this repo, so they
can never drift apart):

- **`anchorbolt start`** — kiosk mode: the venue-side supervisor. Launches your TrussC app
  with the ops environment injected (`TRUSSC_MCP`, `TRUSSC_LOG_FILE`), then
  watches it two ways — process exit and hang (silence on the standard
  `get_health` MCP tool) — and restarts it. The app needs **zero code
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
    --server http://192.168.1.10:8787

# monitoring machine — dashboard at http://localhost:8787/
anchorbolt serve
```

`start` (kiosk mode):

- spawn with env injection, boot grace period
- `get_health` polling → hang detection → SIGTERM/SIGKILL → restart
- process-exit detection → restart
- SIGINT/SIGTERM to anchorbolt shuts the app down cleanly too
- with `--server`: pushes a heartbeat per health poll and a `get_thumbnail`
  JPEG every 30s (raw bytes, cheap for the app — no frame stutter)

Options: `--port` (MCP port, default 47777) `--interval` (poll sec, 3)
`--grace` (boot grace sec, 15) `--misses` (restart threshold, 3)
`--log-dir` (app log destination) `--cwd` `--server <url>` `--id <name>`
(default: binary name) `--thumb-interval <sec>` (30).

`serve` (fleet server):

- live thumbnail wall at `/` — green/red per app, fps / size / uptime,
  stale marking after 10s of silence
- ingest: `POST /api/heartbeat` (JSON), `POST /api/thumb/<id>` (raw JPEG)
- read: `GET /api/apps`, `GET /api/thumb/<id>`
- DB-free storage under `--data` (default `./anchorbolt-data`): daily
  heartbeat JSONL + timestamp-named JPEGs per app
- no auth yet — run it on a trusted network / behind a tunnel

Options: `-p/--port` (HTTP port, default 8787) `--data <dir>`.

Next: Windows support, auth (agent/operator tokens), retention, sinks
(webhook notifications), WS live view, MCP passthrough.

## Build

A standard TrussC project. Requires a TrussC core with `get_health` /
`get_thumbnail` / `TRUSSC_LOG_FILE` (v0.7.0+, currently the `feat/kiosk`
branch).

```bash
trusscli update   # regenerate CMakeLists/CMakePresets (they are gitignored)
trusscli build
```
