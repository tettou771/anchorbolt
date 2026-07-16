// =============================================================================
// Outbound notification sinks — ONE templated HTTP POST engine; presets
// (slack / discord / ntfy / uptime-kuma) just prefill the template. No
// per-service adapters, ever. Delivery is at-least-once with per-sink queues:
// a venue-network outage holds events until it heals (in-memory; events are
// rare, unlike logs). 4xx responses drop the event — a bad webhook URL must
// not retry forever.
// =============================================================================
#pragma once

#include <TrussC.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SinkConfig {
    std::string name;                              // preset or host, for log lines
    std::string url;                               // resolved from url|urlEnv|urlFile
    std::string method = "POST";
    std::string contentType = "application/json";
    std::string bodyTemplate;                      // {{app}} {{event}} {{msg}} {{time}}
    std::vector<std::string> events;               // empty = all events
    bool heartbeat = false;                        // uptime-kuma: ping while healthy
    int  intervalSec = 60;                         // heartbeat cadence
};

// Parse the config's "sinks" array. urlFile paths resolve next to the config
// file (same git-leak escape hatch as tokenFile); invalid entries are logged
// and skipped, never fatal.
std::vector<SinkConfig> parseSinks(const tc::Json& arr,
                                   const std::filesystem::path& configDir);

class Notifier {
public:
    Notifier(std::vector<SinkConfig> sinks, std::string appId);
    ~Notifier();

    // Queue an event for every subscribed sink. Thread-safe, returns fast —
    // delivery happens on the worker thread, never on the supervision loop.
    void notify(const std::string& event, const std::string& msg);

    // Called on each healthy poll; feeds heartbeat-mode sinks (uptime-kuma
    // alerts on the ABSENCE of pushes, so only health produces one).
    void healthyTick();

    // Final bounded flush (so the "stop" event still gets out), then stop.
    void flushAndStop();

private:
    struct Pending {
        std::string event, msg, time;
    };
    struct SinkState {
        SinkConfig cfg;
        std::deque<Pending> queue;
        std::chrono::steady_clock::time_point lastAttempt{};
        std::chrono::steady_clock::time_point lastBeat{};
        bool failing = false;   // one-shot reachability logging + retry backoff
    };

    void worker();
    bool wants(const SinkState& s, const std::string& event) const;
    bool send(SinkState& s, const Pending& p);   // false = retryable failure
    void beat(SinkState& s);

    std::string appId_;
    std::vector<SinkState> sinks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::chrono::steady_clock::time_point lastHealthy_{};
    std::thread worker_;
};

// Set by cmdStart while sinks are armed; the (detached) update job uses it to
// report update outcomes. shared_ptr so a late thread can't dangle.
extern std::shared_ptr<Notifier> g_notifier;
