# Get Started

[日本語](GET_STARTED.ja.md) ・ [← README](../README.md) ・ [Architecture →](ARCHITECTURE.md)

This guide goes from a 30-second local trial to a real deployment, one step at a
time. Every command is copy-pasteable; stop wherever your needs are met.

- [1. Try it locally (30 seconds)](#1-try-it-locally-30-seconds)
- [2. Watch from another machine](#2-watch-from-another-machine)
- [3. Lock the door before you expose it](#3-lock-the-door-before-you-expose-it)
- [4. Group venues and scope clients](#4-group-venues-and-scope-clients)
- [5. Get notified (Slack, ntfy, …)](#5-get-notified-slack-ntfy-)
- [6. Remote control over a tunnel](#6-remote-control-over-a-tunnel)
- [7. Pair a venue with a 6-digit code](#7-pair-a-venue-with-a-6-digit-code)
- [8. Run it as a service](#8-run-it-as-a-service)

Throughout, `myApp` means your TrussC app — a project directory, a `.app`
bundle, or a plain binary.

---

## 1. Try it locally (30 seconds)

Two terminals on one machine. First the dashboard:

```bash
anchorbolt serve
# → fleet server on http://localhost:54722
```

Then the supervisor, pointed at your app and at that dashboard:

```bash
anchorbolt start -p myApp --server http://localhost:54722
```

Open **http://localhost:54722/** — your app appears on the wall with a live
thumbnail. Kill the app (close its window, or `kill -9` it); within a couple of
seconds AnchorBolt restarts it and the wall's uptime resets. That's the whole
core loop, and your app didn't change a line.

![The wall with one venue](images/wall-single.png)

> **Just local?** If you only want auto-restart on the venue machine, drop
> `--server` — no dashboard needed. Logs still collect locally.

---

## 2. Watch from another machine

Run `anchorbolt serve` on a server (a home box, a VPS) and point each venue's
`start` at it:

```bash
# on the server
anchorbolt serve --data ./anchorbolt-data

# on each venue machine
anchorbolt start -p myApp --server http://192.168.1.10:54722 --id osaka-entrance
```

`--id` is the name you'll see on the wall (it defaults to the binary name).
Each venue pushes a heartbeat, a thumbnail every 30s, and its logs. Click a
card to open the **detail view**: a bigger live thumbnail, fps / memory graphs,
the event history, and a searchable log panel.

![Detail view — graphs and logs](images/detail.png)

---

## 3. Lock the door before you expose it

**Do this before putting the dashboard on the public internet.** With no
operators registered, the dashboard is open — and so is the AI endpoint, which
means anyone with the URL could restart a venue. Create yourself an admin:

```bash
# on the server
anchorbolt token operator new toru --role admin --data ./anchorbolt-data
# → prints op-... once. Paste it into the dashboard login.
```

From now on the dashboard asks for a login. Paste that `op-...` token (or a
6-digit login code — see step 7) and you're in.

![The login screen](images/login.png)

Optionally require venues to authenticate too, so nobody can POST fake data:

```bash
anchorbolt token agent new osaka-entrance --data ./anchorbolt-data
# → prints tc-... once. Give it to the venue:
anchorbolt start -p myApp --server https://ops.example.com \
    --id osaka-entrance --token tc-...
```

Roles: **viewer** (read-only), **operator** (+ restart / update / control), and
**admin** (+ everything in the settings page). Manage them all from the gear
icon → **Operators** tab.

![Settings — Operators tab](images/settings-operators.png)

---

## 4. Group venues and scope clients

When you have more than a handful of venues, group them. Open the gear icon →
**Apps** tab and type a group name next to each venue, then Save.

![Settings — Apps tab with groups](images/settings-apps.png)

The wall grows per-group tabs, so you can look at just "osaka" or just "tokyo".

![The wall with group tabs](images/wall.png)

To give a client a login that only shows **their** venues, create an operator
with a **scope** — a comma-separated list of group names or `app:<id>` entries:

```bash
anchorbolt token operator new gallery-client --role viewer --scope tokyo
```

`gallery-client` now sees only the tokyo group; every other venue 404s for
them, including through the AI endpoint. An operator with no scope sees
everything.

---

## 5. Get notified (Slack, ntfy, …)

Add a `sinks` array to the venue's config file. Presets fill in the templating
for you:

```jsonc
// anchorbolt.json on the venue machine
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "id": "osaka-entrance",
  "server": "https://ops.example.com",
  "sinks": [
    { "preset": "slack", "urlFile": "slack.url" },
    { "preset": "ntfy",  "url": "https://ntfy.sh/my-venue-alerts" }
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

That message flows to the same sinks and to the dashboard's event list.

---

## 6. Remote control over a tunnel

To reach venues from anywhere, put `serve` behind a reverse proxy or a
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

**venue** — tell the agent where the WS path is, and opt into control:

```bash
anchorbolt start -p myApp --server https://ops.example.com \
    --ws-url wss://ops.example.com/ws --allow-control
```

Now the detail view has a **Live** button. Click it to watch the screen; toggle
**control** to drive it — clicks, drags and keystrokes go straight to the app.

![Live view with remote control](images/live.png)

Remote control needs *both* the operator role (server side) and `--allow-control`
(venue side), and the app must opt in with `mcp::registerDebuggerTools()`.
Monitoring alone needs only the HTTP route — `--ws-url` is only for the
interactive features.

> Remote update is the same shape: add `--allow-update`, and the detail view's
> **Update** button runs `git pull` + rebuild on the venue while the app keeps
> running, switching over only on success.

---

## 7. Pair a venue with a 6-digit code

Copying `tc-...` strings to venues is error-prone. Instead, mint a **pairing
code** in the settings page (**Apps** tab → *Pairing code*) and read the 6
digits to whoever's at the venue:

```bash
anchorbolt start -p myApp --server https://ops.example.com --pair 483201
```

The code (valid 10 minutes, single-use) is traded for the venue's real token,
which is saved privately on the machine — so later runs need neither `--pair`
nor `--token`. The same mechanism gives **login codes**: mint one for an
operator and they sign in by typing 6 digits instead of pasting a long token.

---

## 8. Run it as a service

For a permanent install, put everything in a config file and let the OS start
it. `anchorbolt start` next to an `anchorbolt.json` picks it up automatically:

```jsonc
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "args": ["--fullscreen"],
  "id": "osaka-entrance",
  "server": "https://ops.example.com",
  "wsUrl": "wss://ops.example.com/ws",
  "tokenFile": "osaka.token",
  "allowControl": true,
  "watchdogTimeout": 10,
  "sinks": [ { "preset": "slack", "urlFile": "slack.url" } ]
}
```

Logs go to the platform-conventional place by default (`~/Library/Logs/anchorbolt/<id>/`
on macOS, `$XDG_STATE_HOME/anchorbolt/<id>/` on Linux, `%LOCALAPPDATA%\anchorbolt\<id>\`
on Windows) — which survives a working directory of `/` under launchd/systemd.
Wrap `anchorbolt start` in a launchd plist or a systemd unit and you're done;
AnchorBolt handles the app, launchd/systemd handles AnchorBolt.

For every flag, run `anchorbolt --help`. For the design behind all this, read
the [Architecture doc](ARCHITECTURE.md).
