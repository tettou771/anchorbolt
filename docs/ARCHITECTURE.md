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
- [Notifications](#notifications)
- [Timeouts and thresholds](#timeouts-and-thresholds)
- [Platform notes](#platform-notes)
- [Design decisions](#design-decisions)

---

## One binary, two roles

AnchorBolt ships as a single executable with four subcommands:

- **`anchorbolt start`** — the venue-side supervisor. Launches the app with the
  ops environment injected (`TRUSSC_MCP`, `TRUSSC_MCP_PORT`, `TRUSSC_LOG_FILE`),
  watches it, restarts it, and — with `--server` — pushes heartbeats,
  thumbnails and logs home.
- **`anchorbolt serve`** — the fleet server. The dashboard, ingest endpoints,
  DB-free storage, the WebSocket command hub, and the AI-facing `/mcp`.
- **`anchorbolt token`** — mints and revokes the two token classes on the
  server side.
- **`anchorbolt approvals`** — lists and decides the AI approval queue from
  the server machine.

Both ends of the wire protocol live in one codebase, so version skew between an
app and its server is structurally impossible.

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
(`--grace`, default 120s) keeps a slow-starting app from being killed before it
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

The app's logs are the source of truth, always complete, kept locally in daily
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
- **Command bridge**: `POST /api/command/<id>` relays a request to the app
  over the WebSocket hub and blocks on the reply.
- **The dashboard** at `/` — one self-contained HTML page, no external assets.

The dashboard shows the **wall** (a live thumbnail per app, green/red,
grouped into tabs) and, on click, the **detail view**: a bigger live thumbnail,
fps / memory / custom graphs, custom image streams, the event list, and a
severity-colored searchable log panel.

**Incident badges:** every app event (`restart` / `up` / `down` / `update` /
`stop` / `alert`) lands in the detail view's event list; incidents
(`restart` / `down` / `alert`) also light a red badge on the wall card until an
operator clicks Clear — so "osaka restarted 3 times overnight" is visible the
next morning without opening anything.

---

## Storage and retention

Storage is **DB-free by design**, under `--data` (default `./anchorbolt-data`),
split by one question — *would a human have to redo work if this were
deleted?*:

```
anchorbolt-data/
├── config/                  # human-set — cleanup keeps this
│   ├── apps.json            #   {app-id: {token: <sha256>, group, hidden}} — the one roster
│   ├── operators.json       #   operator accounts (role, hash, scope)
│   ├── shares.json          #   share links
│   └── notify.json          #   fleet-wide notifications (the Notify tab)
├── state/                   # machine-made — `rm -rf state/` is safe
│   ├── sessions.json  codes.json  approvals.json  apps-health.json
│   └── approval-decisions/
└── apps/<id>/               # per-app content — delete per client
    ├── heartbeat-*.jsonl  log-*.jsonl  alert-*.jsonl
    ├── thumbs/              # timestamp-named JPEGs — the directory *is* the index
    └── images/<name>/       # custom statusImage streams
```

- **`config/`** is everything a human set up — app tokens / groups, operators,
  share links, notification settings. Cleanup never touches it; copy `config/`
  to move a server.
- **`state/`** regenerates itself — sessions, login codes, the approval queue,
  the health cache. Deleting it logs everyone out and nothing more.
- **`apps/<id>/`** is the big per-client content. When a job ends, delete that
  one directory.

A data directory from an older release (flat files at the top level) is
migrated to this layout automatically the first time the new `serve` starts —
no manual steps.

At installation scale (a few MB/day/app) a file scan answers "the errors around
2am last night" at ripgrep speed, and append-only files are crash-proof,
migration-free, and readable with `less` when everything else is broken.

**Retention** (`--keep-days`, default 90, 0 = forever) prunes stored JSONL and
images older than the cutoff, hourly. Thumbnails get an extra thinning: the last
24h is kept complete, older shots thin to one per hour. This is **independent of
the venue-side `--log-keep`** — deletions never propagate in either
direction, and the server typically keeps things *longer* than the kiosk (its
whole reason to exist is after-the-fact investigation). All pruning is filename
string math; no mtime, no parsing.

---

## The command channel (WebSocket)

The interactive features — Restart / Update buttons, live view, remote control,
the AI passthrough — need the server to *talk to* an app, not just receive from
it. That runs over a WebSocket:

- Each app's agent keeps **one outbound WebSocket** to the server's hub
  (NAT-friendly; the venue side dials out, no inbound port at the venue).
- The hub is a minimal RFC6455 server built over TrussC's `TcpServer`, on a
  **separate port** (server port + 1, default 54723) — the vendored httplib has
  no WebSocket support, so the hub is its own thing.
- Server→agent is `{type:"cmd", id, action, …}`; the reply is
  `{type:"result", id, ok, …}`. `POST /api/command/<id>` bridges an HTTP request
  onto this and blocks on the matching reply (15s timeout).

Behind a reverse proxy or tunnel that exposes a single hostname, the venue side
can't reach `host:port+1`. An `https://` server URL signals a TLS-terminating
proxy in front, so the agent derives the hub as **`wss://<host>/ws`** by convention —
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
on every heartbeat, so the wall shows which app runs which version.

It is remote code execution by definition, so it is gated on the operator role
server-side; an app that must never be updated remotely opts out with
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
input injection with `mcp::registerControlTools()` — that is the whole gate,
no venue-side flag. A mutating tool exists only if the app registered it, so the
app's own MCP surface *is* the control opt-in; the agent advertises this
(`caps.control`) so the dashboard shows the control toggle only when it applies.

---

## Authentication

Two token classes, both minted on the server, shown once, stored only as
SHA-256 hashes.

**Agent tokens** (in `config/apps.json`, `{app-id: {token: <sha256>, group,
hidden}}` — the one roster, which also carries each app's group and visibility) —
an app's publish-only identity. There is **no open mode**: every ingest request
and WebSocket hello must authenticate, so a fleet is closed by default and
knowing the URL alone grants nothing. The id is bound to the token at mint
time; an app derives its id from the token (`POST /api/whoami`, a hash
reverse-lookup) rather than declaring one — so there is no `--id`, and a leaked
agent token's blast radius is just that one app's fake data, never
impersonation of another id. An app with no `--server` skips all of this
(local-only supervision needs no token).

**Operator tokens** (`config/operators.json`, `{name: {role, hash, created, scope}}`) —
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
per-app endpoint, and the fleet `/mcp` — so an app's existence doesn't even
leak to a scoped client.

**Sessions and 6-digit codes.** Codes (`state/codes.json`, single-use, 10-min TTL, a
global brute-force guard that 429s after 10 failures) serve two flows:

- **Pairing** — `anchorbolt start --pair <code>` trades the code (no auth; the
  code *is* the auth) for a freshly-minted agent token, saved privately on the
  venue machine (`anchorbolt.token.json` in the platform state dir at 0600,
  keyed by binary name so the machine reuses it on later plain runs). No
  `tc-...` string is ever copied by hand.
- **Login codes** — mint one for an operator; they sign in by typing 6 digits.
  This mints an `os-...` session token (`state/sessions.json`) that resolves back
  through `config/operators.json`, so revoking the operator kills their sessions
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
passthrough uses two fixed proxies with the app as an argument, rather than
mirroring every app's tools into the fleet's tool list — so the list stays fixed
no matter how many apps connect. Roles apply: viewers get the read-only tools;
`restart_app` and mutating `app_call` need operator (and a mutating tool exists
only if the app registered it).

### Approval queue

Mutating `/mcp` calls (`restart_app`, any `app_call` that is not `tc_get_*`) do
not execute directly: they queue for **human approval**. The call waits ~20 s
for a decision — approvals often happen while someone is at the dashboard —
then returns a pending ticket (`ap-xxxxxx`) that the AI polls with the
read-only `get_approval` tool. Approved → the action executes inside serve and
the result lands on the ticket; denied → `denied by <name>`; undecided past
`--approval-ttl` (default 900 s) → expired. Read tools are never queued.

Two ways to decide, both human-only — the `/mcp` surface deliberately has no
approve tool, so an AI cannot approve its own request:

- **Dashboard:** an `approvals N` capsule appears in the header for operators
  whose scope covers the app; the panel lists pending entries with
  Approve / Deny.
- **Server machine:** `anchorbolt approvals list | approve [id] | deny [id]`.
  Ids accept unique prefixes (`approve 3f`); with exactly one pending entry the
  id can be omitted. CLI decisions are written one-file-per-decision under
  `state/approval-decisions/` and picked up by serve's sweeper (~2 s), so the CLI
  never races serve's own `state/approvals.json` writes and execution — which needs
  the agent WebSocket — always happens inside serve.

The queue lives in `state/approvals.json` and survives a serve restart. To find the
data directory without `--data`, the CLI reads the **runtime pointer** serve
writes at startup — `serve.json` (`dataDir`, `port`, `pid`) in the platform
state dir (`~/.local/state/anchorbolt/` on Linux, `~/Library/Logs/anchorbolt/`
on macOS, `%LOCALAPPDATA%\anchorbolt\` on Windows). Resolution order: explicit
`--data` > `./anchorbolt-data` > the pointer (trusted only while its directory
still exists; it is rewritten on every serve start and never deleted, so the
CLI keeps working against the queue file even while serve is down). Note the
pointer is per-user: run the CLI as the same user as the serve process.

`serve --auto-approve` restores direct execution (e.g. a trusted LAN where the
20 s wait is not worth it).

> **Open-mode caution:** with no operators registered, `/mcp` grants everyone
> admin — which means remote restart to anyone with the URL. Register an
> operator admin token *before* exposing the server publicly. The approval
> queue still applies in open mode, but anyone at the dashboard can approve.

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
    mcp::alert("IR camera disconnected!");                        // → notifications + dashboard
}
```

`status` values ride every heartbeat; `statusImage` streams are fetched on the
thumbnail interval; `alert` is drained from the standard `tc_get_alerts` tool
and fanned out to the notification webhooks and the dashboard event list. It is
deliberately named **alert**, not notify — raise it for events a human should
hear about, not as a message bus.

---

## Notifications

One templated webhook engine delivers every outbound notification: each entry
(internally a "sink") is a URL plus an optional body template with `{{app}}` `{{event}}` `{{msg}}`
`{{time}}` placeholders; presets (slack / discord / ntfy / uptime-kuma) just
prefill it. Delivery is at-least-once with a queue and backoff per destination — a
network outage holds events until it heals; a 4xx drops the event (a bad
webhook URL must not retry forever).

The engine runs in **two places**, and they are independent by design (both
configured = both deliver):

- **Venue side** — the `notify` array in the app's `anchorbolt.json`
  (the legacy `sinks` key still works). Works
  with no server at all; this is the local-first path.
- **Server side** — `config/notify.json`, edited on the dashboard's **Notify**
  tab (admin). One place for the whole fleet, which is the point: each entry
  takes a `scope` (group names / `app:<id>`, operator-scope semantics, blank =
  every app), so "client A's Slack hears only client A's apps" is one field,
  and an entry scoped to a single `app:<id>` behaves like the same
  notification configured on that app's machine.

Server-side entries hear everything apps push (`restart` / `up` / `down` /
`update` / `stop` / `alert`, fanned out at the ingest point) **plus three
events only the server can know**:

- **`approval`** — a mutating AI call entered the approval queue.
- **`offline` / `online`** — heartbeat silence past `--offline-after`
  (default 60 s) and its recovery. A machine that dies can't report its own
  death; only the server sees the silence. The watcher baselines on its first
  pass, so a serve restart never re-announces long-gone apps.

### What actually pages you, and when

The failure classes deliberately map to different signals, so a webhook means
"come look" and nothing else:

| what happened | who can tell | signal |
|---|---|---|
| app crashed or hung | the supervisor (it's still alive) | `restart` / `down` events, ⚠ badge — within seconds |
| OS / UPS shutdown (planned) | the supervisor, on its way out | a quiet gray `stop` event; **the `offline` that follows is NOT notified** |
| power cut, kernel panic, vanished network | nobody on the machine | `offline` after `--offline-after` of silence, marked `(no stop received — unexpected)` |

The middle row is the subtle one. A graceful shutdown (systemd/launchd sends
SIGTERM — a UPS-initiated shutdown takes this same path, no app code needed)
fires a `stop` event before the silence starts. The server remembers it: when
the offline threshold later passes, a silence that was *announced by a clean
stop* turns the wall dot red but sends **no** webhook (the matching `online`
on reboot is equally quiet). A silence with no stop on record is the scary
kind, and that one notifies. So a nightly powered-down installation stays
silent in your Slack, while a tripped breaker pages you.

The trade-off, stated honestly: if a machine shuts down cleanly and then
*fails to come back*, no webhook fires — the red dot on the wall is the only
sign. If you need a hard guarantee for "it must be up by 10:00", point an
uptime-kuma monitor (scope `app:<id>`) at it — Kuma alerts on the absence of
pushes on its own schedule, independent of this logic.

uptime-kuma on the server requires a scope of exactly one `app:<id>` — it
beats while that app's heartbeats stay fresh, matching a venue-side kuma entry.
Broader kuma scopes have no sound "healthy" semantics and are rejected at load
with a warning.

Every Notify-tab row has a **Test** button that fires the row *as edited*
synchronously and reports the outcome (HTTP status or connection error), and
the tab links `/help/notify` — short webhook walkthroughs for each service.
Saving re-arms the notifier immediately; no restart needed.

---

## Timeouts and thresholds

The timing constants form one chain, and the rule that keeps it honest is:
**each layer is more patient than the one beneath it**, so a hiccup at one
level can't masquerade as a failure at the next.

| layer | default | reasoning |
|---|---|---|
| health poll (supervisor → app MCP) | `--watchdog-timeout` / 3 ≈ 3 s | frequent enough that the watchdog window means ~3 chances, cheap enough to not matter |
| hang watchdog (restart) | 10 s wall-clock | wall-clock since the last healthy reply — HTTP timeouts can't stretch it |
| boot grace | 120 s | heavy apps (shader warmup, model loads) routinely need more than 15 s for their first healthy reply |
| push HTTP timeout (venue → server) | 10 s | measured on Raspberry-Pi-class boxes over a tunnel: 2 s produced false "unreachable" streaks — a *late* reply is not a *dead* server. The push runs on the supervision cadence, so one slow round-trip must stay smaller than the layers above |
| `offline` status (red dot AND notify event) | `--offline-after` 60 s | ONE threshold: the wall shows the server's offline verdict — the same flag that fires the notification — so "red" and "you got paged" are a single state. A machine that died can't report it; only sustained silence shows it. ~20 missed heartbeats keeps a tunnel flap from paging anyone, while a real crash reaches you sooner via the down/restart notifications. A silence announced by a clean `stop` (planned shutdown) turns the dot red but does not notify — see the Notifications section |
| approval wait | ~20 s, then a ticket (TTL 900 s) | most approvals happen while someone is at the dashboard; past that the AI polls `get_approval` instead of holding a connection |

When tuning: keep `poll < push timeout < offline`, and remember the
wall dot is cosmetic — supervision (restart) and notification (down/alert)
each have their own clocks and fire regardless of what the dashboard shows.

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
adapter. The same engine runs on the venue machine (per-app `notify` in
`anchorbolt.json`) and on the server (fleet-wide entries in `config/notify.json`,
the dashboard's Notify tab).

**Notifications are one-way; interaction lives in the dashboard.** Webhooks
push out (aggregated, deduped), but approving a mutating action, driving the
app, reading logs — those happen in the dashboard, never in a chat app. That
path leads to a swamp of per-service bots and OAuth.

**Local-first.** Most apps only ever need auto-restart and local logs; the
server is for the few who run a fleet. Every server feature is optional — a bare
`anchorbolt start` is a complete, useful tool on its own.
