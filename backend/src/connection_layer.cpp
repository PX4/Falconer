#include "connection_layer.h"

#include "log.h"
#include "mavlink_frame.h"

#include <string>
#include <vector>

namespace falconer {

MavlinkStream::MavlinkStream(udp::socket& socket, udp::endpoint remote, uint8_t system_id) :
    socket_(socket),
    remote_(std::move(remote)),
    system_id_(system_id)
{}

void MavlinkStream::deliver(const uint8_t* data, size_t len)
{
    if (receiver_) {
        receiver_(data, len);
    }
}

void MavlinkStream::send(const char* data, size_t len)
{
    asio::post(
        socket_.get_executor(),
        [this, bytes = std::vector<char>(data, data + len)]() {
            boost::system::error_code ec;
            socket_.send_to(asio::buffer(bytes), remote_, 0, ec);
            if (ec) {
                log_warn("MAVLink send to " + remote_.address().to_string() + ":" +
                         std::to_string(remote_.port()) + " failed: " + ec.message());
            }
        });
}

ConnectionLayer::ConnectionLayer(
    asio::io_context& io, uint16_t port, NewStreamCallback on_new_stream) :
    socket_(io, udp::endpoint(udp::v4(), port)),
    on_new_stream_(std::move(on_new_stream))
{}

void ConnectionLayer::start()
{
    log_info("MAVLink listening on UDP port " + std::to_string(port()));
    receive();
}

void ConnectionLayer::stop()
{
    boost::system::error_code ec;
    socket_.close(ec);
}

uint16_t ConnectionLayer::port() const
{
    return socket_.local_endpoint().port();
}

void ConnectionLayer::receive()
{
    socket_.async_receive_from(
        asio::buffer(buffer_), sender_, [this](boost::system::error_code ec, size_t len) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            if (!ec) {
                handle_datagram(len);
            }
            receive();
        });
}

void ConnectionLayer::handle_datagram(size_t len)
{
    split_mavlink_frames(
        buffer_.data(), len, [this](const uint8_t* frame, size_t frame_len, FrameInfo info) {
            if (info.system_id == 0) {
                return;
            }
            const auto key = std::make_pair(sender_, info.system_id);
            auto it = streams_.find(key);
            if (it == streams_.end()) {
                // Only a heartbeat opens a stream, so stray bytes never
                // create a vehicle.
                if (!info.is_heartbeat) {
                    return;
                }
                auto stream = std::make_shared<MavlinkStream>(socket_, sender_, info.system_id);
                it = streams_.emplace(key, stream).first;
                log_info(
                    "new MAVLink stream: system " + std::to_string(info.system_id) + " from " +
                    sender_.address().to_string() + ":" + std::to_string(sender_.port()));
                on_new_stream_(stream);
            }
            it->second->deliver(frame, frame_len);
        });
}

} // namespace falconer
