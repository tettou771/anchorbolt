#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <TrussC.h>  // tc::Json

// Token management. Registries live under the data dir's config/ (human-set,
// kept on cleanup) and state/ (machine-made, safe to wipe); a dir without
// config/ is the legacy flat layout, which every helper here still reads and
// writes until serve migrates it at startup.
//
//   config/apps.json       {app-id: {token: hash, group?, hidden?}}
//     ONE roster per app: the agent token (publish-only identity for the
//     app's supervisor — any registered => every ingest request / WS hello
//     must authenticate), its wall group, and the hidden flag. Mirrors the
//     settings Apps tab.
//
//   config/operators.json  {name: {role, hash, created, scope?}}
//     humans (and AIs) at the dashboard. Roles: viewer (read-only),
//     operator (+ restart/update/tools/clear), admin (+ token management).
//     Any registered => the dashboard requires login (cookie). Optional
//     "scope" narrows what an operator sees (groups / app:<id>).
//
//   config/shares.json     {sha256(share-tok): {scope?, expires, created}}
//                          — read-only share links delivered as URLs
//   state/sessions.json    {sha256(session-tok): {name, created}}
//                          — login-code sessions (resolve to a live operator)
//   state/codes.json       {6-digit: {kind, subject, expires}}
//                          — single-use pairing / login codes (10 min TTL)
//
// Tokens are shown once; only SHA-256 hashes are stored. Empty/absent
// operator registry = open dashboard (bootstrap path). Verification re-reads
// the file per request: revocation is instant, no sessions to invalidate.
//
// CLI: anchorbolt token agent new|revoke <app-id>
//      anchorbolt token operator new <name> --role viewer|operator|admin [--scope a,app:b]
//      anchorbolt token operator revoke <name>
//      anchorbolt token list
int cmdToken(const std::vector<std::string>& args);

namespace token {

// --- agent tokens ---

// Validate a presented agent token for an app id against the stored hash.
bool verify(const std::string& dataDir, const std::string& appId,
            const std::string& tok);

// Reverse lookup: the app id this raw token authenticates, if any. Lets a
// app learn its server-assigned id from the token alone (no client id).
std::optional<std::string> resolveAgent(const std::string& dataDir,
                                        const std::string& tok);

// Mint (or replace) an agent token for an app id. Returns the plaintext token
// (shown once; only its hash is persisted). Empty on write failure.
std::string mintAgent(const std::string& dataDir, const std::string& appId);

// Remove an agent token. False if none was registered for that id.
bool revokeAgent(const std::string& dataDir, const std::string& appId);

// Agents as a JSON array [{id, hashPrefix}] for admin UIs.
tc::Json listAgents(const std::string& dataDir);

// --- operator tokens ---

struct Operator {
    std::string name;
    std::string role;                 // "viewer" | "operator" | "admin"
    std::vector<std::string> scope;   // group names or "app:<id>"; empty = all
};

// True if any operator is registered => the dashboard requires login.
bool operatorsEnabled(const std::string& dataDir);

// Who is this token? Accepts operator tokens AND login-code session tokens
// (resolved back to their operator, so revoking the operator kills sessions).
// nullopt = invalid/revoked.
std::optional<Operator> verifyOperator(const std::string& dataDir,
                                       const std::string& tok);

// viewer=1 operator=2 admin=3; unknown=0. For "at least this role" checks.
int roleRank(const std::string& role);

// Visibility test: true when the operator's scope is empty (sees all), or the
// app's group is listed, or "app:<appId>" is listed.
bool inScope(const Operator& op, const std::string& appId,
             const std::string& group);

// Mint (or replace) an operator. Returns the plaintext "op-..." token
// (shown once). Empty on invalid role or write failure.
std::string mintOperator(const std::string& dataDir, const std::string& name,
                         const std::string& role,
                         const std::vector<std::string>& scope);

// Re-mint an operator's token keeping its role + scope (recovery without the
// risk of changing the role). Returns the new token, or empty if not found.
std::string renewOperator(const std::string& dataDir, const std::string& name);

// Remove an operator (and, implicitly, all their sessions — verifyOperator
// resolves sessions through operators.json, so an absent operator is dead).
bool revokeOperator(const std::string& dataDir, const std::string& name);

// Replace an operator's scope. False if the operator does not exist.
bool setOperatorScope(const std::string& dataDir, const std::string& name,
                      const std::vector<std::string>& scope);

// Operators as a JSON array [{name, role, scope, created}] for admin UIs.
tc::Json listOperators(const std::string& dataDir);

// --- app groups ---

// The whole groups map {app-id: group}. Apps absent from it are "ungrouped".
tc::Json loadGroups(const std::string& dataDir);

// The group of one app ("" when ungrouped).
std::string groupOf(const std::string& dataDir, const std::string& appId);

// Assign (empty group = remove). Persists the roster (config/apps.json).
bool setGroup(const std::string& dataDir, const std::string& appId,
              const std::string& group);

// --- hidden flag (wall visibility; admin-set => lives in config) ---

bool setHidden(const std::string& dataDir, const std::string& appId, bool hidden);

// App ids currently hidden, as a JSON array (seeds AppState on serve start).
tc::Json hiddenApps(const std::string& dataDir);

// --- sessions ---

// Mint a login session for an operator. Returns the plaintext "os-..." token.
std::string mintSession(const std::string& dataDir, const std::string& name);

// --- 6-digit codes (single-use, TTL) ---

// Mint a 6-digit code. kind = "pair" | "login", subject = app-id | operator.
// Returns the code; collisions with live codes are avoided.
std::string mintCode(const std::string& dataDir, const std::string& kind,
                     const std::string& subject, int ttlSec);

// Redeem (and consume) a code. Returns {kind, subject} on success; nullopt if
// unknown or expired. Expired/matched entries are erased either way.
std::optional<std::pair<std::string, std::string>> redeemCode(
    const std::string& dataDir, const std::string& code);

// --- share links (viewer-scoped tokens delivered as a URL) ---

// Mint a share token (viewer role) with the given scope and an optional expiry
// (ttlSec = 0 → never expires). Returns the plaintext token, shown once.
std::string mintShare(const std::string& dataDir,
                      const std::vector<std::string>& scope, int ttlSec);

// Active shares (hash prefix + scope + expiry), pruning expired ones.
tc::Json listShares(const std::string& dataDir);

// Revoke a share by its hash prefix (from listShares). False if none matched.
bool revokeShare(const std::string& dataDir, const std::string& hashPrefix);

} // namespace token
