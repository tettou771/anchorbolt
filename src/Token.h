#pragma once

#include <optional>
#include <string>
#include <vector>

// Token management, two classes minted on the SERVER side and shown once —
// only SHA-256 hashes are stored:
//
//   agent tokens    <data-dir>/tokens.json     {app-id: hash}
//     publish-only identity for a venue supervisor. Any registered =>
//     every ingest request / WS hello must authenticate.
//
//   operator tokens <data-dir>/operators.json  {name: {role, hash, created}}
//     humans (and AIs) at the dashboard. Roles: viewer (read-only),
//     operator (+ restart/update/tools/clear), admin (+ token management).
//     Any registered => the dashboard requires login (cookie).
//
// Empty/absent registry = open mode for that class, so the zero-config
// path keeps working on trusted networks. Verification re-reads the file
// per request: revocation is instant, no sessions to invalidate.
//
// CLI: anchorbolt token agent new|revoke <app-id>
//      anchorbolt token operator new <name> --role viewer|operator|admin
//      anchorbolt token operator revoke <name>
//      anchorbolt token list
int cmdToken(const std::vector<std::string>& args);

namespace token {

// --- agent tokens ---

// True if any agent token is registered => serve must enforce ingest auth.
bool enforcementEnabled(const std::string& dataDir);

// Validate a presented agent token for an app id against the stored hash.
bool verify(const std::string& dataDir, const std::string& appId,
            const std::string& tok);

// --- operator tokens ---

struct Operator {
    std::string name;
    std::string role;  // "viewer" | "operator" | "admin"
};

// True if any operator is registered => the dashboard requires login.
bool operatorsEnabled(const std::string& dataDir);

// Who is this token? nullopt = invalid/revoked.
std::optional<Operator> verifyOperator(const std::string& dataDir,
                                       const std::string& tok);

// viewer=1 operator=2 admin=3; unknown=0. For "at least this role" checks.
int roleRank(const std::string& role);

} // namespace token
