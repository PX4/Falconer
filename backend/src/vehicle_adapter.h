#pragma once

#include "connection_layer.h"

#include "falconer/v1/contract.pb.h"

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/events/events.h>
#include <mavsdk/plugins/mission_raw/mission_raw.h>
#include <mavsdk/plugins/telemetry/telemetry.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace falconer {

// One vehicle: a MAVSDK instance fed by exactly one MAVLink stream.
//
// Threading: constructed, commanded and destroyed on the io thread. MAVSDK
// calls never run on the io thread: some "async" MAVSDK calls block while
// waiting for vehicle replies, and those replies arrive through the io thread.
// They run on `mavsdk_pool` instead. MAVSDK callbacks arrive on MAVSDK
// threads; anything that touches adapter state other than the telemetry
// snapshot is posted back to the io thread.
class VehicleAdapter {
public:
    using Done = std::function<void(std::optional<v1::Error>)>;
    using Notify = std::function<void()>;

    VehicleAdapter(
        asio::io_context& io,
        asio::thread_pool& mavsdk_pool,
        std::string id,
        std::shared_ptr<MavlinkStream> stream,
        Notify on_changed);
    ~VehicleAdapter();

    VehicleAdapter(const VehicleAdapter&) = delete;
    VehicleAdapter& operator=(const VehicleAdapter&) = delete;

    const std::string& id() const { return id_; }
    uint8_t system_id() const { return stream_->system_id(); }

    // Io thread.
    bool ready() const { return action_ != nullptr; }
    bool connected() const { return connected_; }

    // Any thread.
    v1::VehicleState snapshot() const;

    // Io thread. `done` is called on the io thread.
    void arm(Done done);
    void disarm(Done done);
    void takeoff(float altitude_m, Done done);
    void land(Done done);
    void return_to_launch(Done done);
    void upload_mission(const std::string& qgc_plan, Done done);
    void start_mission(Done done);

private:
    void on_new_system();
    void attach(std::shared_ptr<mavsdk::System> system);
    void subscribe_telemetry();
    void run(const std::function<void(mavsdk::Action::ResultCallback)>& start, Done done);
    void run_mission(
        const std::function<void(mavsdk::MissionRaw::ResultCallback)>& start, Done done);
    std::optional<v1::Error> check_ready() const;
    void post(std::function<void()> fn);

    asio::io_context& io_;
    asio::thread_pool& mavsdk_pool_;
    std::string id_;
    std::shared_ptr<MavlinkStream> stream_;
    Notify on_changed_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    // Declared before the plugins so it is destroyed after them.
    mavsdk::Mavsdk mavsdk_;
    mavsdk::Mavsdk::RawBytesHandle raw_handle_;
    mavsdk::Mavsdk::NewSystemHandle new_system_handle_;
    std::shared_ptr<mavsdk::System> system_;
    std::unique_ptr<mavsdk::Telemetry> telemetry_;
    std::unique_ptr<mavsdk::Action> action_;
    std::unique_ptr<mavsdk::MissionRaw> mission_;
    // Created on the MAVSDK pool; only touched again on destruction.
    std::unique_ptr<mavsdk::Events> events_;
    bool connected_ = false;

    mutable std::mutex state_mutex_;
    v1::VehicleState state_;
};

} // namespace falconer
