# Get Started

[日本語](GET_STARTED.ja.md) ・ [← README](../README.md) ・ [Architecture →](ARCHITECTURE.md)

This guide goes from a 30-second local trial to a real deployment, one step at a
time. Every command is copy-pasteable; stop wherever your needs are met.

- [1. Try it locally (30 seconds)](#1-try-it-locally-30-seconds)
- [2. Add a dashboard](#2-add-a-dashboard)
- [3. Lock the dashboard before you expose it](#3-lock-the-dashboard-before-you-expose-it)
- [4. Group apps and scope clients](#4-group-apps-and-scope-clients)
- [5. Get notified (Slack, ntfy, …)](#5-get-notified-slack-ntfy-)
- [6. Fleet-wide notifications — the Notify tab](#6-fleet-wide-notifications--the-notify-tab)
- [7. Remote control over a tunnel](#7-remote-control-over-a-tunnel)
- [8. Pair an app with a 6-digit code](#8-pair-an-app-with-a-6-digit-code)
- [9. AI operations and the approval queue](#9-ai-operations-and-the-approval-queue)
- [10. Run it as a service](#10-run-it-as-a-service)

Throughout, `myApp` means your TrussC app — a project directory, a `.app`
bundle, or a plain binary.

---

## 1. Try it locally (30 seconds)

Point AnchorBolt at your app — a project directory, a `.app` bundle, or a
plain binary — and it supervises it:

```bash
anchorbolt start -p myApp
```

Kill the app (close its window, or `kill -9` it); within a couple of seconds
AnchorBolt restarts it. That's the core loop: auto-restart with zero config —
no server, no account — and your app didn't change a line. Logs collect
locally under the platform log dir.

Everything below layers a dashboard, remote control, and notifications on top.
Add only what you need.

---

## 2. Add a dashboard

Run `anchorbolt serve` on a machine you can reach — the same box, a home
server, a VPS:

```bash
anchorbolt serve --data ./anchorbolt-data
# → fleet server on http://localhost:54722
```

Joining a fleet needs a token — there is no open mode, so knowing the URL
alone gets nobody in. Mint one for this app (its id is set here, on the
server — the app never names itself):

```bash
anchorbolt token agent new osaka-entrance --data ./anchorbolt-data
# → prints tc-... once
```

Then point the app at the server. Pass the token once; later runs reuse it:

```bash
anchorbolt start -p myApp --server http://192.168.1.10:54722 --token tc-...
```

The id comes from the token — there is no `--id`. In a terminal you can even
drop `--token`: the first run prompts for a token or a 6-digit pairing code
(see step 8) and remembers it, so the next run just needs `--server`.

Open **http://localhost:54722/** — your app appears on the wall with a live
thumbnail, pushing a heartbeat, a thumbnail every 30s, and its logs. Click a
card for the **detail view**: a bigger thumbnail, fps / memory graphs, the
event history, and a searchable log panel.

![The wall with one app](images/wall-single.png)

![Detail view — graphs and logs](images/detail.png)

---

## 3. Lock the dashboard before you expose it

The venue side is already locked: a fleet needs a token (step 2), so nobody
can POST fake data or connect without one. The **dashboard** is the other
door — with no operators registered it's open, and so is the AI endpoint,
which means anyone with the URL could view — and restart — every app. Before
you put it on the public internet, create yourself an admin:

```bash
# on the server
anchorbolt token operator new toru --role admin --data ./anchorbolt-data
# → prints op-... once. Paste it into the dashboard login.
```

From now on the dashboard asks for a login. Paste that `op-...` token (or a
6-digit login code — see step 8) and you're in.

![The login screen](images/login.png)

Roles: **viewer** (read-only), **operator** (+ restart / update / control), and
**admin** (+ everything in the settings page). Manage them all from the gear
icon → **Operators** tab.

![Settings — Operators tab](images/settings-operators.png)

---

## 4. Group apps and scope clients

When you have more than a handful of apps, group them. Open the gear icon →
**Apps** tab and type a group name next to each app, then Save.

![Settings — Apps tab with groups](images/settings-apps.png)

The wall grows per-group tabs, so you can look at just "osaka" or just "tokyo".

![The wall with group tabs](images/wall.png)

To give a client a login that only shows **their** apps, create an operator
with a **scope** — a comma-separated list of group names or `app:<id>` entries:

```bash
anchorbolt token operator new gallery-client --role viewer --scope tokyo
```

`gallery-client` now sees only the tokyo group; every other app 404s for
them, including through the AI endpoint. An operator with no scope sees
everything.

---

## 5. Get notified (Slack, ntfy, …)

Add a `notify` array to the app's config file on the venue machine. Presets
fill in the templating for you:

```jsonc
// anchorbolt.json on the venue machine
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "server": "https://ops.example.com",
  "notify": [
    { "preset": "slack", "urlFile": "slack.url" },
    { "preset": "ntfy",  "url": "https://ntfy.sh/my-app-alerts" }
  ]
}
```

Now a crash, a hang, a failed update, or an app-raised alert lands in the
channel:

> `[osaka-entrance] restart: app killed by signal 9; restarting`
> `[osaka-entrance] up: app healthy again (restart #1)`

The webhook URL is a secret, so keep it in a gitignored file (`urlFile`) or an
env var (`urlEnv`), never inline in a config that might be committed. `uptime-kuma`
is a special preset that *pings while healthy*, so Kuma alerts you on silence.

Your app can raise its own alerts with one line — a sensor unplugged, a help
button pressed:

```cpp
mcp::alert("IR camera disconnected!");
```

That message flows to the same notification webhooks and to the dashboard's event list.

---

## 6. Fleet-wide notifications — the Notify tab

Step 5 configured notifications on one machine. The server can notify for the whole
fleet instead: open the gear icon → **Notify** tab. Sinks added here are
stored in `config/notify.json` on the server and fire for every app — no
per-machine config to keep in sync.

The same presets apply — **slack**, **discord**, **ntfy**, **uptime-kuma** —
plus **generic**, which reveals a JSON body template with `{{app}}`,
`{{event}}`, `{{msg}}` and `{{time}}` placeholders for anything else. Each
entry takes two filters:

- **events** — which of `restart`, `up`, `down`, `update`, `stop`, `alert` to
  send, plus three only the server can produce: `approval` (a queued AI
  action, step 9), and `offline` / `online` — the server noticing an app's
  heartbeats went silent and came back (`--offline-after`, default 120 s).
  That last pair is the one thing a dead machine can't report about itself.
- **scope** — group names or `app:<id>` entries; blank = all apps. An entry
  scoped to a single `app:<id>` behaves like the same notification configured on that
  app's machine.

Every row has a **Test** button that fires the webhook immediately and reports
success or failure — no waiting for a real crash to find a typo in the URL.
The "How to get a webhook URL" link opens `/help/notify`, a 3-step walkthrough
per service.

---

## 7. Remote control over a tunnel

To reach apps from anywhere, put `serve` behind a reverse proxy or a
Cloudflare tunnel. The dashboard is plain HTTP (port 54722), but the
interactive features (Restart / Update buttons, live view, remote control) ride
a **separate WebSocket hub** on port 54723 — so route a path to it.

**cloudflared ingress** (same hostname, split by path):

```yaml
ingress:
  - hostname: ops.example.com
    path: /ws
    service: ws://localhost:54723
  - hostname: ops.example.com
    service: http://localhost:54722
```

**venue side** — nothing extra to add:

```bash
anchorbolt start -p myApp --server https://ops.example.com
```

An `https://` server tells the agent a TLS-terminating proxy is in front, so it
reaches the hub at `wss://<same-host>/ws` by convention — matching the ingress
above. No per-app WS flag needed. (Only a non-standard proxy path needs an
explicit `--ws-url wss://host/other`.)

Now the detail view has a **Live** button. Click it to watch the screen; toggle
**control** to drive it — clicks, drags and keystrokes go straight to the app.

![Live view with remote control](images/live.png)

Remote control needs the operator role (server side) and an app that opted in
with `mcp::registerDebuggerTools()` — that's the whole gate, no venue-side
flag. The control toggle only appears when the app exposes input tools.
Monitoring alone needs only the HTTP route.

> Remote update is allowed for operators by default — the detail view's
> **Update** button runs `git pull` + rebuild on the venue machine while the
> app keeps running, switching over only on success. An app that must never be
> updated remotely opts out with `--deny-update`.

---

## 8. Pair an app with a 6-digit code

Copying `tc-...` strings to venue machines is error-prone. Instead, mint a
**pairing code** in the settings page (**Apps** tab → *Pairing code*) and read
the 6 digits to whoever's at the venue:

```bash
anchorbolt start -p myApp --server https://ops.example.com --pair 483201
```

The code (valid 10 minutes, single-use) is traded for the app's real token,
which is saved privately on the machine — so later runs need neither `--pair`
nor `--token`. In a terminal you can skip `--pair` entirely: run with just
`--server` and AnchorBolt prompts for the code. The same mechanism gives
**login codes**: mint one for an operator and they sign in by typing 6 digits
instead of pasting a long token.

---

## 9. AI operations and the approval queue

The server is itself an MCP endpoint, so an AI assistant can work the fleet —
search last night's logs, pull screenshots, check health history:

```bash
claude mcp add --transport http anchorbolt https://ops.example.com/mcp \
    --header "Authorization: Bearer op-..."
```

Read tools run directly. **Mutating calls** — `restart_app`, or an `app_call`
to anything that isn't `tc_get_*` — don't: they queue for human approval.
Approve or deny from the dashboard's yellow **`approvals N`** capsule in the
header, or on the server machine:

```bash
anchorbolt approvals list
anchorbolt approvals approve 3f      # unique id prefixes are enough
anchorbolt approvals deny            # id optional when only one is pending
```

No `--data` needed — the CLI finds the running serve's data directory by
itself. A pending entry expires after `--approval-ttl` (default 900 s). On a
trusted LAN where the wait isn't worth it, `serve --auto-approve` restores
direct execution.

---

## 10. Run it as a service

For a permanent install, put everything in a config file and let the OS start
it. `anchorbolt start --generate-config` prints a commented template to start
from; `anchorbolt start` next to an `anchorbolt.json` picks it up automatically:

```jsonc
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "args": ["--fullscreen"],
  "server": "https://ops.example.com",
  "tokenFile": "osaka.token",
  "watchdogTimeout": 10,
  "notify": [ { "preset": "slack", "urlFile": "slack.url" } ]
}
```

There's no `id` key — the id comes from the token. `tokenFile` points at a
gitignored file holding the `tc-...` token; or pair once (step 8) and omit it.

Logs go to the platform-conventional place by default (`~/Library/Logs/anchorbolt/<id>/`
on macOS, `$XDG_STATE_HOME/anchorbolt/<id>/` on Linux, `%LOCALAPPDATA%\anchorbolt\<id>\`
on Windows) — which survives a working directory of `/` under launchd/systemd.
Wrap `anchorbolt start` in a launchd plist or a systemd unit and you're done;
AnchorBolt handles the app, launchd/systemd handles AnchorBolt.

For every flag, run `anchorbolt --help`. For the design behind all this, read
the [Architecture doc](ARCHITECTURE.md).
