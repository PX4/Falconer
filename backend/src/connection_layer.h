#pragma once

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace falconer {

namespace asio = boost::asio;
using udp = asio::ip::udp;

// One vehicle's MAVLink stream: one system ID from one remote endpoint.
class MavlinkStream {
public:
    using Receiver = std::function<void(const uint8_t* data, size_t len)>;

    MavlinkStream(udp::socket& socket, udp::endpoint remote, uint8_t system_id);

    uint8_t system_id() const { return system_id_; }
    const udp::endpoint& remote() const { return remote_; }

    // Io thread only.
    void set_receiver(Receiver receiver) { receiver_ = std::move(receiver); }
    void deliver(const uint8_t* data, size_t len);

    // Any thread.
    void send(const char* data, size_t len);

private:
    udp::socket& socket_;
    udp::endpoint remote_;
    uint8_t system_id_;
    Receiver receiver_;
};

// Owns the MAVLink UDP socket and splits incoming traffic into one stream per
// (remote endpoint, system ID). A stream is created on its first heartbeat.
class ConnectionLayer {
public:
    using NewStreamCallback = std::function<void(std::shared_ptr<MavlinkStream>)>;

    ConnectionLayer(asio::io_context& io, uint16_t port, NewStreamCallback on_new_stream);

    void start();
    void stop();
    uint16_t port() const;

private:
    void receive();
    void handle_datagram(size_t len);

    udp::socket socket_;
    udp::endpoint sender_;
    std::array<uint8_t, 65536> buffer_{};
    NewStreamCallback on_new_stream_;
    std::map<std::pair<udp::endpoint, uint8_t>, std::shared_ptr<MavlinkStream>> streams_;
};

} // namespace falconer
