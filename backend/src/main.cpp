// falconer-backend
//
// Launcher protocol (the Tauri shell, or fctl during development):
//   1. The launcher starts the backend with stdin and stdout as pipes.
//   2. It writes one line to stdin: "<client_secret_hex> <server_secret_hex>",
//      fresh random 32-byte secrets per launch.
//   3. The backend prints exactly one line to stdout:
//      "FALCONER_LISTENING port=<port>". All logging goes to stderr.
//   4. The launcher keeps stdin open. When it closes (launcher exit or crash),
//      the backend shuts down.

#include "auth.h"
#include "connection_layer.h"
#include "contract_server.h"
#include "coordinator.h"
#include "log.h"

#include <mavsdk/log_callback.h>

#include <boost/asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct Options {
    uint16_t mavlink_port = 14550;
    int stream_rate_hz = 10;
};

void print_usage()
{
    std::fprintf(
        stderr,
        "usage: falconer-backend [--mavlink-port N] [--stream-rate HZ] [--version]\n"
        "Reads per-launch secrets from stdin; see the launcher protocol in main.cpp.\n");
}

bool parse_args(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_int = [&](int& out) {
            if (i + 1 >= argc) {
                return false;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0') {
                return false;
            }
            out = static_cast<int>(value);
            return true;
        };
        if (arg == "--version") {
            std::printf("%s\n", FALCONER_VERSION);
            std::exit(0);
        } else if (arg == "--mavlink-port") {
            int port = 0;
            if (!next_int(port) || port <= 0 || port > 65535) {
                return false;
            }
            options.mavlink_port = static_cast<uint16_t>(port);
        } else if (arg == "--stream-rate") {
            if (!next_int(options.stream_rate_hz) || options.stream_rate_hz <= 0 ||
                options.stream_rate_hz > 120) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace falconer;

    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 2;
    }

    // MAVSDK logs to stdout by default; route it to stderr so stdout carries
    // only the launcher protocol.
    mavsdk::log::subscribe([](mavsdk::log::Level level, const std::string& message,
                              const std::string&, int) {
        switch (level) {
            case mavsdk::log::Level::Debug:
                break;
            case mavsdk::log::Level::Info:
                log_info("mavsdk: " + message);
                break;
            case mavsdk::log::Level::Warn:
                log_warn("mavsdk: " + message);
                break;
            case mavsdk::log::Level::Err:
                log_error("mavsdk: " + message);
                break;
        }
        return true;
    });

    std::string secrets_line;
    if (!std::getline(std::cin, secrets_line)) {
        log_error("no secrets on stdin");
        return 1;
    }
    auto secrets = parse_secrets_line(secrets_line);
    if (!secrets) {
        log_error("malformed secrets line on stdin");
        return 1;
    }

    asio::io_context io;
    // MAVSDK commands run here, never on the io thread (see VehicleAdapter).
    asio::thread_pool mavsdk_pool(4);
    Coordinator coordinator(io, mavsdk_pool);

    std::unique_ptr<ConnectionLayer> connection;
    try {
        connection = std::make_unique<ConnectionLayer>(
            io, options.mavlink_port, [&coordinator](std::shared_ptr<MavlinkStream> stream) {
                coordinator.on_new_stream(std::move(stream));
            });
    } catch (const std::exception& e) {
        log_error(
            "cannot open MAVLink UDP port " + std::to_string(options.mavlink_port) + ": " +
            e.what());
        return 1;
    }

    ContractServer server(
        io,
        coordinator,
        std::move(*secrets),
        FALCONER_VERSION,
        std::chrono::milliseconds(1000 / options.stream_rate_hz));

    auto shutdown = [&]() {
        log_info("shutting down");
        server.stop();
        connection->stop();
        io.stop();
    };

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& ec, int) {
        if (!ec) {
            shutdown();
        }
    });

    // Launcher gone means we go too.
    std::thread stdin_watch([&]() {
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        asio::post(io, [&]() {
            log_info("launcher closed stdin");
            shutdown();
        });
    });
    stdin_watch.detach();

    connection->start();
    server.start();
    log_info(std::string("falconer-backend ") + FALCONER_VERSION + " contract on 127.0.0.1:" +
             std::to_string(server.port()));
    std::printf("FALCONER_LISTENING port=%u\n", server.port());
    std::fflush(stdout);

    io.run();

    // Adapters (and their MAVSDK instances) go before the connection layer.
    mavsdk_pool.join();
    coordinator.shutdown();
    return 0;
}
