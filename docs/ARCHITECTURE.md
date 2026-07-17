# Architecture

[日本語](ARCHITECTURE.ja.md) ・ [← README](../README.md) ・ [Get started](GET_STARTED.md)

The reference and the reasoning: how AnchorBolt is put together and why the
choices are what they are. For step-by-step usage see the
[Get Started guide](GET_STARTED.md); for every flag run `anchorbolt --help`.

- [One binary, two roles](#one-binary-two-roles)
- [Supervision (`start`)](#supervision-start)
- [Log delivery — the offline spool](#log-delivery--the-offline-spool)
- [The fleet server (`serve`)](#the-fleet-server-serve)
- [Storage and retention](#storage-and-retention)
- [The command channel (WebSocket)](#the-command-channel-websocket)
- [Remote update](#remote-update)
- [Live view and remote control](#live-view-and-remote-control)
- [Authentication](#authentication)
- [Fleet MCP (AI operations)](#fleet-mcp-ai-operations)
- [App-published status and alerts](#app-published-status-and-alerts)
- [Platform notes](#platform-notes)
- [Design decisions](#design-decisions)

---

## One binary, two roles

AnchorBolt ships as a single executable with three subcommands:

- **`anchorbolt start`** — the venue-side supervisor. Launches the app with the
  ops environment injected (`TRUSSC_MCP`, `TRUSSC_MCP_PORT`, `TRUSSC_LOG_FILE`),
  watches it, restarts it, and — with `--server` — pushes heartbeats,
  thumbnails and logs home.
- **`anchorbolt serve`** — the fleet server. The dashboard, ingest endpoints,
  DB-free storage, the WebSocket command hub, and the AI-facing `/mcp`.
- **`anchorbolt token`** — mints and revokes the two token classes on the
  server side.

Both ends of the wire protocol live in one codebase, so version skew between a
venue and its server is structurally impossible.

Precedence for every setting: **flags > `ANCHORBOLT_TOKEN` env > config file >
defaults.** The config is JSON with comments allowed; `anchorbolt start` next
to an `anchorbolt.json` loads it automatically.

---

## Supervision (`start`)

The supervisor watches the app **two independent ways**:

1. **Process exit** — `waitpid` (POSIX) / the process handle (Windows). The app
   died; restart it.
2. **Hang** — no healthy reply to the standard `tc_get_health` MCP tool for
   `--watchdog-timeout` seconds (wall-clock, default 10). The process is alive
   but wedged; terminate and restart it.

The watchdog is wall-clock from the last healthy reply, not a miss-counter, so
HTTP timeouts don't stretch the effective window. A boot grace period
(`--grace`, default 15s) keeps a slow-starting app from being killed before it
first reports healthy. `--watchdog-timeout 0` supervises process exit only,
which is what makes AnchorBolt useful for **any** binary, not just TrussC apps
with an MCP endpoint.

Termination is graceful-then-forceful: SIGTERM → 5s → SIGKILL on POSIX; WM_CLOSE
to the app's windows → 5s → TerminateProcess on Windows. A restart waits a
short backoff, and the `restart` event fires at the *actual respawn*, not at
detection — so an OS shutdown that kills the app moments before the supervisor
records only a quiet `stop`, never a phantom incident.

**Port-collision safety:** `tc_get_health` returns the app's pid, and the
supervisor checks it against its own child. If a foreign app answers on a shared
port, that counts as a miss rather than masking a dead child. The MCP port
itself is scanned upward from a default so several supervisors coexist on one
machine.

---

## Log delivery — the offline spool

The venue's logs are the source of truth, always complete, kept locally in daily
files. When `--server` is set, they're also shipped — and the shipping is built
to survive the reality of venue networks, which drop for hours.

The local daily files **are** the spool. A persisted cursor
(`push-cursor.json`) records how far the server has *confirmed* receipt; it
advances only on a 200. So a network outage, an AnchorBolt restart, even a
machine reboot doesn't lose anything — on reconnect the backlog ships from the
cursor. Semantics are **at-least-once**: an in-flight batch is frozen and
resent byte-for-byte, and the server dedups by `(file, byte offset)`, so a lost
acknowledgement costs a duplicate, never a gap. Files not yet confirmed are
protected from the local pruner regardless of age.

The cursor is flushed lazily (every ~60s + on exit, atomic tmp-then-rename) so a
constantly-updated pointer doesn't wear out an SD card. Losing the last flush in
a crash re-sends at most a minute of already-delivered lines, which the server
discards.

Crucially, **log push is independent of app health.** While an app hangs, the
supervisor's own "unresponsive / restarting" lines keep flowing to the server —
so a hung app (being handled) is remotely distinguishable from a machine that
went dark.

---

## The fleet server (`serve`)

`serve` is a headless TrussC app hosting an HTTP server (via the vendored
httplib) plus a small WebSocket hub. It answers:

- **Ingest** (from agents): `POST /api/heartbeat` (health JSON),
  `POST /api/thumb/<id>` and `POST /api/image/<id>/<name>` (raw JPEG),
  `POST /api/log/<id>` and `POST /api/alert/<id>` (JSON lines).
- **Read** (for the dashboard): `GET /api/apps`, `/api/thumb/<id>`,
  `/api/image/<id>/<name>`, `/api/history/<id>` (heartbeat series),
  `/api/log/<id>?after=<seq>`, `/api/alert/<id>?after=<seq>`.
- **Command bridge**: `POST /api/command/<id>` relays a request to the venue
  over the WebSocket hub and blocks on the reply.
- **The dashboard** at `/` — one self-contained HTML page, no external assets.

The dashboard shows the **wall** (a live thumbnail per venue, green/red,
grouped into tabs) and, on click, the **detail view**: a bigger live thumbnail,
fps / memory / custom graphs, custom image streams, the event list, and a
severity-colored searchable log panel.

**Incident badges:** every venue event (`restart` / `up` / `down` / `update` /
`stop` / `alert`) lands in the detail view's event list; incidents
(`restart` / `down` / `alert`) also light a red badge on the wall card until an
operator clicks Clear — so "osaka restarted 3 times overnight" is visible the
next morning without opening anything.

---

## Storage and retention

Storage is **DB-free by design**, under `--data` (default `./anchorbolt-data`):

- daily heartbeat / log / alert JSONL per app (`heartbeat-YYYY-MM-DD.jsonl`, …)
- timestamp-named JPEGs (`thumbs/YYYYMMDD-HHMMSS.jpg`, and per-name custom
  images) — the directory *is* the index
- `tokens.json`, `operators.json`, `groups.json`, `sessions.json`, `codes.json`

At installation scale (a few MB/day/app) a file scan answers "the errors around
2am last night" at ripgrep speed, and append-only files are crash-proof,
migration-free, and readable with `less` when everything else is broken.

**Retention** (`--keep-days`, default 90, 0 = forever) prunes stored JSONL and
images older than the cutoff, hourly. Thumbnails get an extra thinning: the last
24h is kept complete, older shots thin to one per hour. This is **independent of
the venue's local `--log-keep`** — deletions never propagate in either
direction, and the server typically keeps things *longer* than the kiosk (its
whole reason to exist is after-the-fact investigation). All pruning is filename
string math; no mtime, no parsing.

---

## The command channel (WebSocket)

The interactive features — Restart / Update buttons, live view, remote control,
the AI passthrough — need the server to *talk to* a venue, not just receive from
it. That runs over a WebSocket:

- Each venue keeps **one outbound WebSocket** to the server's hub (NAT-friendly;
  the venue dials out, no inbound port at the venue).
- The hub is a minimal RFC6455 server built over TrussC's `TcpServer`, on a
  **separate port** (server port + 1, default 54723) — the vendored httplib has
  no WebSocket support, so the hub is its own thing.
- Server→venue is `{type:"cmd", id, action, …}`; the reply is
  `{type:"result", id, ok, …}`. `POST /api/command/<id>` bridges an HTTP request
  onto this and blocks on the matching reply (15s timeout).

Behind a reverse proxy or tunnel that exposes a single hostname, the venue can't
reach `host:port+1`. An `https://` server URL signals a TLS-terminating proxy in
front, so the venue derives the hub as **`wss://<host>/ws`** by convention —
route that path to the hub in your proxy (the hub ignores the path in its
handshake, so no server-side change is needed). A non-standard proxy path needs
an explicit **`--ws-url`**. Either way, monitoring works fully over the HTTP
route alone — only the interactive features use the hub.

---

## Remote update

The dashboard's **Update** button (operators, allowed by default) runs a
pipeline on the venue machine — by default `git pull --ff-only` →
`trusscli update` → `trusscli build`, overridable via the config `update` array
(prepend `trusscli upgrade` to also update TrussC itself; use any commands for
non-TrussC projects).

The pipeline runs **while the old binary keeps running**, so a failed build
never takes the installation down — the build output streams live into the
dashboard log panel, and the app only restarts onto the new binary on success.
The previous binary is kept as `<binary>.prev`: a **Roll back** button restores
it, and if the new binary doesn't become healthy on its first run it is rolled
back automatically.

Two conveniences: if `git pull` brings nothing new, the default pipeline stops
there and doesn't touch the app (pressing Update "just in case" is free); and a
failed attempt remembers the commit it failed at, so a retry on the same commit
runs fully instead of being skipped. The agent reports the project's git commit
on every heartbeat, so the wall shows which venue runs which version.

It is remote code execution by definition, so it is gated on the operator role
server-side; a venue that must never be updated remotely opts out with
`--deny-update`.

---

## Live view and remote control

The **Live** button streams the app's screen: the agent polls
`tc_get_screenshot` and pushes JPEG frames over the same command WebSocket
(~10fps), only while a browser is watching — polling the frame endpoint starts
the stream, and the agent auto-stops 10s after the browser leaves. Zero explicit
lifecycle.

Toggling **control** maps browser pointer/keyboard events onto the app: clicks
and drags become `tc_mouse_*`, keystrokes become `tc_key_press` (sokol
keycodes), and hover is forwarded throttled to ~20/s so hover-reactive
installations respond without flooding the channel. Coordinates scale by the
app's real window size (from health), not the downscaled frame.

Control requires the operator role (server side) and an app that opted into
input injection with `mcp::registerDebuggerTools()` — that is the whole gate,
no venue flag. A mutating tool exists only if the app registered it, so the
app's own MCP surface *is* the control opt-in; the venue advertises this
(`caps.control`) so the dashboard shows the control toggle only when it applies.

---

## Authentication

Two token classes, both minted on the server, shown once, stored only as
SHA-256 hashes.

**Agent tokens** (`tokens.json`, `{app-id: hash}`) — a venue's publish-only
identity. There is **no open mode**: every ingest request and WebSocket hello
must authenticate, so a fleet is closed by default and knowing the URL alone
grants nothing. The id is bound to the token at mint time; a venue derives its
id from the token (`POST /api/whoami`, a hash reverse-lookup) rather than
declaring one — so there is no `--id`, and a leaked agent token's blast radius
is just that one venue's fake data, never impersonation of another id. A
venue with no `--server` skips all of this (local-only supervision needs no
token).

**Operator tokens** (`operators.json`, `{name: {role, hash, created, scope}}`) —
humans (and AIs) at the dashboard. Roles: **viewer** (read-only), **operator**
(+ restart / update / control / alert-clear), **admin** (+ the settings page).
This class *does* have an open mode: with no operator registered the dashboard
is open (the bootstrap path, so you can reach the settings page to mint the
first admin). With any operator registered, the dashboard requires login. The token rides an
**HttpOnly cookie** (so `<img>` thumbnails authenticate too, which an
`Authorization` header can't do), re-verified against the hash on every request —
so revoking an operator locks them out on their next poll, no session table to
invalidate.

**Scope** narrows what an operator sees: an array of group names or `app:<id>`
entries; empty = everything. Out-of-scope apps 404 everywhere — the wall, every
per-app endpoint, and the fleet `/mcp` — so a venue's existence doesn't even
leak to a scoped client.

**Sessions and 6-digit codes.** Codes (`codes.json`, single-use, 10-min TTL, a
global brute-force guard that 429s after 10 failures) serve two flows:

- **Pairing** — `anchorbolt start --pair <code>` trades the code (no auth; the
  code *is* the auth) for a freshly-minted agent token, saved privately on the
  venue machine (`anchorbolt.token.json` in the platform state dir at 0600,
  keyed by binary name so the machine reuses it on later plain runs). No
  `tc-...` string is ever copied by hand.
- **Login codes** — mint one for an operator; they sign in by typing 6 digits.
  This mints an `os-...` session token (`sessions.json`) that resolves back
  through `operators.json`, so revoking the operator kills their sessions
  automatically.

---

## Fleet MCP (AI operations)

`POST /mcp` on the serve port is a full MCP server (the same JSON-RPC-over-HTTP
transport as TrussC apps), so an AI assistant can be pointed at the fleet:

```bash
claude mcp add --transport http anchorbolt https://ops.example.com/mcp \
    --header "Authorization: Bearer op-..."
```

Tools: `list_apps`, `search_logs`, `tail_logs`, `get_events`,
`get_health_history`, `list_screenshots`, `get_screenshot` (returns a real MCP
image block — the assistant *sees* the installation), `restart_app`, and the
`app_list_tools` / `app_call` passthrough to each app's own MCP tools. The
passthrough uses two fixed proxies with the venue as an argument, rather than
mirroring every app's tools into the fleet's tool list — so the list stays fixed
no matter how many venues connect. Roles apply: viewers get the read-only tools;
`restart_app` and mutating `app_call` need operator (and a mutating tool exists
only if the app registered it).

> **Open-mode caution:** with no operators registered, `/mcp` grants everyone
> admin — which means remote restart to anyone with the URL. Register an
> operator admin token *before* exposing the server publicly.

---

## App-published status and alerts

A TrussC app can expose its own monitoring data with one line per value — no
supervisor configuration; the supervisor discovers the tools via the MCP
`tools/list`:

```cpp
void tcApp::setup() {
    mcp::status("scene",         [&] { return sceneName; });     // shown as-is
    mcp::statusGraph("visitors", [&] { return visitorCount; });  // plotted over time
    mcp::statusImage("entranceCam", [&] { return camPixels; });  // e.g. a webcam
    mcp::alert("IR camera disconnected!");                        // → sinks + dashboard
}
```

`status` values ride every heartbeat; `statusImage` streams are fetched on the
thumbnail interval; `alert` is drained from the standard `tc_get_alerts` tool
and fanned out to the notification sinks and the dashboard event list. It is
deliberately named **alert**, not notify — raise it for events a human should
hear about, not as a message bus.

---

## Platform notes

- **macOS / Linux** — fully supported. Logs default to `~/Library/Logs/anchorbolt/<id>/`
  (macOS) or `$XDG_STATE_HOME/anchorbolt/<id>/` (Linux); the platform-conventional
  location survives a working directory of `/` under launchd/systemd.
- **Windows** — the supervisor uses CreateProcess in a job object with
  `KILL_ON_JOB_CLOSE`, so the whole app process tree dies with the supervisor
  (no orphaned fullscreen window if AnchorBolt is force-killed). Logs go to
  `%LOCALAPPDATA%\anchorbolt\<id>\`. Remote update accounts for the fact that a
  running exe is write-locked but renameable (the backup is a rename, freeing
  the path for the linker). Compiles in CI; real-hardware verification in
  progress.
- All outbound JSON is dumped with invalid-UTF-8 replaced by U+FFFD rather than
  throwing — a localized toolchain streaming CP932 build output into a log must
  not crash the supervisor.

---

## Design decisions

**Separate from `trusscli`.** The dev CLI's job is building projects and it
stays free of watch-and-phone-home behavior — the npm-vs-pm2 split. Nannying is
obnoxious in a builder and is the entire value of an ops agent; the audiences
barely overlap (workshop beginners never meet AnchorBolt; installation pros
don't mind a second tool).

**Files, not a database.** History is a hard requirement, so a store always
exists; append-only JSONL + timestamped JPEGs are crash-proof, migration-free,
greppable, and need no daemon. A database (SQLite as an FTS index) is a possible
*later* optimization over long ranges, with files staying the source of truth —
not a day-one dependency.

**Tokens, not OAuth.** Offline operation is a hard requirement — a venue on a
flaky network must keep working, and the server must deploy per-project without
a Google/SSO dependency. Hashed tokens give instant revocation (which matters
more than statelessness here) and an offline hash check. If SSO is ever needed,
it terminates at a reverse proxy, not in AnchorBolt.

**One templated webhook engine, not per-service adapters.** Slack, Discord,
ntfy, Kuma and any generic endpoint are the same HTTP POST with a body
template; presets just prefill it. Adding a service is adding a preset, not an
adapter.

**Notifications are one-way; interaction lives in the dashboard.** Webhooks
push out (aggregated, deduped), but approving a mutating action, driving the
app, reading logs — those happen in the dashboard, never in a chat app. That
path leads to a swamp of per-service bots and OAuth.

**Local-first.** Most venues only ever need auto-restart and local logs; the
server is for the few who run a fleet. Every server feature is optional — a bare
`anchorbolt start` is a complete, useful tool on its own.
