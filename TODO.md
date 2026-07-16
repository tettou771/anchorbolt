# anchorbolt TODO

Full settled design lives in TrussC `docs/ROADMAP.md` (kiosk / fleet entries).
This file tracks what's left, roughly in priority order.
(Done: delivery cursor / offline spool; serve-side retention `--keep-days`;
remote update — `--allow-update` + pipeline + auto-rollback + git hash on
the wall; sink engine — slack/discord/ntfy/uptime-kuma presets + generic
templated webhook, at-least-once with per-sink queues; Windows start —
real-hardware verified on a JP-locale box incl. remote update + rollback,
CP932 dump crash fixed with dumpSafe, dual-stack serve.)

## 1. Share URLs (deferred from operator tokens)

A share link = viewer-scoped token in URL form (+ optional password +
expiry). Operator tokens/login shipped; admin mint/revoke UI in the
dashboard (CLI-only today) and `reset-admin` recovery verb also pending.

## 2. Fleet /mcp for AI

Plain HTTP MCP server on serve: `search_logs`, `tail_logs`,
`get_screenshot_history`, `restart_app`, plus `app_list_tools(app_id)` /
`app_call(app_id, tool, args)` passthrough. Mutating calls go through a
server-side approval queue (block + TTL + approve/deny page).

## 3. Remote live view (v2)

JPEG frames over the existing WS into `<img>`; remote ImGui panel as an
HTML mirror (`tcx_imgui_get_widgets` → native HTML controls →
`tcx_imgui_click` / `tcx_imgui_input`). Upgrade path: H.264 fMP4 over WS.

## Small stuff

- CP932/mojibake cosmetics: dumpSafe (U+FFFD) stops the crash, but a
  localized MSVC's build output shows as replacement chars on the
  dashboard — consider decoding the console codepage at ingest on Windows
- serve restart empties the log panel ring — preload tail from disk JSONL
- `tc::fromBase64` is a candidate for TrussC core (currently vendored here)
