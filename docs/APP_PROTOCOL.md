# App protocol — supervising any app

[日本語](APP_PROTOCOL.ja.md) ・ [← README](../README.md) ・ [Architecture](ARCHITECTURE.md)

TrussC apps get everything on the box for free — health, thumbnails, status,
remote control — because the framework ships the MCP endpoint AnchorBolt
expects. But nothing in that contract actually requires TrussC. This page is
the contract itself: what a Unity, Electron, Processing, openFrameworks or
plain-anything app must implement to be supervised like a native.

> **About the `tc_` prefix:** the protocol originated in
> [TrussC](https://github.com/TrussC-org/TrussC), and the tool names keep its
> prefix the way LSP methods keep `textDocument/`. Implementing it requires
> no TrussC code whatsoever.

- [Levels of integration](#levels-of-integration)
- [What the supervisor hands your app](#what-the-supervisor-hands-your-app)
- [The transport](#the-transport)
- [Tool contracts](#tool-contracts)
- [Timing](#timing)
- [Testing your implementation](#testing-your-implementation)

---

## Levels of integration

Each level is independent bang for buck — stop wherever the effort stops
being worth it. Level 0 costs nothing and is already useful.

| level | you implement | you get |
|---|---|---|
| **0 — launchable** | nothing (run with `--watchdog-timeout 0`) | crash detection + auto-restart, supervisor events on the dashboard, remote restart/update |
| **1 — logs** | append your log lines to the file named by `TRUSSC_LOG_FILE` | log collection, shipping to the server, `search_logs` / dashboard log view |
| **2 — health** | an HTTP JSON-RPC endpoint with `tc_get_health` | hang detection (not just crash), fps/memory graphs, green/red on the wall |
| **3 — screenshots** | `tc_get_screenshot` | live thumbnails on the wall, the live view |
| **4 — extras** | any of `tc_get_status`, `tc_get_alerts`, input tools | custom graphs & status values, phone-reaching alerts, remote control from the browser |

Level 0 deserves emphasis: `anchorbolt start -p anything --watchdog-timeout 0`
already restarts a crashed binary and records the events. Everything below is
about the levels above it.

---

## What the supervisor hands your app

`anchorbolt start` launches your app as a child process with three
environment variables set:

| variable | meaning |
|---|---|
| `TRUSSC_MCP=1` | "you are being supervised — bring up your MCP endpoint" |
| `TRUSSC_MCP_PORT` | the local TCP port to serve it on (default 47777, scanned upward so several supervisors coexist) |
| `TRUSSC_LOG_FILE` | the file to **append** your log lines to |

Two things people trip over:

- **stdout is not captured.** The supervisor collects logs from
  `TRUSSC_LOG_FILE` only. If your app logs to stdout, tee those lines into the
  file yourself (open in append mode — the same file is reused across
  restarts within a day).
- **Shut down on the platform's polite signal.** When the supervisor restarts
  or stops your app it sends SIGTERM (POSIX) / WM_CLOSE (Windows) and waits
  **5 seconds** before force-killing. Exit cleanly within that window.

---

## The transport

The endpoint is a plain HTTP server on `localhost:$TRUSSC_MCP_PORT` answering
JSON-RPC 2.0 on `POST /mcp`. It is a genuine
[MCP](https://modelcontextprotocol.io) server — any MCP client can talk to
your app directly, which is also how you debug it — but the supervisor only
ever uses a fixed subset, and the contract is pinned to that subset so MCP
spec evolution can't break you:

| method | required | used for |
|---|---|---|
| `tools/list` | yes | discovering which optional conventions you speak |
| `tools/call` | yes | everything else |
| `initialize`, `ping` | recommended | not used by the supervisor, but MCP clients (Claude, inspectors) need them |

One request per POST, `Content-Type: application/json`, response is the
JSON-RPC reply. No sessions, no streaming, no notifications.

### Result format

A `tools/call` reply carries MCP **content blocks** in
`result.content`:

- A **data tool** (health, status, alerts) returns one `text` block whose
  `text` is a JSON **object**, serialized:

```json
{"jsonrpc": "2.0", "id": 1,
 "result": {"content": [
   {"type": "text", "text": "{\"status\":\"ok\",\"fps\":60.0,\"pid\":4242}"}
 ]}}
```

- An **image tool** (screenshot, status image) returns one `image` block —
  base64 data plus `mimeType` — optionally followed by a `text` block of JSON
  metadata (the supervisor merges the two):

```json
{"jsonrpc": "2.0", "id": 1,
 "result": {"content": [
   {"type": "image", "data": "<base64 jpeg>", "mimeType": "image/jpeg"},
   {"type": "text", "text": "{\"width\":512,\"height\":288}"}
 ]}}
```

A transport failure, a non-200, or a reply that doesn't parse all count the
same way: as a **miss** on the watchdog clock.

---

## Tool contracts

### `tc_get_health` — required for level 2

Polled on the health cadence (see [Timing](#timing)). Must be cheap — read
counters, touch nothing heavy.

No arguments. Return a JSON object; two rules and the rest is yours:

- **`pid` is mandatory** — your process id, as a number. The supervisor
  checks it against its own child, and a mismatch counts as a miss. This is
  what keeps a stale or foreign app on a shared port from masking a dead
  child.
- The whole object is forwarded to the fleet server as the heartbeat. Fields
  the dashboard already knows how to graph: `fps`, `rssBytes` (process memory,
  the number to watch for leaks), `uptimeSec`. `version`, `width`, `height`
  are displayed where relevant. Extra fields are harmless.

```json
{"status": "ok", "fps": 60.0, "frameCount": 86400, "uptimeSec": 1440.5,
 "width": 1920, "height": 1080, "version": "myapp 2.1", "pid": 4242,
 "rssBytes": 268435456}
```

Answer it from a thread that proves your app is *alive*, not just that a
socket is open — if your render loop can hang while an HTTP thread keeps
answering, you've built a watchdog that watches nothing. (Have the main loop
bump a counter; refuse to answer, or answer stale, when it stops moving.)

### `tc_get_screenshot` — required for level 3

| argument | meaning |
|---|---|
| `format` | `"png"` (default) or `"jpg"` |
| `width` | target width, aspect preserved, never upscale; omit = full resolution |
| `quality` | JPEG quality 1–100 (default 75) |

Return an `image` content block. The supervisor calls it three ways:

- **Thumbnails:** `{format:"jpg", width:512, quality:75}` (the `--thumb-*`
  flags) every `--thumb-interval` (default 30 s).
- **Live view:** the same JPEG call at 1–15 fps while someone watches.
- **Detail view:** `{format:"png"}` — full resolution, on demand.

Grab pixels on your frame loop, but encode off it — at live-view rates an
on-loop encoder becomes visible stutter in the installation.

### Optional conventions — level 4

The supervisor calls `tools/list` once per app run and enables each of these
only if the name is present. Absence is fine; there is no registration
anywhere else.

**`tc_get_status`** — app-published ops values. No arguments; return:

```json
{"values": [
   {"name": "scene",    "value": "idle",  "mode": "status"},
   {"name": "visitors", "value": 132,     "mode": "graph"}
 ],
 "images": ["entranceCam"]}
```

`mode:"graph"` means "plot me over time"; `"status"` means "show the current
value". `images` lists names fetchable via `tc_get_status_image`. The payload
rides the heartbeat to the dashboard.

**`tc_get_status_image`** — arguments `{name, width?, quality?}`; return an
`image` block, JPEG, like the screenshot tool. Polled on the thumbnail
cadence for every name listed by `tc_get_status` — a webcam, a debug view,
whatever pixels you want on the dashboard.

**`tc_get_alerts`** — "a human should hear about this". No arguments; return
pending alerts **and clear them** (drain semantics — exactly one consumer
receives each alert):

```json
{"alerts": [{"at": "2026-07-22T03:12:44", "text": "IR camera disconnected!"}]}
```

Drained on the health cadence; each entry fires the notification sinks
(Slack / Discord / ntfy / …), so this can literally end up on someone's
phone. Also write the message to your own log — it should survive locally
even with no supervisor attached.

**Input tools — the remote-control opt-in.** If `tools/list` contains
`tc_mouse_move` or `tc_key_press`, the app has opted into remote control and
the dashboard shows the control UI (operator role required server-side).
The conventional set:

| tool | arguments |
|---|---|
| `tc_mouse_move` | `x`, `y` (pixels, window coordinates) |
| `tc_mouse_click` | `x`, `y`, `button` (`"left"`/`"right"`/`"middle"`) |
| `tc_key_press` | `key` (a character or a name like `"enter"`, `"space"`, `"left"`) |

Don't expose these unless you mean it: listing them *is* the consent switch.

---

## Timing

The numbers your implementation lives against (defaults; see
[Architecture — timeouts](ARCHITECTURE.md#timeouts-and-thresholds) for the
reasoning):

| what | default | contract for your app |
|---|---|---|
| health poll | `--watchdog-timeout` / 3 ≈ every 3 s | answer fast and cheap, every time |
| hang watchdog | 10 s wall-clock without a healthy reply → restart | ~3 misses and you're gone; make health reflect real liveness |
| boot grace | 120 s | your first healthy reply must land within this; heavy startups are fine |
| thumbnail poll | every 30 s | plus 1–15 fps bursts during live view |
| termination | SIGTERM / WM_CLOSE, then 5 s, then force kill | exit cleanly within 5 s |

---

## Testing your implementation

Talk to your endpoint directly first — no AnchorBolt needed:

```bash
TRUSSC_MCP=1 TRUSSC_MCP_PORT=47777 TRUSSC_LOG_FILE=/tmp/app.log ./myapp &

curl -s localhost:47777/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'

curl -s localhost:47777/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"tc_get_health","arguments":{}}}'
```

Check: the health reply is a `text` block of JSON, the `pid` matches your
process, and a screenshot call returns an `image` block that decodes to an
actual picture. If you implemented `initialize`, any MCP client works too:
`claude mcp add --transport http myapp http://localhost:47777/mcp`.

Then the real thing:

```bash
anchorbolt start -p ./myapp
```

Within the grace window you should see `app healthy (fps ...)` in the
supervisor output. Kill your render loop (keep the process alive) and the
watchdog should restart it — that's the test that matters. Add `--server` and
the app appears on the wall, thumbnail and all.

---

## Pointers

- [Architecture](ARCHITECTURE.md) — how supervision, the spool, and the fleet
  server fit together; the reasoning behind every timeout.
- [Get started](GET_STARTED.md) — deploying for real: tokens, groups,
  notifications, tunnels.
- TrussC's [AI_AUTOMATION.md](https://github.com/TrussC-org/TrussC/blob/main/docs/AI_AUTOMATION.md)
  — the reference implementation of this protocol, including the app-side API
  (`mcp::status`, `mcp::alert`, custom tools).
