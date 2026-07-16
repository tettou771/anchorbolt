# anchorbolt TODO

Full settled design lives in TrussC `docs/ROADMAP.md` (kiosk / fleet entries).
This file tracks what's left, roughly in priority order.
(Done: delivery cursor / offline spool; serve-side retention `--keep-days`.)

## 1. Windows support for `start`

CreateProcess + job object (kill child tree), Windows log dir
(`%LOCALAPPDATA%\anchorbolt\<id>\`), port probe already portable?

## 2. Sink engine (webhook notifications)

ONE templated HTTP POST engine (`{{app}}/{{event}}/{{msg}}/{{lines}}`),
presets: Uptime Kuma push, Discord/Slack webhook, ntfy, generic JSON.
Two modes: event notification (error/restart/down) + batched log stream.
Shares the delivery/spool engine (LogShipper pattern: frozen batch + confirm-then-advance).

## 3. Operator tokens + dashboard login

viewer / operator / admin roles; share URLs = viewer-scoped tokens
(+ optional password + expiry). Agent tokens stay publish-only.
Admin lockout recovery = `reset-admin` on the server shell.

## 4. Fleet /mcp for AI

Plain HTTP MCP server on serve: `search_logs`, `tail_logs`,
`get_screenshot_history`, `restart_app`, plus `app_list_tools(app_id)` /
`app_call(app_id, tool, args)` passthrough. Mutating calls go through a
server-side approval queue (block + TTL + approve/deny page).

## 5. Remote update (deploy from the dashboard)

Update button → agent runs a configurable pipeline (config `update` array;
default `git pull --ff-only` → `trusscli update` → `trusscli build`) while
the OLD binary keeps running; restart only on build success — a failed
build never takes the installation down. Async job over the command
channel (immediate "started", build output streams through the existing
log push, outcome rides the heartbeat). Keep `bin/<name>.prev` for a
rollback button; auto-rollback when the new binary isn't healthy within
the boot grace (the watchdog already knows). Gated venue-side by an
explicit `--allow-update` (this is remote code execution by definition);
dashboard-side auth stays the reverse proxy's job until #3. Cheap add-on:
agent puts `git rev-parse` of the project in the heartbeat so the wall
shows which venue runs which commit. Out of scope for v1: TrussC core
upgrade, anchorbolt self-update.

## 6. Remote live view (v2)

JPEG frames over the existing WS into `<img>`; remote ImGui panel as an
HTML mirror (`tcx_imgui_get_widgets` → native HTML controls →
`tcx_imgui_click` / `tcx_imgui_input`). Upgrade path: H.264 fMP4 over WS.

## Small stuff

- serve restart empties the log panel ring — preload tail from disk JSONL
- stray "[NOTICE] TCP server stopped" atexit noise on CLI error paths
- `tc::fromBase64` is a candidate for TrussC core (currently vendored here)
