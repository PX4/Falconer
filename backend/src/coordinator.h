#pragma once

#include "connection_layer.h"
#include "vehicle_adapter.h"

#include "falconer/v1/contract.pb.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace falconer {

// Owns all vehicles and runs group commands. Above the coordinator nothing
// knows how many vehicles exist; a single vehicle is a group of one.
//
// Io thread only.
class Coordinator {
public:
    using Reply = std::function<void(v1::Response)>;

    Coordinator(asio::io_context& io, asio::thread_pool& mavsdk_pool);
    ~Coordinator();

    void on_new_stream(std::shared_ptr<MavlinkStream> stream);

    // Destroys all vehicles. Call before the connection layer goes away.
    void shutdown() { adapters_.clear(); }

    v1::VehicleList vehicle_list() const;

    // Vehicle commands (arm, takeoff, ...). `reply` runs on the io thread once
    // every addressed vehicle has answered.
    void execute(const v1::Request& request, Reply reply);

    // Latest state of ready vehicles; empty `ids` means all.
    std::vector<v1::VehicleState> states(const std::vector<std::string>& ids) const;

    std::vector<v1::FleetEvent> take_events();
    void clear_events() { events_.clear(); }

private:
    using Op = std::function<void(VehicleAdapter&, VehicleAdapter::Done)>;

    VehicleAdapter* find(const std::string& id) const;
    v1::VehicleInfo info(const VehicleAdapter& adapter) const;
    void on_vehicle_changed(const std::string& id);
    void run_group(
        uint32_t request_id,
        const google::protobuf::RepeatedPtrField<std::string>& ids,
        const Op& op,
        Reply reply);

    asio::io_context& io_;
    asio::thread_pool& mavsdk_pool_;
    std::vector<std::unique_ptr<VehicleAdapter>> adapters_;
    std::set<std::string> announced_;
    std::vector<v1::FleetEvent> events_;
    int next_id_ = 1;
};

} // namespace falconer
