#pragma once

#include <filesystem>
#include <string>

// One home for "where is the data directory?" so every CLI verb resolves it
// identically.
//
// serve writes a runtime pointer (serve.json: dataDir/port/pid) into the
// platform state dir at startup; CLI verbs (token, approvals) resolve their
// data directory as:
//
//   explicit --data  >  ./anchorbolt-data  >  the pointer's dataDir
//
// The pointer is rewritten on every serve start and never deleted; it is only
// trusted while its directory still exists, so the CLI keeps working against
// the on-disk registries even when serve is down. It lives per-user — run the
// CLI as the same user as the serve process.

namespace datadir {

// Platform state base: ~/.local/state/anchorbolt (Linux, XDG-aware),
// ~/Library/Logs/anchorbolt (macOS), %LOCALAPPDATA%\anchorbolt (Windows).
// Also the base under which `start` keeps per-app log dirs and pair tokens.
std::filesystem::path stateBase();

std::filesystem::path servePointerPath();

// Called by serve at startup (best-effort; failures are silent).
void writeServePointer(const std::filesystem::path& dataDir, int port);

// Resolve per the order above. An explicit dir is returned verbatim (verbs
// that mint may create it). Returns "" and sets *err when nothing resolves.
std::string resolveDataDir(const std::string& explicitDir, std::string* err);

} // namespace datadir
