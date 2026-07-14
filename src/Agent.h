#pragma once

#include <string>
#include <vector>

// `anchorbolt agent` entry point. Parses agent options from args (everything
// after the subcommand), then supervises the app until SIGINT/SIGTERM:
// spawn -> poll get_health over local MCP -> restart on exit or hang.
int runAgent(const std::vector<std::string>& args);
