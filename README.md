# anchorbolt

TrussC installation ops tool — anchors a running installation to home base.

One binary, two roles (both ends of the protocol live in this repo, so they
can never drift apart):

- **`anchorbolt agent`** — venue-side supervisor. Launches your TrussC app
  with the ops environment injected (`TRUSSC_MCP`, `TRUSSC_LOG_FILE`), then
  watches it two ways — process exit and hang (silence on the standard
  `get_health` MCP tool) — and restarts it. The app needs **zero code
  changes**.
- **`anchorbolt serve`** — fleet server: live thumbnail wall, log storage,
  webhook notifications, MCP endpoint for AI-driven operations.
  *(not implemented yet)*

Deliberately a separate tool from `trusscli`: the dev CLI builds projects;
anchorbolt babysits them in production. See the TrussC ROADMAP (kiosk / fleet
entries) for the full settled design.

## Status

Early / Phase 0. Working today (macOS / Linux):

```bash
anchorbolt agent --run /path/to/bin/myApp.app/Contents/MacOS/myApp
```

- spawn with env injection, boot grace period
- `get_health` polling → hang detection → SIGTERM/SIGKILL → restart
- process-exit detection → restart
- SIGINT/SIGTERM to the agent shuts the app down cleanly too

Options: `--port` (MCP port, default 47777) `--interval` (poll sec, 3)
`--grace` (boot grace sec, 15) `--misses` (restart threshold, 3)
`--log-dir` (app log destination) `--cwd`.

Windows agent, thumbnails, sinks, and the server are next.

## Build

A standard TrussC project. Requires a TrussC core with `get_health` /
`get_thumbnail` / `TRUSSC_LOG_FILE` (v0.7.0+, currently the `feat/kiosk`
branch).

```bash
trusscli update   # regenerate CMakeLists/CMakePresets (they are gitignored)
trusscli build
```
