#include "interpreter.h"

#include "output.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace fctl {

namespace {

constexpr auto kWatchPeriod = std::chrono::seconds(1);
constexpr auto kPollPeriod = std::chrono::milliseconds(100);
constexpr double kDefaultWaitS = 60.0;

std::vector<std::string> tokenize(const std::string& line)
{
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        out.push_back(token);
    }
    return out;
}

bool parse_number(const std::string& s, double& out)
{
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(out);
}

std::string error_name(v1::ErrorCode code)
{
    std::string name = v1::ErrorCode_Name(code);
    const std::string prefix = "ERROR_CODE_";
    if (name.rfind(prefix, 0) == 0) {
        name = name.substr(prefix.size());
    }
    return name;
}

std::string format_error(const v1::Error& error)
{
    return error_name(error.code()) + " (" + error.message() + ")";
}

// Sorts v2 before v10.
bool id_less(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    return a < b;
}

// Multi-line view for "state <id>".
std::string format_state_detail(const v1::VehicleState& s)
{
    const auto& p = s.position();
    const auto& a = s.attitude();
    const auto& v = s.velocity();
    char out[1024];
    std::snprintf(
        out,
        sizeof(out),
        "%s\n"
        "  connected    %s\n"
        "  armed        %s\n"
        "  armable      %s\n"
        "  in air       %s (%s)\n"
        "  flight mode  %s\n"
        "  position     %.7f, %.7f\n"
        "  altitude     %.2f m relative, %.2f m AMSL\n"
        "  attitude     roll %.1f, pitch %.1f, yaw %.1f deg\n"
        "  velocity     N %.2f, E %.2f, D %.2f m/s\n"
        "  battery      %.0f%%, %.2f V\n"
        "  gps          %s, %d satellites\n"
        "  mission      item %d of %d",
        s.id().c_str(),
        s.connected() ? "yes" : "no",
        s.armed() ? "yes" : "no",
        s.armable() ? "yes" : "no",
        s.in_air() ? "yes" : "no",
        s.landed_state().c_str(),
        s.flight_mode().c_str(),
        p.latitude_deg(),
        p.longitude_deg(),
        p.relative_altitude_m(),
        p.absolute_altitude_m(),
        a.roll_deg(),
        a.pitch_deg(),
        a.yaw_deg(),
        v.north_m_s(),
        v.east_m_s(),
        v.down_m_s(),
        s.battery().remaining_percent(),
        s.battery().voltage_v(),
        s.gps().fix_type().c_str(),
        s.gps().num_satellites(),
        s.mission().current(),
        s.mission().total());
    std::string text = out;
    for (const auto& problem : s.arming_problems()) {
        text += "\n  problem      " + problem.severity() + ": " + problem.message();
        if (!problem.component().empty()) {
            text += " [" + problem.component() + "]";
        }
    }
    return text;
}

std::string format_state(const v1::VehicleState& s)
{
    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "%-4s %-9s %-8s %-6s %-16s alt %6.1f m  pos %.6f, %.6f  bat %3.0f%%  gps %s/%d",
        s.id().c_str(),
        s.connected() ? (s.armed() ? "armed" : "disarmed") : "LOST",
        s.in_air() ? "in-air" : "ground",
        s.armable() ? "ready" : "-",
        s.flight_mode().c_str(),
        s.position().relative_altitude_m(),
        s.position().latitude_deg(),
        s.position().longitude_deg(),
        s.battery().remaining_percent(),
        s.gps().fix_type().c_str(),
        s.gps().num_satellites());
    return line;
}

} // namespace

Interpreter::Interpreter(Client& client) :
    client_(client)
{}

std::string Interpreter::help()
{
    return R"(Vehicles are addressed by ID (v1, v2, ...) or "all". Several IDs: "v1 v2" or "v1,v2".

  vehicles                         list vehicles
  state <id> [--json]              latest state of one vehicle; --json for the raw message
  watch [ids|all]                  print state once a second; "watch" alone = all
  unwatch                          stop printing state
  arm <ids|all>                    arm
  disarm <ids|all>                 disarm
  takeoff <ids|all> [alt_m]        take off, optionally to alt_m above home
  land <ids|all>                   land
  rtl <ids|all>                    return to launch
  mission upload <ids|all> <file.plan>
                                   upload the mission items of a QGC plan
  mission start <ids|all>          start the uploaded mission
  wait <n> [timeout_s]             wait until at least n vehicles are known
  wait-until <ids|all> <cond> [timeout_s]
                                   cond: armable, armed, disarmed, in-air, landed
  wait-alt <ids|all> <alt_m> [timeout_s]
                                   wait until relative altitude >= alt_m
  sleep <s>                        pause
  help                             this text
  quit                             exit

Default wait timeout is 60 s. Lines starting with # are comments.)";
}

void Interpreter::start()
{
    v1::Request list;
    list.mutable_list_vehicles();
    const auto response = send(list);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& info : response.vehicle_list().vehicles()) {
            vehicles_[info.id()] = info;
        }
    }
    v1::Request subscribe;
    subscribe.mutable_subscribe(); // empty = all vehicles
    send(subscribe);
}

void Interpreter::on_stream(const v1::StreamBatch& batch)
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : batch.events()) {
            if (event.has_vehicle_added()) {
                const auto& info = event.vehicle_added();
                vehicles_[info.id()] = info;
                lines.push_back(
                    "[fleet] " + info.id() + " added (system " +
                    std::to_string(info.display_system_id()) + ")" +
                    (info.system_id_conflict() ? " SYSTEM ID CONFLICT" : ""));
            } else if (event.has_vehicle_changed()) {
                const auto& info = event.vehicle_changed();
                const auto old = vehicles_[info.id()];
                vehicles_[info.id()] = info;
                if (old.connected() != info.connected()) {
                    lines.push_back(
                        "[fleet] " + info.id() + (info.connected() ? " connected" : " lost"));
                }
                if (!old.system_id_conflict() && info.system_id_conflict()) {
                    lines.push_back("[fleet] " + info.id() + " SYSTEM ID CONFLICT");
                }
            }
        }
        for (const auto& state : batch.vehicles()) {
            states_[state.id()] = state;
        }

        const auto now = Clock::now();
        if ((watch_all_ || !watched_.empty()) && now - last_watch_print_ >= kWatchPeriod) {
            last_watch_print_ = now;
            std::vector<std::string> ids;
            for (const auto& [id, state] : states_) {
                if (watch_all_ || watched_.count(id) != 0) {
                    ids.push_back(id);
                }
            }
            std::sort(ids.begin(), ids.end(), id_less);
            for (const auto& id : ids) {
                lines.push_back(format_state(states_[id]));
            }
        }
    }
    for (const auto& line : lines) {
        print_line(line);
    }
}

std::vector<v1::VehicleInfo> Interpreter::vehicles_snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<v1::VehicleInfo> out;
    for (const auto& [id, info] : vehicles_) {
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return id_less(a.id(), b.id());
    });
    return out;
}

std::map<std::string, v1::VehicleState> Interpreter::states_snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return states_;
}

Interpreter::Result Interpreter::execute(const std::string& line)
{
    cancelled_ = false;
    const auto args = tokenize(line);
    if (args.empty() || args[0][0] == '#') {
        return Result::Ok;
    }
    const auto& cmd = args[0];
    try {
        if (cmd == "quit" || cmd == "exit") {
            return Result::Quit;
        }
        if (cmd == "help") {
            print_line(help());
            return Result::Ok;
        }
        if (cmd == "vehicles") {
            return cmd_vehicles();
        }
        if (cmd == "state") {
            return cmd_state(args);
        }
        if (cmd == "watch") {
            return cmd_watch(args);
        }
        if (cmd == "unwatch") {
            std::lock_guard<std::mutex> lock(mutex_);
            watched_.clear();
            watch_all_ = false;
            return Result::Ok;
        }
        if (cmd == "arm" || cmd == "disarm" || cmd == "takeoff" || cmd == "land" ||
            cmd == "rtl") {
            return cmd_group(cmd, args);
        }
        if (cmd == "mission") {
            return cmd_mission(args);
        }
        if (cmd == "wait") {
            return cmd_wait(args);
        }
        if (cmd == "wait-until") {
            return cmd_wait_until(args);
        }
        if (cmd == "wait-alt") {
            return cmd_wait_alt(args);
        }
        if (cmd == "sleep") {
            return cmd_sleep(args);
        }
        print_error("unknown command '" + cmd + "', try 'help'");
        return Result::Failed;
    } catch (const std::exception& e) {
        print_error(cmd + ": " + e.what());
        return Result::Failed;
    }
}

v1::Response Interpreter::send(v1::Request request)
{
    auto response = client_.request(std::move(request), request_timeout_);
    if (response.has_error()) {
        throw std::runtime_error(format_error(response.error()));
    }
    return response;
}

std::vector<std::string> Interpreter::known_ids()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, info] : vehicles_) {
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), id_less);
    return ids;
}

std::vector<std::string> Interpreter::resolve_ids(const Args& args, size_t first, size_t last)
{
    std::vector<std::string> ids;
    for (size_t i = first; i < last && i < args.size(); ++i) {
        std::string token = args[i];
        std::replace(token.begin(), token.end(), ',', ' ');
        for (const auto& id : tokenize(token)) {
            if (id == "all") {
                for (const auto& known : known_ids()) {
                    ids.push_back(known);
                }
            } else {
                ids.push_back(id);
            }
        }
    }
    // Keep order, drop duplicates.
    std::vector<std::string> unique;
    for (const auto& id : ids) {
        if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
            unique.push_back(id);
        }
    }
    return unique;
}

Interpreter::Result Interpreter::cmd_vehicles()
{
    v1::Request request;
    request.mutable_list_vehicles();
    const auto response = send(request);
    std::vector<v1::VehicleInfo> list(
        response.vehicle_list().vehicles().begin(), response.vehicle_list().vehicles().end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return id_less(a.id(), b.id());
    });
    if (list.empty()) {
        print_line("no vehicles");
        return Result::Ok;
    }
    for (const auto& info : list) {
        v1::VehicleState state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            vehicles_[info.id()] = info;
            auto it = states_.find(info.id());
            if (it != states_.end()) {
                state = it->second;
            } else {
                state.set_id(info.id());
                state.set_connected(info.connected());
            }
        }
        print_line(
            format_state(state) + "  sys " + std::to_string(info.display_system_id()) +
            (info.system_id_conflict() ? "  SYSTEM ID CONFLICT" : ""));
    }
    return Result::Ok;
}

Interpreter::Result Interpreter::cmd_group(const std::string& name, const Args& args)
{
    size_t last = args.size();
    float altitude = 0.0f;
    if (name == "takeoff" && args.size() >= 3) {
        double value = 0.0;
        if (parse_number(args.back(), value)) {
            altitude = static_cast<float>(value);
            last = args.size() - 1;
        }
    }
    const auto ids = resolve_ids(args, 1, last);
    if (ids.empty()) {
        print_error(name + ": no vehicles given (or none known yet)");
        return Result::Failed;
    }

    v1::Request request;
    if (name == "arm") {
        request.mutable_arm()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
    } else if (name == "disarm") {
        request.mutable_disarm()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
    } else if (name == "takeoff") {
        request.mutable_takeoff()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
        request.mutable_takeoff()->set_altitude_m(altitude);
    } else if (name == "land") {
        request.mutable_land()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
    } else {
        request.mutable_return_to_launch()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
    }
    return report_group(send(request));
}

Interpreter::Result Interpreter::cmd_mission(const Args& args)
{
    if (args.size() >= 4 && args[1] == "upload") {
        std::ifstream in(args.back());
        if (!in) {
            print_error("cannot read " + args.back());
            return Result::Failed;
        }
        std::stringstream plan;
        plan << in.rdbuf();
        const auto ids = resolve_ids(args, 2, args.size() - 1);
        if (ids.empty()) {
            print_error("mission upload: no vehicles given (or none known yet)");
            return Result::Failed;
        }
        v1::Request request;
        request.mutable_upload_mission()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
        request.mutable_upload_mission()->set_qgc_plan(plan.str());
        return report_group(send(request));
    }
    if (args.size() >= 3 && args[1] == "start") {
        const auto ids = resolve_ids(args, 2, args.size());
        if (ids.empty()) {
            print_error("mission start: no vehicles given (or none known yet)");
            return Result::Failed;
        }
        v1::Request request;
        request.mutable_start_mission()->mutable_vehicle_ids()->Assign(ids.begin(), ids.end());
        return report_group(send(request));
    }
    print_error("usage: mission upload <ids|all> <file.plan> | mission start <ids|all>");
    return Result::Failed;
}

Interpreter::Result Interpreter::report_group(const v1::Response& response)
{
    bool all_ok = true;
    for (const auto& result : response.group().results()) {
        if (result.has_error()) {
            all_ok = false;
            print_line("  " + result.vehicle_id() + ": " + format_error(result.error()));
        } else {
            print_line("  " + result.vehicle_id() + ": ok");
        }
    }
    return all_ok ? Result::Ok : Result::Failed;
}

Interpreter::Result Interpreter::cmd_state(const Args& args)
{
    const bool json = args.size() == 3 && args[2] == "--json";
    if (args.size() != 2 && !json) {
        print_error("usage: state <id> [--json]");
        return Result::Failed;
    }
    v1::VehicleState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(args[1]);
        if (it == states_.end()) {
            print_error("no state for " + args[1]);
            return Result::Failed;
        }
        state = it->second;
    }
    if (!json) {
        print_line(format_state_detail(state));
        return Result::Ok;
    }
    // Full message, field names as in the .proto, zero values included.
    std::string text;
    google::protobuf::util::JsonPrintOptions options;
    options.add_whitespace = true;
    options.always_print_fields_with_no_presence = true;
    options.preserve_proto_field_names = true;
    (void)google::protobuf::util::MessageToJsonString(state, &text, options);
    print_line(text);
    return Result::Ok;
}

Interpreter::Result Interpreter::cmd_watch(const Args& args)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (args.size() == 1) {
        watch_all_ = true;
        return Result::Ok;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        std::string token = args[i];
        std::replace(token.begin(), token.end(), ',', ' ');
        for (const auto& id : tokenize(token)) {
            if (id == "all") {
                watch_all_ = true;
            } else {
                watched_.insert(id);
            }
        }
    }
    return Result::Ok;
}

Interpreter::Result Interpreter::cmd_wait(const Args& args)
{
    double count = 0;
    double timeout_s = kDefaultWaitS;
    if (args.size() < 2 || args.size() > 3 || !parse_number(args[1], count) ||
        (args.size() == 3 && !parse_number(args[2], timeout_s))) {
        print_error("usage: wait <n> [timeout_s]");
        return Result::Failed;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(
                                             static_cast<int64_t>(timeout_s * 1000));
    while (Clock::now() < deadline && !cancelled()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (static_cast<double>(vehicles_.size()) >= count) {
                print_line("  " + std::to_string(vehicles_.size()) + " vehicle(s) known");
                return Result::Ok;
            }
        }
        std::this_thread::sleep_for(kPollPeriod);
    }
    print_error("timed out waiting for " + args[1] + " vehicle(s)");
    return Result::Failed;
}

bool Interpreter::wait_for(
    const std::vector<std::string>& ids,
    const std::function<bool(const v1::VehicleState&)>& predicate,
    std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline && !cancelled()) {
        bool done = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& id : ids) {
                auto it = states_.find(id);
                if (it == states_.end() || !predicate(it->second)) {
                    done = false;
                    break;
                }
            }
        }
        if (done) {
            return true;
        }
        std::this_thread::sleep_for(kPollPeriod);
    }
    return false;
}

Interpreter::Result Interpreter::cmd_wait_until(const Args& args)
{
    if (args.size() < 3) {
        print_error("usage: wait-until <ids|all> <armable|armed|disarmed|in-air|landed> [timeout_s]");
        return Result::Failed;
    }
    double timeout_s = kDefaultWaitS;
    size_t cond_index = args.size() - 1;
    if (parse_number(args.back(), timeout_s)) {
        cond_index = args.size() - 2;
    } else {
        timeout_s = kDefaultWaitS;
    }
    const auto& cond = args[cond_index];
    std::function<bool(const v1::VehicleState&)> predicate;
    if (cond == "armable") {
        predicate = [](const auto& s) { return s.armable(); };
    } else if (cond == "armed") {
        predicate = [](const auto& s) { return s.armed(); };
    } else if (cond == "disarmed") {
        predicate = [](const auto& s) { return !s.armed(); };
    } else if (cond == "in-air") {
        predicate = [](const auto& s) { return s.in_air(); };
    } else if (cond == "landed") {
        predicate = [](const auto& s) { return !s.in_air(); };
    } else {
        print_error("unknown condition '" + cond + "'");
        return Result::Failed;
    }
    const auto ids = resolve_ids(args, 1, cond_index);
    if (ids.empty()) {
        print_error("wait-until: no vehicles given (or none known yet)");
        return Result::Failed;
    }
    if (!wait_for(ids, predicate,
                  std::chrono::milliseconds(static_cast<int64_t>(timeout_s * 1000)))) {
        print_error("timed out waiting for " + cond);
        if (cond == "armable") {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& id : ids) {
                auto it = states_.find(id);
                if (it == states_.end()) {
                    continue;
                }
                for (const auto& problem : it->second.arming_problems()) {
                    print_line("  " + id + ": " + problem.message());
                }
            }
        }
        return Result::Failed;
    }
    print_line("  " + cond);
    return Result::Ok;
}

Interpreter::Result Interpreter::cmd_wait_alt(const Args& args)
{
    if (args.size() < 3) {
        print_error("usage: wait-alt <ids|all> <alt_m> [timeout_s]");
        return Result::Failed;
    }
    double timeout_s = kDefaultWaitS;
    double altitude = 0;
    size_t alt_index = args.size() - 1;
    double last_two = 0;
    if (args.size() >= 4 && parse_number(args[args.size() - 2], last_two) &&
        parse_number(args.back(), timeout_s)) {
        alt_index = args.size() - 2;
    } else {
        timeout_s = kDefaultWaitS;
    }
    if (!parse_number(args[alt_index], altitude)) {
        print_error("usage: wait-alt <ids|all> <alt_m> [timeout_s]");
        return Result::Failed;
    }
    const auto ids = resolve_ids(args, 1, alt_index);
    if (ids.empty()) {
        print_error("wait-alt: no vehicles given (or none known yet)");
        return Result::Failed;
    }
    const auto target = static_cast<float>(altitude);
    if (!wait_for(
            ids,
            [target](const auto& s) { return s.position().relative_altitude_m() >= target; },
            std::chrono::milliseconds(static_cast<int64_t>(timeout_s * 1000)))) {
        print_error("timed out waiting for altitude " + args[alt_index] + " m");
        return Result::Failed;
    }
    print_line("  reached " + args[alt_index] + " m");
    return Result::Ok;
}

Interpreter::Result Interpreter::cmd_sleep(const Args& args)
{
    double seconds = 0;
    if (args.size() != 2 || !parse_number(args[1], seconds) || seconds < 0) {
        print_error("usage: sleep <s>");
        return Result::Failed;
    }
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
    while (Clock::now() < deadline) {
        if (cancelled()) {
            print_error("sleep cancelled");
            return Result::Failed;
        }
        std::this_thread::sleep_for(std::min<Clock::duration>(kPollPeriod, deadline - Clock::now()));
    }
    return Result::Ok;
}

} // namespace fctl
