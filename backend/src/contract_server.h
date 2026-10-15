#pragma once

#include "auth.h"
#include "coordinator.h"

#include <boost/asio.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace falconer {

using tcp = asio::ip::tcp;

class Session;

// Serves the contract to exactly one client at a time over a WebSocket bound
// to 127.0.0.1 on an OS-assigned port. Io thread only.
class ContractServer {
public:
    ContractServer(
        asio::io_context& io,
        Coordinator& coordinator,
        SessionSecrets secrets,
        std::string version,
        std::chrono::milliseconds stream_period);

    uint16_t port() const;
    void start();
    void stop();

    Coordinator& coordinator() { return coordinator_; }
    const SessionSecrets& secrets() const { return secrets_; }
    const std::string& version() const { return version_; }

private:
    void accept();
    void schedule_tick();
    void tick();

    Coordinator& coordinator_;
    SessionSecrets secrets_;
    std::string version_;
    std::chrono::milliseconds stream_period_;
    tcp::acceptor acceptor_;
    asio::steady_timer tick_timer_;
    std::weak_ptr<Session> session_;
    bool stopped_ = false;
};

} // namespace falconer
