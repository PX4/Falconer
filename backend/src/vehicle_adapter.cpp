#include "vehicle_adapter.h"

#include "log.h"

#include <sstream>

namespace falconer {

namespace {

template<typename T> std::string to_text(const T& value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

std::optional<v1::Error> to_error(mavsdk::Action::Result result)
{
    using R = mavsdk::Action::Result;
    if (result == R::Success) {
        return std::nullopt;
    }
    v1::Error error;
    switch (result) {
        case R::NoSystem:
        case R::ConnectionError:
            error.set_code(v1::ERROR_CODE_NOT_CONNECTED);
            break;
        case R::Timeout:
            error.set_code(v1::ERROR_CODE_TIMEOUT);
            break;
        case R::InvalidArgument:
            error.set_code(v1::ERROR_CODE_INVALID_REQUEST);
            break;
        default:
            error.set_code(v1::ERROR_CODE_VEHICLE_REJECTED);
            break;
    }
    error.set_message(to_text(result));
    return error;
}

v1::Error make_error(v1::ErrorCode code, const std::string& message)
{
    v1::Error error;
    error.set_code(code);
    error.set_message(message);
    return error;
}

} // namespace

VehicleAdapter::VehicleAdapter(
    asio::io_context& io,
    asio::thread_pool& mavsdk_pool,
    std::string id,
    std::shared_ptr<MavlinkStream> stream,
    Notify on_changed) :
    io_(io),
    mavsdk_pool_(mavsdk_pool),
    id_(std::move(id)),
    stream_(std::move(stream)),
    on_changed_(std::move(on_changed)),
    mavsdk_(mavsdk::Mavsdk::Configuration{mavsdk::ComponentType::GroundStation})
{
    state_.set_id(id_);

    mavsdk_.add_any_connection("raw://");
    raw_handle_ = mavsdk_.subscribe_raw_bytes_to_be_sent(
        [stream = stream_](const char* bytes, size_t length) { stream->send(bytes, length); });
    stream_->set_receiver([this](const uint8_t* data, size_t len) {
        mavsdk_.pass_received_raw_bytes(reinterpret_cast<const char*>(data), len);
    });
    new_system_handle_ = mavsdk_.subscribe_on_new_system([this]() { on_new_system(); });
}

VehicleAdapter::~VehicleAdapter()
{
    stream_->set_receiver(nullptr);
    mavsdk_.unsubscribe_on_new_system(new_system_handle_);
    mavsdk_.unsubscribe_raw_bytes_to_be_sent(raw_handle_);
    events_.reset();
    action_.reset();
    telemetry_.reset();
}

void VehicleAdapter::post(std::function<void()> fn)
{
    asio::post(io_, [weak = std::weak_ptr<int>(alive_), fn = std::move(fn)]() {
        if (weak.lock()) {
            fn();
        }
    });
}

void VehicleAdapter::on_new_system()
{
    // MAVSDK thread. Plugin creation happens on the io thread.
    post([this]() {
        if (system_) {
            return;
        }
        for (const auto& system : mavsdk_.systems()) {
            if (system->has_autopilot()) {
                attach(system);
                return;
            }
        }
    });
}

void VehicleAdapter::attach(std::shared_ptr<mavsdk::System> system)
{
    system_ = std::move(system);
    telemetry_ = std::make_unique<mavsdk::Telemetry>(system_);
    action_ = std::make_unique<mavsdk::Action>(system_);
    connected_ = system_->is_connected();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_connected(connected_);
    }

    system_->subscribe_is_connected([this](bool connected) {
        post([this, connected]() {
            connected_ = connected;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.set_connected(connected);
            }
            log_info(id_ + (connected ? " connected" : " disconnected"));
            on_changed_();
        });
    });

    subscribe_telemetry();
    log_info(id_ + " ready (system " + std::to_string(system_id()) + ")");
    on_changed_();
}

void VehicleAdapter::subscribe_telemetry()
{
    // Telemetry callbacks only update the snapshot; the contract server reads
    // it at its own rate, so per-message updates never reach the UI directly.
    telemetry_->subscribe_position([this](mavsdk::Telemetry::Position p) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto* pos = state_.mutable_position();
        pos->set_latitude_deg(p.latitude_deg);
        pos->set_longitude_deg(p.longitude_deg);
        pos->set_absolute_altitude_m(p.absolute_altitude_m);
        pos->set_relative_altitude_m(p.relative_altitude_m);
    });
    telemetry_->subscribe_attitude_euler([this](mavsdk::Telemetry::EulerAngle a) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto* att = state_.mutable_attitude();
        att->set_roll_deg(a.roll_deg);
        att->set_pitch_deg(a.pitch_deg);
        att->set_yaw_deg(a.yaw_deg);
    });
    telemetry_->subscribe_velocity_ned([this](mavsdk::Telemetry::VelocityNed v) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto* vel = state_.mutable_velocity();
        vel->set_north_m_s(v.north_m_s);
        vel->set_east_m_s(v.east_m_s);
        vel->set_down_m_s(v.down_m_s);
    });
    telemetry_->subscribe_battery([this](mavsdk::Telemetry::Battery b) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto* bat = state_.mutable_battery();
        bat->set_remaining_percent(b.remaining_percent);
        bat->set_voltage_v(b.voltage_v);
    });
    telemetry_->subscribe_gps_info([this](mavsdk::Telemetry::GpsInfo g) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto* gps = state_.mutable_gps();
        gps->set_fix_type(to_text(g.fix_type));
        gps->set_num_satellites(g.num_satellites);
    });
    telemetry_->subscribe_armed([this](bool armed) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_armed(armed);
    });
    telemetry_->subscribe_in_air([this](bool in_air) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_in_air(in_air);
    });
    telemetry_->subscribe_flight_mode([this](mavsdk::Telemetry::FlightMode mode) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_flight_mode(to_text(mode));
    });
    telemetry_->subscribe_landed_state([this](mavsdk::Telemetry::LandedState landed) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_landed_state(to_text(landed));
    });
    telemetry_->subscribe_health([this](mavsdk::Telemetry::Health health) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.set_armable(health.is_armable);
    });

    // The events plugin may do blocking work, so it is created off the io
    // thread like every other MAVSDK call.
    asio::post(mavsdk_pool_, [this, system = system_]() {
        events_ = std::make_unique<mavsdk::Events>(system);
        events_->subscribe_health_and_arming_checks(
            [this](mavsdk::Events::HealthAndArmingCheckReport report) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.clear_arming_problems();
                for (const auto& problem : report.current_mode_intention.problems) {
                    auto* p = state_.add_arming_problems();
                    p->set_message(problem.message);
                    p->set_component(problem.health_component);
                    p->set_severity(to_text(problem.log_level));
                }
            });
    });
}

v1::VehicleState VehicleAdapter::snapshot() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void VehicleAdapter::run(
    const std::function<void(mavsdk::Action::ResultCallback)>& start, Done done)
{
    if (!action_) {
        done(make_error(v1::ERROR_CODE_NOT_CONNECTED, "vehicle not ready"));
        return;
    }
    if (!connected_) {
        done(make_error(v1::ERROR_CODE_NOT_CONNECTED, "vehicle not connected"));
        return;
    }
    auto callback = [this, done = std::move(done)](mavsdk::Action::Result result) {
        post([done, result]() { done(to_error(result)); });
    };
    asio::post(mavsdk_pool_, [start, callback = std::move(callback)]() { start(callback); });
}

void VehicleAdapter::arm(Done done)
{
    run([this](auto cb) { action_->arm_async(cb); }, std::move(done));
}

void VehicleAdapter::disarm(Done done)
{
    run([this](auto cb) { action_->disarm_async(cb); }, std::move(done));
}

void VehicleAdapter::takeoff(float altitude_m, Done done)
{
    if (altitude_m <= 0.0f) {
        run([this](auto cb) { action_->takeoff_async(cb); }, std::move(done));
        return;
    }
    // Set the altitude first; only take off if the vehicle accepted it.
    run(
        [this, altitude_m](auto cb) { action_->set_takeoff_altitude_async(altitude_m, cb); },
        [this, done](std::optional<v1::Error> error) {
            if (error) {
                done(error);
                return;
            }
            run([this](auto cb) { action_->takeoff_async(cb); }, done);
        });
}

void VehicleAdapter::land(Done done)
{
    run([this](auto cb) { action_->land_async(cb); }, std::move(done));
}

void VehicleAdapter::return_to_launch(Done done)
{
    run([this](auto cb) { action_->return_to_launch_async(cb); }, std::move(done));
}

} // namespace falconer
