// fctl: Falconer terminal ground control station.
//
// Starts its own backend exactly like the Falconer app's Tauri shell does
// (secrets over stdin, port over stdout), authenticates over the contract,
// and flies vehicles from a live dashboard, an interactive shell or a
// script. Also starts and stops PX4 SITL containers.

#include "client.h"
#include "interpreter.h"
#include "launcher.h"
#include "output.h"
#include "sitl.h"
#include "top.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string backend = FCTL_DEFAULT_BACKEND;
    uint16_t mavlink_port = 14550;
    std::string data_dir = ".falconer";
};

void usage()
{
    std::cerr << R"(usage:
  fctl [options]                      interactive shell (starts the backend)
  fctl top [options]                  live dashboard with a command line
  fctl run -c "<cmd>; <cmd>" [options] run commands, stop at the first failure
  fctl run <file> [options]           run commands from a file, one per line
  fctl sitl up [n] [--model M] [--home lat,lon,alt]
                                      start n PX4 SITL (SIH) containers, default 1
  fctl sitl down                      remove all fctl SITL containers
  fctl sitl ps                        list fctl SITL containers
  fctl help                           shell commands

options:
  --backend PATH      falconer-backend binary (default: the one built with fctl)
  --mavlink-port N    UDP port the backend listens on for vehicles (default 14550)
  --data-dir DIR      backend logs and SITL ULogs (default .falconer)

SITL image: )" << FCTL_SITL_IMAGE
              << "\n";
}

std::string timestamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::vector<std::string> split_commands(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ';' || c == '\n') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

bool is_blank(const std::string& s)
{
    return s.find_first_not_of(" \t\r") == std::string::npos;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

enum class Mode { Shell, Script, Top };

int run_session(
    const Options& options, Mode mode, const std::optional<std::vector<std::string>>& script)
{
    using fctl::Interpreter;

    const auto log_dir = std::filesystem::path(options.data_dir) / "logs";
    std::filesystem::create_directories(log_dir);
    const auto log_path = (log_dir / ("backend-" + timestamp() + ".log")).string();

    std::unique_ptr<fctl::BackendProcess> backend;
    try {
        backend = std::make_unique<fctl::BackendProcess>(
            fctl::BackendProcess::Options{options.backend, options.mavlink_port, log_path});
    } catch (const std::exception& e) {
        fctl::print_error(e.what());
        return 1;
    }

    fctl::Client client;
    Interpreter interpreter(client);
    std::atomic<bool> disconnected{false};
    std::mutex refresh_mutex;
    std::function<void()> refresh;
    client.set_stream_handler([&](const fctl::v1::StreamBatch& batch) {
        interpreter.on_stream(batch);
        std::lock_guard<std::mutex> lock(refresh_mutex);
        if (refresh) {
            refresh();
        }
    });
    client.set_disconnect_handler([&](const std::string& reason) {
        disconnected = true;
        fctl::print_error(reason + " (backend log: " + log_path + ")");
    });

    try {
        client.connect(backend->port(), backend->secrets(), FALCONER_VERSION);
        interpreter.start();
    } catch (const std::exception& e) {
        fctl::print_error(std::string("cannot connect to the backend: ") + e.what());
        fctl::print_error("backend log: " + log_path);
        return 1;
    }
    if (mode != Mode::Top) {
        fctl::print_line(
            "backend " FALCONER_VERSION " on 127.0.0.1:" + std::to_string(backend->port()) +
            ", vehicles on UDP " + std::to_string(options.mavlink_port) + ", log " + log_path);
    }

    int exit_code = 0;
    if (mode == Mode::Top) {
        exit_code = fctl::run_top(
            interpreter,
            fctl::TopInfo{FALCONER_VERSION, backend->port(), options.mavlink_port, log_path},
            [&](std::function<void()> fn) {
                std::lock_guard<std::mutex> lock(refresh_mutex);
                refresh = std::move(fn);
            });
    } else if (mode == Mode::Script) {
        for (const auto& raw : *script) {
            const auto line = trim(raw);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            fctl::print_line("> " + line);
            const auto result = interpreter.execute(line);
            if (result == Interpreter::Result::Quit) {
                break;
            }
            if (result == Interpreter::Result::Failed || disconnected) {
                exit_code = 1;
                break;
            }
        }
    } else {
        const bool tty = isatty(STDIN_FILENO) != 0;
        if (tty) {
            fctl::print_line("type 'help' for commands, 'quit' or Ctrl-D to exit");
        }
        std::string line;
        while (true) {
            if (tty) {
                std::cout << "fctl> " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                break;
            }
            if (is_blank(line)) {
                continue;
            }
            if (interpreter.execute(line) == Interpreter::Result::Quit) {
                break;
            }
        }
    }

    client.close();
    backend.reset();
    return exit_code;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;
    std::optional<std::string> inline_commands;
    fctl::sitl::UpOptions sitl_options;
    sitl_options.image = FCTL_SITL_IMAGE;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        std::optional<std::string> v;
        if (arg == "--backend") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            options.backend = *v;
        } else if (arg == "--mavlink-port") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            options.mavlink_port = static_cast<uint16_t>(std::stoi(*v));
        } else if (arg == "--data-dir") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            options.data_dir = *v;
        } else if (arg == "-c") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            inline_commands = *v;
        } else if (arg == "--model") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            sitl_options.model = *v;
        } else if (arg == "--home") {
            if (!(v = value())) {
                usage();
                return 2;
            }
            sitl_options.home = *v;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) {
        return run_session(options, Mode::Shell, std::nullopt);
    }

    const auto& command = positional[0];
    if (command == "top" && positional.size() == 1) {
        return run_session(options, Mode::Top, std::nullopt);
    }
    if (command == "help") {
        std::cout << fctl::Interpreter::help() << "\n";
        return 0;
    }
    if (command == "run") {
        if (inline_commands) {
            return run_session(options, Mode::Script, split_commands(*inline_commands));
        }
        if (positional.size() != 2) {
            usage();
            return 2;
        }
        std::ifstream in(positional[1]);
        if (!in) {
            fctl::print_error("cannot read " + positional[1]);
            return 2;
        }
        std::stringstream text;
        text << in.rdbuf();
        return run_session(options, Mode::Script, split_commands(text.str()));
    }
    if (command == "sitl" && positional.size() >= 2) {
        const auto& sub = positional[1];
        if (sub == "up") {
            if (positional.size() == 3) {
                sitl_options.count = std::stoi(positional[2]);
            }
            sitl_options.data_dir = options.data_dir;
            return fctl::sitl::up(sitl_options) ? 0 : 1;
        }
        if (sub == "down") {
            return fctl::sitl::down() ? 0 : 1;
        }
        if (sub == "ps") {
            return fctl::sitl::ps() ? 0 : 1;
        }
    }
    usage();
    return 2;
}
