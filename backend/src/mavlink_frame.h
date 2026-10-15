#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace falconer {

struct FrameInfo {
    uint8_t system_id;
    bool is_heartbeat;
};

// Splits a buffer of MAVLink v1/v2 bytes into whole frames without decoding
// payloads or checking CRCs (MAVSDK does that). Bytes that are not a frame
// start are skipped; a truncated frame at the end is dropped.
void split_mavlink_frames(
    const uint8_t* data,
    size_t len,
    const std::function<void(const uint8_t* frame, size_t frame_len, FrameInfo info)>& on_frame);

} // namespace falconer
