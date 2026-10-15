#include "top.h"

#include "output.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

namespace fctl {

using namespace ftxui;

namespace {

constexpr size_t kMaxLogLines = 1000;

struct LogLine {
    std::string text;
    bool is_error;
};

std::string clock_text()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

std::string fmt(const char* format, double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return buf;
}

Element cell(const std::string& s, int width)
{
    return text(s) | size(WIDTH, EQUAL, width);
}

// Column widths for the fleet table.
constexpr int kId = 5, kSys = 5, kLink = 6, kArmed = 7, kAir = 8, kMode = 17, kAlt = 10,
              kSpeed = 9, kBat = 6, kGps = 12, kReady = 6;

Element header_row()
{
    return hbox({
               cell("ID", kId),
               cell("SYS", kSys),
               cell("LINK", kLink),
               cell("ARMED", kArmed),
               cell("AIR", kAir),
               cell("MODE", kMode),
               cell("ALT rel", kAlt),
               cell("SPD m/s", kSpeed),
               cell("BAT", kBat),
               cell("GPS", kGps),
               cell("READY", kReady),
           }) |
           bold;
}

Element vehicle_row(const v1::VehicleInfo& info, const v1::VehicleState* s, bool selected)
{
    Elements cells;
    cells.push_back(cell(info.id(), kId));
    cells.push_back(
        cell(std::to_string(info.display_system_id()), kSys) |
        (info.system_id_conflict() ? color(Color::Red) : nothing));
    cells.push_back(
        info.connected() ? cell("ok", kLink) | color(Color::Green)
                         : cell("LOST", kLink) | color(Color::Red) | bold);

    if (s == nullptr) {
        cells.push_back(text("waiting for telemetry") | dim);
    } else {
        cells.push_back(
            s->armed() ? cell("ARMED", kArmed) | color(Color::Yellow) | bold
                       : cell("no", kArmed));
        cells.push_back(
            s->in_air() ? cell("in-air", kAir) | color(Color::Cyan) : cell("ground", kAir));
        cells.push_back(cell(s->flight_mode(), kMode));
        cells.push_back(cell(fmt("%.1f m", s->position().relative_altitude_m()), kAlt));
        const double speed = std::hypot(s->velocity().north_m_s(), s->velocity().east_m_s());
        cells.push_back(cell(fmt("%.1f", speed), kSpeed));
        const float bat = s->battery().remaining_percent();
        cells.push_back(
            cell(fmt("%.0f%%", bat), kBat) |
            (bat < 20 ? color(Color::Red) : bat < 40 ? color(Color::Yellow) : nothing));
        const bool good_fix = s->gps().fix_type().find("3D") != std::string::npos ||
                              s->gps().fix_type().find("Rtk") != std::string::npos;
        cells.push_back(
            cell(s->gps().fix_type() + "/" + std::to_string(s->gps().num_satellites()), kGps) |
            (good_fix ? nothing : color(Color::Yellow)));
        cells.push_back(
            s->armable() ? cell("yes", kReady) | color(Color::Green)
                         : cell("no", kReady) | color(Color::Red));
    }
    auto row = hbox(std::move(cells));
    return selected ? row | inverted | focus : row;
}

Element detail_panel(const v1::VehicleInfo& info, const v1::VehicleState* s)
{
    if (s == nullptr) {
        return text(info.id() + ": no telemetry yet") | dim;
    }
    const auto& p = s->position();
    const auto& a = s->attitude();
    const auto& v = s->velocity();
    Elements lines;
    lines.push_back(hbox({
        text(info.id()) | bold,
        text("  " + s->landed_state()),
        info.system_id_conflict() ? text("  SYSTEM ID CONFLICT") | color(Color::Red) | bold
                                  : text(""),
    }));
    char buf[256];
    std::snprintf(buf, sizeof(buf), "pos   %.7f, %.7f   AMSL %.1f m   rel %.2f m",
                  p.latitude_deg(), p.longitude_deg(), p.absolute_altitude_m(),
                  p.relative_altitude_m());
    lines.push_back(text(buf));
    std::snprintf(buf, sizeof(buf), "att   roll %6.1f   pitch %6.1f   yaw %6.1f deg",
                  a.roll_deg(), a.pitch_deg(), a.yaw_deg());
    lines.push_back(text(buf));
    std::snprintf(buf, sizeof(buf), "vel   N %6.2f   E %6.2f   D %6.2f m/s", v.north_m_s(),
                  v.east_m_s(), v.down_m_s());
    lines.push_back(text(buf));
    std::snprintf(buf, sizeof(buf), "bat   %.0f%%   %.2f V", s->battery().remaining_percent(),
                  s->battery().voltage_v());
    lines.push_back(text(buf));
    if (s->mission().total() > 0) {
        std::snprintf(buf, sizeof(buf), "mission   item %d of %d", s->mission().current(),
                      s->mission().total());
        lines.push_back(text(buf));
    }

    if (s->arming_problems_size() == 0) {
        lines.push_back(text("arming checks: no problems reported") | color(Color::Green));
    } else {
        // PX4 also reports warnings that do not block arming; the vehicle's
        // armable flag says which case this is.
        lines.push_back(
            s->armable() ? text("arming checks: armable, with warnings") | color(Color::Yellow)
                         : text("arming checks: NOT armable") | color(Color::Red) | bold);
        for (const auto& problem : s->arming_problems()) {
            std::string line = "  " + problem.severity() + ": " + problem.message();
            if (!problem.component().empty()) {
                line += "  [" + problem.component() + "]";
            }
            const auto& sev = problem.severity();
            const bool severe =
                sev == "Emergency" || sev == "Alert" || sev == "Critical" || sev == "Error";
            lines.push_back(text(line) | color(severe ? Color::Red : Color::Yellow));
        }
    }
    return vbox(std::move(lines));
}

} // namespace

int run_top(
    Interpreter& interpreter,
    const TopInfo& info,
    const std::function<void(std::function<void()>)>& set_refresh)
{
    auto screen = App::Fullscreen();

    std::mutex log_mutex;
    std::deque<LogLine> log;
    auto append_log = [&](const std::string& line, bool is_error) {
        std::istringstream in(line);
        std::string part;
        std::lock_guard<std::mutex> lock(log_mutex);
        bool first = true;
        while (std::getline(in, part)) {
            log.push_back({(first ? clock_text() + " " : std::string(9, ' ')) + part, is_error});
            first = false;
        }
        while (log.size() > kMaxLogLines) {
            log.pop_front();
        }
    };
    set_output_sink([&](const std::string& line, bool is_error) {
        append_log(is_error ? "error: " + line : line, is_error);
        screen.PostEvent(Event::Custom);
    });
    set_refresh([&screen]() { screen.PostEvent(Event::Custom); });

    // Commands run on a worker so a long "wait" never freezes the screen.
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::string> queue;
    std::string running;
    bool stopping = false;
    std::thread worker([&]() {
        while (true) {
            std::string command;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&]() { return stopping || !queue.empty(); });
                if (stopping) {
                    return;
                }
                command = queue.front();
                queue.pop_front();
                running = command;
            }
            screen.PostEvent(Event::Custom);
            const auto result = interpreter.execute(command);
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                running.clear();
            }
            if (result == Interpreter::Result::Quit) {
                screen.Post(screen.ExitLoopClosure());
                return;
            }
            screen.PostEvent(Event::Custom);
        }
    });

    std::string command;
    int selected = 0;
    InputOption input_option;
    input_option.multiline = false;
    input_option.placeholder = "command (help, arm all, takeoff v1 10, ...)   Esc cancels   Ctrl-C quits";
    input_option.on_enter = [&]() {
        if (command.find_first_not_of(" \t") == std::string::npos) {
            return;
        }
        print_line("> " + command);
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push_back(command);
        }
        queue_cv.notify_one();
        command.clear();
    };
    auto input = Input(&command, input_option);

    auto renderer = Renderer(input, [&]() {
        const auto vehicles = interpreter.vehicles_snapshot();
        const auto states = interpreter.states_snapshot();
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(vehicles.size()) - 1));

        auto header = hbox({
            text(" Falconer " + info.version) | bold,
            text("   contract 127.0.0.1:" + std::to_string(info.contract_port)),
            text("   MAVLink UDP " + std::to_string(info.mavlink_port)),
            text("   " + std::to_string(vehicles.size()) + " vehicle(s)"),
            filler(),
            text(clock_text() + " "),
        });

        Elements rows;
        rows.push_back(header_row());
        for (size_t i = 0; i < vehicles.size(); ++i) {
            auto it = states.find(vehicles[i].id());
            rows.push_back(vehicle_row(
                vehicles[i], it == states.end() ? nullptr : &it->second,
                static_cast<int>(i) == selected));
        }
        if (vehicles.empty()) {
            rows.push_back(
                text("waiting for vehicles on UDP " + std::to_string(info.mavlink_port) +
                     "   (start some with: fctl sitl up N)") |
                dim);
        }

        Element detail = text("");
        if (!vehicles.empty()) {
            const auto& sel = vehicles[static_cast<size_t>(selected)];
            auto it = states.find(sel.id());
            detail = detail_panel(sel, it == states.end() ? nullptr : &it->second);
        }

        Elements log_lines;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            for (const auto& line : log) {
                log_lines.push_back(
                    line.is_error ? text(line.text) | color(Color::Red) : text(line.text));
            }
        }
        if (!log_lines.empty()) {
            log_lines.back() = log_lines.back() | focus;
        }

        std::string status;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!running.empty()) {
                status = "running: " + running + (queue.empty() ? "" : "  (+" +
                         std::to_string(queue.size()) + " queued)");
            }
        }

        return vbox({
            header | inverted,
            window(text(" fleet  (Up/Down select) "), vbox(std::move(rows)) | yframe) |
                size(HEIGHT, LESS_THAN, 16),
            window(text(" vehicle "), detail),
            window(text(" events "), vbox(std::move(log_lines)) | yframe | flex),
            hbox({text("> ") | bold, input->Render() | flex}),
            text(status.empty() ? "log: " + info.log_path : status) | dim,
        });
    });

    auto app = CatchEvent(renderer, [&](Event event) {
        if (event == Event::ArrowUp) {
            selected = std::max(0, selected - 1);
            return true;
        }
        if (event == Event::ArrowDown) {
            ++selected;
            return true;
        }
        if (event == Event::Escape) {
            interpreter.cancel();
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.clear();
            }
            return true;
        }
        return false;
    });

    // Repaint the clock even when nothing else happens.
    std::atomic<bool> ticking{true};
    std::thread ticker([&]() {
        while (ticking) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            screen.PostEvent(Event::Custom);
        }
    });

    print_line("dashboard started; backend log " + info.log_path);
    screen.Loop(app);

    ticking = false;
    interpreter.cancel();
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stopping = true;
    }
    queue_cv.notify_all();
    worker.join();
    ticker.join();
    set_refresh(nullptr);
    set_output_sink(nullptr);
    return 0;
}

} // namespace fctl
