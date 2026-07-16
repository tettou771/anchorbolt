# anchorbolt TODO

Full settled design lives in TrussC `docs/ROADMAP.md` (kiosk / fleet entries).
This file tracks what's left, roughly in priority order.
(Done: delivery cursor / offline spool; serve-side retention `--keep-days`;
remote update — `--allow-update` + pipeline + auto-rollback + git hash on
the wall.)

## 1. Windows: real-hardware verification

Code shipped (CreateProcess + job object w/ KILL_ON_JOB_CLOSE, WM_CLOSE →
TerminateProcess stop, winsock dual-stack port probe, GlobalMemoryStatusEx,
`%LOCALAPPDATA%\anchorbolt\<id>\` logs, rename-based update backup because
a running exe is write-locked but renameable) and compiles on windows-2022
CI. Still needs a real-machine pass: spawn / watchdog restart / clean stop,
remote update incl. the exe rename dance and rollback, log push + cursor,
MCP port scan with two supervisors.

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

## 5. Remote live view (v2)

JPEG frames over the existing WS into `<img>`; remote ImGui panel as an
HTML mirror (`tcx_imgui_get_widgets` → native HTML controls →
`tcx_imgui_click` / `tcx_imgui_input`). Upgrade path: H.264 fMP4 over WS.

## Small stuff

- serve restart empties the log panel ring — preload tail from disk JSONL
- `tc::fromBase64` is a candidate for TrussC core (currently vendored here)
