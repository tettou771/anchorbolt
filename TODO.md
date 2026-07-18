# anchorbolt TODO

Full settled design lives in TrussC `docs/ROADMAP.md` (kiosk / fleet entries).
This file tracks what's left, roughly in priority order.

## Deferred / out of scope for now

- **Serve-side sink notifications.** The Notifier/sink engine lives on the
  venue (`start`) only; serve has no push channel. Wiring it into serve would
  let "approval pending" ping slack/ntfy — until then the dashboard badge and
  `anchorbolt approvals list` are the ways to notice a queued call.

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

Approval queue for AI-driven mutating calls: fleet `/mcp` restart_app +
mutating app_call queue for human approval by default (`--auto-approve` opts
out, `--approval-ttl` default 900s); the call waits ~20s for a decision, then
returns a ticket polled via the read-only `get_approval` tool; deciders are the
dashboard Approvals badge/panel (operator+ within scope) and `anchorbolt
approvals list|approve|deny` on the server machine (unique id prefixes; id
optional when exactly one is pending; decisions travel as one-file-per-decision
under approval-decisions/ so the CLI never races serve's approvals.json). The
/mcp surface has no approve tool — an AI cannot approve its own request.

Delivery cursor / offline spool; serve retention `--keep-days`; remote update
pipeline + auto-rollback + git hash on the wall; sink engine (slack / discord /
ntfy / uptime-kuma presets + templated webhook, at-least-once per-sink queues);
`mcp::alert` + dashboard event badge; operator tokens + login
(viewer/operator/admin) + agent tokens; app groups + scoped visibility +
settings page (Apps / Operators tabs, operator & agent mint/revoke UI) + 6-digit
pairing/login codes; fleet `/mcp` for AI; live view + remote control.

Instant screenshot download in the detail view (live -> relayed full-res PNG,
offline -> last thumbnail fallback); ingest-time image dedup (a frame equal to
the previous one is not stored — thumbnails AND statusImage streams, per name —
so static screens stop bloating disk/zip; the scrubber's "newest frame at or
before t" holds the last one across the gap, so playback is unchanged and no log
is needed to reconstruct the picture).

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
