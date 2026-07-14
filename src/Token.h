#pragma once

#include <string>
#include <vector>

// Agent-token management. Tokens are minted by the SERVER side (`anchorbolt
// token new <app-id>`), shown once, and stored as SHA-256 hashes in
// <data-dir>/tokens.json — the venue machine only ever carries the token.
// An empty/absent tokens.json means open mode (no enforcement), so the
// zero-config path keeps working on trusted networks.

// `anchorbolt token <new|list|revoke> ...` entry point.
int cmdToken(const std::vector<std::string>& args);

namespace token {

// True if any token is registered => serve must enforce authentication.
bool enforcementEnabled(const std::string& dataDir);

// Validate a presented token for an app id against the stored hash.
bool verify(const std::string& dataDir, const std::string& appId,
            const std::string& tok);

} // namespace token
