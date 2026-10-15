#include "contract_server.h"

#include "log.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <array>
#include <deque>

namespace falconer {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

namespace {

constexpr size_t kNonceBytes = 32;
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr size_t kMaxMessageBytes = 1 << 20;

// Origins the Tauri WebView uses, per platform. A missing Origin is allowed:
// browsers always send one, and a local non-browser client (fctl) must still
// pass the secret-based authentication.
constexpr std::array<std::string_view, 3> kAllowedOrigins = {
    "tauri://localhost",
    "http://tauri.localhost",
    "https://tauri.localhost",
};

// Answers a rejected upgrade with a plain HTTP error and closes.
void reject(tcp::socket socket, http::status status, std::string reason)
{
    struct Reject {
        beast::tcp_stream stream;
        http::response<http::string_body> response;
    };
    auto r = std::make_shared<Reject>(Reject{beast::tcp_stream(std::move(socket)), {}});
    r->response.result(status);
    r->response.set(http::field::content_type, "text/plain");
    r->response.keep_alive(false);
    r->response.body() = std::move(reason);
    r->response.prepare_payload();
    r->stream.expires_after(std::chrono::seconds(2));
    http::async_write(r->stream, r->response, [r](beast::error_code, size_t) {
        beast::error_code ec;
        r->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    });
}

} // namespace

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, ContractServer& server) :
        ws_(std::move(socket)),
        server_(server),
        handshake_timer_(ws_.get_executor())
    {}

    void start()
    {
        beast::get_lowest_layer(ws_).expires_after(kHandshakeTimeout);
        http::async_read(
            ws_.next_layer(),
            buffer_,
            request_,
            [self = shared_from_this()](beast::error_code ec, size_t) { self->on_http(ec); });
    }

    bool ready() const { return state_ == State::Ready; }
    bool subscribed() const { return subscribed_; }
    const std::vector<std::string>& subscription() const { return subscription_; }

    void send(const v1::ServerMessage& message)
    {
        if (state_ == State::Closed) {
            return;
        }
        outbox_.push_back(message.SerializeAsString());
        if (!writing_) {
            write_next();
        }
    }

private:
    enum class State { AwaitHello, AwaitAuth, Ready, Closing, Closed };

    void on_http(beast::error_code ec)
    {
        if (ec) {
            log_warn("contract: bad upgrade request: " + ec.message());
            state_ = State::Closed;
            return;
        }
        const auto origin = request_[http::field::origin];
        if (!server_.origin_allowed(origin)) {
            log_warn("contract: rejected connection from origin " + std::string(origin));
            state_ = State::Closed;
            reject(
                std::move(beast::get_lowest_layer(ws_).socket()),
                http::status::forbidden,
                "origin not allowed");
            return;
        }
        if (!websocket::is_upgrade(request_)) {
            state_ = State::Closed;
            reject(
                std::move(beast::get_lowest_layer(ws_).socket()),
                http::status::bad_request,
                "websocket upgrade required");
            return;
        }

        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.read_message_max(kMaxMessageBytes);
        ws_.binary(true);
        ws_.async_accept(request_, [self = shared_from_this()](beast::error_code ec) {
            self->on_accept(ec);
        });
    }

    void on_accept(beast::error_code ec)
    {
        if (ec) {
            log_warn("contract: websocket accept failed: " + ec.message());
            state_ = State::Closed;
            return;
        }
        log_info("contract: client connected, awaiting handshake");
        handshake_timer_.expires_after(kHandshakeTimeout);
        handshake_timer_.async_wait([weak = weak_from_this()](beast::error_code ec) {
            auto self = weak.lock();
            if (!ec && self && !self->ready()) {
                self->fail(v1::ERROR_CODE_AUTH_FAILED, "handshake timed out");
            }
        });
        read();
    }

    void read()
    {
        buffer_.clear();
        ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, size_t) {
            self->on_read(ec);
        });
    }

    void on_read(beast::error_code ec)
    {
        if (ec) {
            if (state_ != State::Closed) {
                log_info("contract: client disconnected (" + ec.message() + ")");
            }
            state_ = State::Closed;
            handshake_timer_.cancel();
            return;
        }
        if (state_ == State::Closing || state_ == State::Closed) {
            return;
        }

        v1::ClientMessage message;
        const auto data = buffer_.cdata();
        if (!message.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            fail(v1::ERROR_CODE_INVALID_REQUEST, "unparseable message");
            return;
        }
        handle(message);
        if (state_ != State::Closing && state_ != State::Closed) {
            read();
        }
    }

    void handle(const v1::ClientMessage& message)
    {
        switch (state_) {
            case State::AwaitHello:
                if (!message.has_hello()) {
                    fail(v1::ERROR_CODE_AUTH_FAILED, "expected hello");
                    return;
                }
                on_hello(message.hello());
                return;
            case State::AwaitAuth:
                if (!message.has_auth()) {
                    fail(v1::ERROR_CODE_AUTH_FAILED, "expected auth");
                    return;
                }
                on_auth(message.auth());
                return;
            case State::Ready:
                if (!message.has_request()) {
                    fail(v1::ERROR_CODE_INVALID_REQUEST, "expected request");
                    return;
                }
                on_request(message.request());
                return;
            default:
                return;
        }
    }

    void on_hello(const v1::Hello& hello)
    {
        if (hello.version() != server_.version()) {
            fail(
                v1::ERROR_CODE_VERSION_MISMATCH,
                "version mismatch: backend " + server_.version() + ", client " + hello.version());
            return;
        }
        if (hello.client_nonce().size() != kNonceBytes) {
            fail(v1::ERROR_CODE_AUTH_FAILED, "bad client nonce");
            return;
        }
        server_nonce_ = random_bytes(kNonceBytes);

        v1::ServerMessage reply;
        auto* ack = reply.mutable_hello_ack();
        ack->set_version(server_.version());
        ack->set_server_nonce(server_nonce_);
        ack->set_server_proof(hmac_sha256(server_.secrets().server, hello.client_nonce()));
        state_ = State::AwaitAuth;
        send(reply);
    }

    void on_auth(const v1::Auth& auth)
    {
        const auto expected = hmac_sha256(server_.secrets().client, server_nonce_);
        if (!constant_time_equal(auth.client_proof(), expected)) {
            fail(v1::ERROR_CODE_AUTH_FAILED, "client proof rejected");
            return;
        }
        handshake_timer_.cancel();
        state_ = State::Ready;
        // A new session rebuilds its view from list_vehicles; earlier events
        // are not replayed.
        server_.coordinator().clear_events();
        log_info("contract: session authenticated");

        v1::ServerMessage reply;
        reply.mutable_auth_ok();
        send(reply);
    }

    void on_request(const v1::Request& request)
    {
        const auto request_id = request.request_id();
        v1::ServerMessage reply;
        auto* response = reply.mutable_response();
        response->set_request_id(request_id);

        switch (request.cmd_case()) {
            case v1::Request::kListVehicles:
                *response->mutable_vehicle_list() = server_.coordinator().vehicle_list();
                send(reply);
                return;
            case v1::Request::kSubscribe:
                subscribed_ = true;
                subscription_.assign(
                    request.subscribe().vehicle_ids().begin(),
                    request.subscribe().vehicle_ids().end());
                response->mutable_ok();
                send(reply);
                return;
            case v1::Request::kUnsubscribe:
                subscribed_ = false;
                subscription_.clear();
                response->mutable_ok();
                send(reply);
                return;
            case v1::Request::CMD_NOT_SET:
                response->mutable_error()->set_code(v1::ERROR_CODE_INVALID_REQUEST);
                response->mutable_error()->set_message("empty request");
                send(reply);
                return;
            default:
                server_.coordinator().execute(
                    request, [weak = weak_from_this()](v1::Response response) {
                        if (auto self = weak.lock()) {
                            v1::ServerMessage reply;
                            *reply.mutable_response() = std::move(response);
                            self->send(reply);
                        }
                    });
                return;
        }
    }

    void fail(v1::ErrorCode code, const std::string& message)
    {
        if (state_ == State::Closing || state_ == State::Closed) {
            return;
        }
        log_warn("contract: closing session: " + message);
        v1::ServerMessage reply;
        reply.mutable_fatal()->set_code(code);
        reply.mutable_fatal()->set_message(message);
        send(reply);
        state_ = State::Closing;
        handshake_timer_.cancel();
        if (!writing_) {
            close();
        }
    }

    void write_next()
    {
        writing_ = true;
        ws_.async_write(
            asio::buffer(outbox_.front()),
            [self = shared_from_this()](beast::error_code ec, size_t) {
                self->outbox_.pop_front();
                if (ec) {
                    self->state_ = State::Closed;
                    self->writing_ = false;
                    return;
                }
                if (!self->outbox_.empty()) {
                    self->write_next();
                    return;
                }
                self->writing_ = false;
                if (self->state_ == State::Closing) {
                    self->close();
                }
            });
    }

    void close()
    {
        ws_.async_close(
            websocket::close_code::policy_error,
            [self = shared_from_this()](beast::error_code) { self->state_ = State::Closed; });
    }

    websocket::stream<beast::tcp_stream> ws_;
    ContractServer& server_;
    asio::steady_timer handshake_timer_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    std::deque<std::string> outbox_;
    bool writing_ = false;
    State state_ = State::AwaitHello;
    std::string server_nonce_;
    bool subscribed_ = false;
    std::vector<std::string> subscription_;
};

ContractServer::ContractServer(
    asio::io_context& io,
    Coordinator& coordinator,
    SessionSecrets secrets,
    std::string version,
    std::chrono::milliseconds stream_period,
    std::vector<std::string> extra_origins) :
    coordinator_(coordinator),
    secrets_(std::move(secrets)),
    version_(std::move(version)),
    stream_period_(stream_period),
    extra_origins_(std::move(extra_origins)),
    acceptor_(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
    tick_timer_(io)
{}

bool ContractServer::origin_allowed(std::string_view origin) const
{
    if (origin.empty()) {
        return true;
    }
    for (const auto allowed : kAllowedOrigins) {
        if (origin == allowed) {
            return true;
        }
    }
    for (const auto& allowed : extra_origins_) {
        if (origin == allowed) {
            return true;
        }
    }
    return false;
}

uint16_t ContractServer::port() const
{
    return acceptor_.local_endpoint().port();
}

void ContractServer::start()
{
    accept();
    schedule_tick();
}

void ContractServer::stop()
{
    stopped_ = true;
    beast::error_code ec;
    acceptor_.close(ec);
    tick_timer_.cancel();
}

void ContractServer::accept()
{
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (stopped_) {
            return;
        }
        if (!ec) {
            if (session_.lock()) {
                log_warn("contract: rejected second client, a session is active");
                reject(std::move(socket), http::status::conflict, "a session is already active");
            } else {
                auto session = std::make_shared<Session>(std::move(socket), *this);
                session_ = session;
                session->start();
            }
        }
        accept();
    });
}

void ContractServer::schedule_tick()
{
    tick_timer_.expires_after(stream_period_);
    tick_timer_.async_wait([this](beast::error_code ec) {
        if (ec || stopped_) {
            return;
        }
        tick();
        schedule_tick();
    });
}

void ContractServer::tick()
{
    auto session = session_.lock();
    if (!session || !session->ready()) {
        coordinator_.clear_events();
        return;
    }

    v1::ServerMessage message;
    auto* batch = message.mutable_stream();
    for (auto& event : coordinator_.take_events()) {
        *batch->add_events() = std::move(event);
    }
    if (session->subscribed()) {
        for (auto& state : coordinator_.states(session->subscription())) {
            *batch->add_vehicles() = std::move(state);
        }
    }
    if (batch->events_size() > 0 || batch->vehicles_size() > 0) {
        session->send(message);
    }
}

} // namespace falconer
