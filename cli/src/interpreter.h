#pragma once

#include "client.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fctl {

// Runs fctl commands against a connected backend. Shared by the REPL and
// script mode; every command reports success or failure so scripts can stop
// at the first failure.
class Interpreter {
public:
    enum class Result { Ok, Failed, Quit };

    explicit Interpreter(Client& client);

    // Call once after connecting: loads the vehicle list and subscribes to
    // state for all vehicles.
    void start();

    Result execute(const std::string& line);

    // Io thread.
    void on_stream(const v1::StreamBatch& batch);

    static std::string help();

    // Thread-safe copies for the dashboard.
    std::vector<v1::VehicleInfo> vehicles_snapshot();
    std::map<std::string, v1::VehicleState> states_snapshot();

    // Makes a running wait or sleep return early (as a failure).
    void cancel() { cancelled_ = true; }

private:
    using Args = std::vector<std::string>;
    using Clock = std::chrono::steady_clock;

    Result cmd_vehicles();
    Result cmd_group(const std::string& name, const Args& args);
    Result cmd_mission(const Args& args);
    Result cmd_state(const Args& args);
    Result cmd_watch(const Args& args);
    Result cmd_wait(const Args& args);
    Result cmd_wait_until(const Args& args);
    Result cmd_wait_alt(const Args& args);
    Result cmd_sleep(const Args& args);

    // Expands "all" and comma or space separated IDs. Empty on error.
    std::vector<std::string> resolve_ids(const Args& args, size_t first, size_t last);
    std::vector<std::string> known_ids();
    v1::Response send(v1::Request request);
    Result report_group(const v1::Response& response);
    bool wait_for(
        const std::vector<std::string>& ids,
        const std::function<bool(const v1::VehicleState&)>& predicate,
        std::chrono::milliseconds timeout);

    bool cancelled() const { return cancelled_; }

    Client& client_;
    std::atomic<bool> cancelled_{false};
    std::chrono::milliseconds request_timeout_{15000};

    std::mutex mutex_;
    std::map<std::string, v1::VehicleInfo> vehicles_;
    std::map<std::string, v1::VehicleState> states_;
    std::set<std::string> watched_;
    bool watch_all_ = false;
    Clock::time_point last_watch_print_{};
};

} // namespace fctl
