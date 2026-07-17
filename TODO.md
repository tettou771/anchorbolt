# anchorbolt TODO

Full settled design lives in TrussC `docs/ROADMAP.md` (kiosk / fleet entries).
This file tracks what's left, roughly in priority order.

## 1. Approval queue for AI-driven mutating calls

Fleet `/mcp` shipped (read tools + passthrough + restart, role-gated) and
executes mutating calls immediately. Remaining: mutating calls (restart /
update / control / mutating `app_call`) block on a server-side approval queue
with a TTL — notification links the human to an approve/deny page on the
dashboard (same shape as a permission prompt; never lives in chat apps). Read
tools stay instant.

## Deferred / out of scope for now

- **`tc::fromBase64` → TrussC core.** Core has `toBase64` (a *documented* API),
  so adding the decode counterpart pulls in the full add-API pipeline
  (`api-reference.toml` + luagen + web regen). A TrussC-side task, not an
  anchorbolt cleanup. Two vendored copies (Start.cpp / Serve.cpp) meanwhile.
- **CP932 / mojibake at ingest (Windows).** `dumpSafe` (U+FFFD) stops the
  crash, but a localized MSVC's build output shows as replacement chars on the
  dashboard — decode the console codepage at ingest. Windows-only; verify on a
  real JP-locale box.
- **Password on share links.** Shares are viewer-scoped + expiry today; an
  optional password gate is a possible follow-up.

## Done

Delivery cursor / offline spool; serve retention `--keep-days`; remote update
pipeline + auto-rollback + git hash on the wall; sink engine (slack / discord /
ntfy / uptime-kuma presets + templated webhook, at-least-once per-sink queues);
`mcp::alert` + dashboard event badge; operator tokens + login
(viewer/operator/admin) + agent tokens; app groups + scoped visibility +
settings page (Apps / Operators tabs, operator & agent mint/revoke UI) + 6-digit
pairing/login codes; fleet `/mcp` for AI; live view + remote control.

This session: fleet push over https (tcxCurl) + ws-url derived from `--server`
(`wss://host/ws` convention); **`--id` abolished** — server-assigned, derived
from the token via `/api/whoami`; **secure by default** — no agent open mode;
interactive onboarding (TTY prompt for a pairing code / token); short `--help`
+ `--verbose`; `--generate-config`; dashboard UX (2-line wall cards, 3-row
detail header, plain health dot, Clear empties the event list, no bogus
last-seen for unreported venues); default boot grace 120s; pair-token file keyed
by binary *path* (not just name) + path recorded inside it; **`--allow-control` /
`--allow-update` removed** — control auto-detected from `registerDebuggerTools`,
update operator-gated with `--deny-update` opt-out, capability-gated dashboard
buttons; log-panel preload from disk after a serve restart; log date picker +
zip export (logs + images, today / 30d / all); screenshot scrubber (today +
past days, timelapse play); **share links** (viewer-scoped URL, scope + expiry,
auto-login, `shares.json`); Windows console-subsystem fix (`TRUSSC_SHOW_CONSOLE`
in local.cmake + core `TRUSSC_LIBRARY_TU` guard); WS Ping→Pong so a tunnel
doesn't reap the command channel (restores the live/control flag).
