# AnchorBolt

**Keep an eye on the apps you left running somewhere else.**

[日本語](README.ja.md) ・ [Get started](docs/GET_STARTED.md) ・ [Architecture](docs/ARCHITECTURE.md) ・ [App protocol](docs/APP_PROTOCOL.md)

![The fleet wall — every app at a glance](docs/images/wall.png)

## Why

You built an app for an art installation, a museum piece, an event booth — and
now it runs unattended on a machine in another building, another city, for
weeks. Then it crashes at 2am. Or it quietly hangs. Or the client asks for a
change and you'd rather not drive there. The usual answers are a pile of shell
scripts, a cron job, and someone pointing a phone camera at the screen "just to
be sure".

**AnchorBolt is the ops layer for those installations.** It babysits your app
on the venue machine — restarts it when it dies or hangs, collects the logs,
and phones home with a live thumbnail so you can see every app at a glance
from your desk. When something needs a human, it can restart, update, or even
let you drive the app remotely from the browser. Your app needs **zero code
changes** to get the core of this.

It's built for [TrussC](https://github.com/TrussC-org/TrussC) apps (it uses
their MCP endpoint for health and screenshots), but the supervision, logging,
and notification parts work for any program you can launch.

## What it does

- **Auto-restart** — watches the app two ways (it exited / it went silent) and
  brings it back. No app code needed.
- **The wall** — a live thumbnail of every app in one browser page, green/red
  at a glance, grouped into tabs.
- **Logs, always** — the app's logs are collected locally and shipped to your
  server, surviving hours of network outage and reconnecting where it left off.
- **Notifications** — only when it matters: a crash, a hang, an app-raised
  alert, or a machine going silent *without* a clean shutdown lands in Slack /
  Discord / ntfy / Uptime Kuma, or a red badge on the wall.
- **Remote update** — press a button to `git pull` + rebuild on the venue
  machine while the app keeps running; it only switches over if the build
  succeeds, and rolls back automatically if the new build won't start.
- **Live view + remote control** — watch the app's screen live and, when you
  allow it, click and type into it from the browser.
- **Graphs & status** — fps, memory (the app's and the whole machine's), and
  any numbers/images your app chooses to publish, plotted over time.
- **For AI** — the server is itself an MCP endpoint, so an assistant can search
  last night's logs, pull screenshots, and (with human approval) restart an app.
- **Access control** — operator logins with roles, per-client visibility scopes,
  and 6-digit codes so nobody copies long secret strings around.

## One tool, two roles

AnchorBolt is a single binary. On the **venue machine** you run the supervisor;
on **your server** you run the fleet dashboard. Both ends of the protocol live
in the same codebase, so they can never drift out of sync.

```bash
# on the venue machine — supervise the app, phone home
anchorbolt start -p myApp --server https://ops.example.com

# on your server — the dashboard everyone looks at
anchorbolt serve
```

An app that only needs local auto-restart can skip `--server` entirely — the
supervisor works fine on its own.

![Clicking an app opens the detail view: live thumbnail, graphs, events, logs](docs/images/detail.png)

## Get started

The [Get Started guide](docs/GET_STARTED.md) walks from a 30-second local trial
to a real deployment behind a Cloudflare tunnel — grouping apps, wiring up
Slack, giving a client a read-only login, and turning on remote control. Each
step is copy-pasteable.

```bash
# the 30-second taste (two terminals on one machine)
anchorbolt serve                       # open http://localhost:54722/
anchorbolt start -p myApp --server http://localhost:54722
```

## How it's built

The [Architecture doc](docs/ARCHITECTURE.md) covers the design: the two-layer
supervision model, the DB-free file storage, the offline-proof log delivery
cursor, the two token classes, the WebSocket command channel, the fleet MCP
surface, and the reasoning behind the choices (why it's separate from
`trusscli`, why files instead of a database, why tokens instead of OAuth).

## Status

Feature-complete for real use on **macOS and Linux**; the venue-side supervisor also
builds for **Windows** (real-hardware verification in progress). It's a young
tool moving fast — the CLI and dashboard are stable enough to deploy, and
recent additions (the approval queue for AI-driven changes, fleet-wide
notifications, shareable view-only URLs) all landed as additive layers.

Deliberately a **separate tool from `trusscli`**: the dev CLI builds projects;
AnchorBolt babysits them in production. See the TrussC ROADMAP (kiosk / fleet
entries) for the full settled design.

## Build

A standard TrussC project. Requires a TrussC core with `tc_get_health` /
`tc_get_screenshot` / `TRUSSC_LOG_FILE` (v0.7.0+).

```bash
trusscli update   # regenerate CMakeLists/CMakePresets (they are gitignored)
trusscli build
```
