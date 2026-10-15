#include "client.h"

#include <stdexcept>

namespace fctl {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {
constexpr size_t kNonceBytes = 32;
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);

std::string fatal_text(const v1::Error& error)
{
    return v1::ErrorCode_Name(error.code()) + ": " + error.message();
}
} // namespace

Client::Client() :
    ws_(io_)
{}

Client::~Client()
{
    close();
}

void Client::send_sync(const v1::ClientMessage& message)
{
    ws_.write(asio::buffer(message.SerializeAsString()));
}

v1::ServerMessage Client::read_sync()
{
    beast::flat_buffer buffer;
    ws_.read(buffer);
    v1::ServerMessage message;
    const auto data = buffer.cdata();
    if (!message.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        throw std::runtime_error("unparseable message from backend");
    }
    if (message.has_fatal()) {
        throw std::runtime_error("backend refused the session: " + fatal_text(message.fatal()));
    }
    return message;
}

void Client::connect(
    uint16_t port, const falconer::SessionSecrets& secrets, const std::string& version)
{
    auto& tcp_stream = beast::get_lowest_layer(ws_);
    tcp_stream.expires_after(kHandshakeTimeout);
    tcp_stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    ws_.binary(true);
    ws_.handshake("127.0.0.1:" + std::to_string(port), "/");

    // Mutual authentication; see proto/falconer/v1/contract.proto.
    const std::string client_nonce = falconer::random_bytes(kNonceBytes);
    v1::ClientMessage hello;
    hello.mutable_hello()->set_version(version);
    hello.mutable_hello()->set_client_nonce(client_nonce);
    send_sync(hello);

    const auto ack = read_sync();
    if (!ack.has_hello_ack()) {
        throw std::runtime_error("expected hello_ack from backend");
    }
    const auto expected = falconer::hmac_sha256(secrets.server, client_nonce);
    if (!falconer::constant_time_equal(ack.hello_ack().server_proof(), expected)) {
        throw std::runtime_error("backend failed to prove its identity; refusing to talk to it");
    }

    v1::ClientMessage auth;
    auth.mutable_auth()->set_client_proof(
        falconer::hmac_sha256(secrets.client, ack.hello_ack().server_nonce()));
    send_sync(auth);
    if (!read_sync().has_auth_ok()) {
        throw std::runtime_error("expected auth_ok from backend");
    }

    tcp_stream.expires_never();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
    }
    read_loop();
    thread_ = std::thread([this]() { io_.run(); });
}

void Client::close()
{
    {
        // A deliberate close is not a disconnect worth reporting.
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }
    if (thread_.joinable()) {
        asio::post(io_, [this]() {
            beast::error_code ec;
            beast::get_lowest_layer(ws_).socket().close(ec);
        });
        thread_.join();
    }
    fail_all("client closed");
}

void Client::read_loop()
{
    buffer_.clear();
    ws_.async_read(buffer_, [this](beast::error_code ec, size_t) {
        if (ec) {
            fail_all("connection lost: " + ec.message());
            return;
        }
        v1::ServerMessage message;
        const auto data = buffer_.cdata();
        if (!message.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            fail_all("unparseable message from backend");
            return;
        }
        if (message.has_response()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(message.response().request_id());
            if (it != pending_.end()) {
                it->second.set_value(message.response());
                pending_.erase(it);
            }
        } else if (message.has_stream()) {
            if (on_stream_) {
                on_stream_(message.stream());
            }
        } else if (message.has_fatal()) {
            fail_all("backend closed the session: " + fatal_text(message.fatal()));
            return;
        }
        read_loop();
    });
}

void Client::fail_all(const std::string& reason)
{
    std::map<uint32_t, std::promise<v1::Response>> pending;
    bool was_open = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_);
        was_open = open_;
        open_ = false;
    }
    for (auto& [id, promise] : pending) {
        promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
    }
    if (was_open && on_disconnect_ && reason != "client closed") {
        on_disconnect_(reason);
    }
}

v1::Response Client::request(v1::Request request, std::chrono::milliseconds timeout)
{
    std::future<v1::Response> future;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            throw std::runtime_error("not connected to the backend");
        }
        id = next_id_++;
        request.set_request_id(id);
        future = pending_[id].get_future();
    }

    v1::ClientMessage message;
    *message.mutable_request() = std::move(request);
    asio::post(io_, [this, bytes = message.SerializeAsString()]() mutable {
        outbox_.push_back(std::move(bytes));
        if (!writing_) {
            write_next();
        }
    });

    if (future.wait_for(timeout) != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        throw std::runtime_error("request timed out");
    }
    return future.get();
}

void Client::write_next()
{
    writing_ = true;
    ws_.async_write(asio::buffer(outbox_.front()), [this](beast::error_code ec, size_t) {
        outbox_.pop_front();
        if (ec) {
            writing_ = false;
            fail_all("write failed: " + ec.message());
            return;
        }
        if (!outbox_.empty()) {
            write_next();
        } else {
            writing_ = false;
        }
    });
}

} // namespace fctl
