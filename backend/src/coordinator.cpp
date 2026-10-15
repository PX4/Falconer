#include "coordinator.h"

#include "log.h"

#include <algorithm>

namespace falconer {

namespace {

v1::Response error_response(uint32_t request_id, v1::ErrorCode code, const std::string& message)
{
    v1::Response response;
    response.set_request_id(request_id);
    response.mutable_error()->set_code(code);
    response.mutable_error()->set_message(message);
    return response;
}

} // namespace

Coordinator::Coordinator(asio::io_context& io, asio::thread_pool& mavsdk_pool) :
    io_(io),
    mavsdk_pool_(mavsdk_pool)
{}

Coordinator::~Coordinator() = default;

void Coordinator::on_new_stream(std::shared_ptr<MavlinkStream> stream)
{
    const auto system_id = stream->system_id();
    const std::string id = "v" + std::to_string(next_id_++);

    for (const auto& existing : adapters_) {
        if (existing->system_id() == system_id) {
            log_warn(
                "system ID " + std::to_string(system_id) + " seen on a second stream; " +
                existing->id() + " and " + id + " are kept apart and flagged");
            if (announced_.count(existing->id()) != 0) {
                v1::FleetEvent event;
                *event.mutable_vehicle_changed() = info(*existing);
                // Flag is recomputed below once the new adapter is added.
                event.mutable_vehicle_changed()->set_system_id_conflict(true);
                events_.push_back(std::move(event));
            }
        }
    }

    adapters_.push_back(std::make_unique<VehicleAdapter>(
        io_, mavsdk_pool_, id, std::move(stream), [this, id]() { on_vehicle_changed(id); }));
}

VehicleAdapter* Coordinator::find(const std::string& id) const
{
    for (const auto& adapter : adapters_) {
        if (adapter->id() == id) {
            return adapter.get();
        }
    }
    return nullptr;
}

v1::VehicleInfo Coordinator::info(const VehicleAdapter& adapter) const
{
    v1::VehicleInfo info;
    info.set_id(adapter.id());
    info.set_connected(adapter.connected());
    info.set_display_system_id(adapter.system_id());
    const bool conflict = std::any_of(adapters_.begin(), adapters_.end(), [&](const auto& other) {
        return other.get() != &adapter && other->system_id() == adapter.system_id();
    });
    info.set_system_id_conflict(conflict);
    return info;
}

void Coordinator::on_vehicle_changed(const std::string& id)
{
    const auto* adapter = find(id);
    if (adapter == nullptr || !adapter->ready()) {
        return;
    }
    v1::FleetEvent event;
    if (announced_.insert(id).second) {
        *event.mutable_vehicle_added() = info(*adapter);
    } else {
        *event.mutable_vehicle_changed() = info(*adapter);
    }
    events_.push_back(std::move(event));
}

v1::VehicleList Coordinator::vehicle_list() const
{
    v1::VehicleList list;
    for (const auto& adapter : adapters_) {
        if (adapter->ready()) {
            *list.add_vehicles() = info(*adapter);
        }
    }
    return list;
}

std::vector<v1::VehicleState> Coordinator::states(const std::vector<std::string>& ids) const
{
    std::vector<v1::VehicleState> out;
    for (const auto& adapter : adapters_) {
        if (!adapter->ready()) {
            continue;
        }
        if (!ids.empty() && std::find(ids.begin(), ids.end(), adapter->id()) == ids.end()) {
            continue;
        }
        out.push_back(adapter->snapshot());
    }
    return out;
}

std::vector<v1::FleetEvent> Coordinator::take_events()
{
    std::vector<v1::FleetEvent> out;
    out.swap(events_);
    return out;
}

void Coordinator::execute(const v1::Request& request, Reply reply)
{
    const auto request_id = request.request_id();
    switch (request.cmd_case()) {
        case v1::Request::kArm:
            run_group(
                request_id,
                request.arm().vehicle_ids(),
                [](VehicleAdapter& v, VehicleAdapter::Done done) { v.arm(std::move(done)); },
                std::move(reply));
            return;
        case v1::Request::kDisarm:
            run_group(
                request_id,
                request.disarm().vehicle_ids(),
                [](VehicleAdapter& v, VehicleAdapter::Done done) { v.disarm(std::move(done)); },
                std::move(reply));
            return;
        case v1::Request::kTakeoff: {
            const float altitude = request.takeoff().altitude_m();
            run_group(
                request_id,
                request.takeoff().vehicle_ids(),
                [altitude](VehicleAdapter& v, VehicleAdapter::Done done) {
                    v.takeoff(altitude, std::move(done));
                },
                std::move(reply));
            return;
        }
        case v1::Request::kLand:
            run_group(
                request_id,
                request.land().vehicle_ids(),
                [](VehicleAdapter& v, VehicleAdapter::Done done) { v.land(std::move(done)); },
                std::move(reply));
            return;
        case v1::Request::kReturnToLaunch:
            run_group(
                request_id,
                request.return_to_launch().vehicle_ids(),
                [](VehicleAdapter& v, VehicleAdapter::Done done) {
                    v.return_to_launch(std::move(done));
                },
                std::move(reply));
            return;
        case v1::Request::kUploadMission: {
            auto plan = std::make_shared<std::string>(request.upload_mission().qgc_plan());
            run_group(
                request_id,
                request.upload_mission().vehicle_ids(),
                [plan](VehicleAdapter& v, VehicleAdapter::Done done) {
                    v.upload_mission(*plan, std::move(done));
                },
                std::move(reply));
            return;
        }
        case v1::Request::kStartMission:
            run_group(
                request_id,
                request.start_mission().vehicle_ids(),
                [](VehicleAdapter& v, VehicleAdapter::Done done) {
                    v.start_mission(std::move(done));
                },
                std::move(reply));
            return;
        default:
            reply(error_response(
                request_id, v1::ERROR_CODE_INVALID_REQUEST, "not a vehicle command"));
            return;
    }
}

void Coordinator::run_group(
    uint32_t request_id,
    const google::protobuf::RepeatedPtrField<std::string>& ids,
    const Op& op,
    Reply reply)
{
    if (ids.empty()) {
        reply(error_response(request_id, v1::ERROR_CODE_INVALID_REQUEST, "no vehicle IDs given"));
        return;
    }

    struct Pending {
        v1::Response response;
        size_t remaining;
        Reply reply;
    };
    auto pending = std::make_shared<Pending>();
    pending->response.set_request_id(request_id);
    pending->remaining = static_cast<size_t>(ids.size());
    pending->reply = std::move(reply);

    // Pre-size so results keep the request's vehicle order.
    auto* group = pending->response.mutable_group();
    for (const auto& id : ids) {
        group->add_results()->set_vehicle_id(id);
    }

    for (int i = 0; i < ids.size(); ++i) {
        auto finish = [pending, i](std::optional<v1::Error> error) {
            if (error) {
                *pending->response.mutable_group()->mutable_results(i)->mutable_error() = *error;
            }
            if (--pending->remaining == 0) {
                pending->reply(std::move(pending->response));
            }
        };
        auto* adapter = find(ids[i]);
        if (adapter == nullptr || !adapter->ready()) {
            v1::Error error;
            error.set_code(v1::ERROR_CODE_UNKNOWN_VEHICLE);
            error.set_message("unknown vehicle " + ids[i]);
            finish(error);
            continue;
        }
        op(*adapter, finish);
    }
}

} // namespace falconer
