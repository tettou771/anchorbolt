#pragma once

#include <string>
#include <vector>

// `anchorbolt serve` entry point — fleet server. Receives heartbeats and
// thumbnails pushed by `anchorbolt start --server`, stores them under a data
// directory, and serves a live thumbnail-wall dashboard over HTTP.
int cmdServe(const std::vector<std::string>& args);

// `anchorbolt approvals` — list / approve / deny queued mutating calls on the
// server machine (same --data directory as the running serve).
int cmdApprovals(const std::vector<std::string>& args);
