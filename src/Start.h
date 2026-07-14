#pragma once

#include <string>
#include <vector>

// `anchorbolt start` entry point — kiosk mode. The first positional argument
// is the app binary; the rest are options. Supervises the app until
// SIGINT/SIGTERM: spawn -> poll get_health over local MCP -> restart on exit
// or hang.
int cmdStart(const std::vector<std::string>& args);
