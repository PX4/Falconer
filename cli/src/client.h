#pragma once

#include "auth.h"

#include "falconer/v1/contract.pb.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace fctl {

namespace v1 = falconer::v1;

// Contract client. Connect and authenticate synchronously, then a background
// io thread reads responses and stream batches.
class Client {
public:
    using StreamHandler = std::function<void(const v1::StreamBatch&)>;
    using DisconnectHandler = std::function<void(const std::string& reason)>;

    Client();
    ~Client();

    // Throws std::runtime_error on connection, version or auth failure.
    void connect(uint16_t port, const falconer::SessionSecrets& secrets, const std::string& version);
    void close();

    // Handlers run on the io thread. Set before connect().
    void set_stream_handler(StreamHandler handler) { on_stream_ = std::move(handler); }
    void set_disconnect_handler(DisconnectHandler handler) { on_disconnect_ = std::move(handler); }

    // Blocking. Assigns the request ID. Throws std::runtime_error on timeout
    // or disconnect.
    v1::Response request(v1::Request request, std::chrono::milliseconds timeout);

private:
    void send_sync(const v1::ClientMessage& message);
    v1::ServerMessage read_sync();
    void read_loop();
    void write_next();
    void fail_all(const std::string& reason);

    boost::asio::io_context io_;
    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;
    std::thread thread_;
    std::deque<std::string> outbox_;
    bool writing_ = false;

    std::mutex mutex_;
    uint32_t next_id_ = 1;
    std::map<uint32_t, std::promise<v1::Response>> pending_;
    bool open_ = false;

    StreamHandler on_stream_;
    DisconnectHandler on_disconnect_;
};

} // namespace fctl
