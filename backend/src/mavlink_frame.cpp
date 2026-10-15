#include "mavlink_frame.h"

namespace falconer {

namespace {
constexpr uint8_t kMagicV1 = 0xFE;
constexpr uint8_t kMagicV2 = 0xFD;
constexpr size_t kHeaderV1 = 6;
constexpr size_t kHeaderV2 = 10;
constexpr size_t kChecksum = 2;
constexpr size_t kSignature = 13;
constexpr uint8_t kIncompatSigned = 0x01;
} // namespace

void split_mavlink_frames(
    const uint8_t* data,
    size_t len,
    const std::function<void(const uint8_t*, size_t, FrameInfo)>& on_frame)
{
    size_t i = 0;
    while (i < len) {
        const size_t remaining = len - i;
        const uint8_t* p = data + i;
        size_t frame_len = 0;
        FrameInfo info{};

        if (p[0] == kMagicV2) {
            if (remaining < kHeaderV2) {
                return;
            }
            const bool is_signed = (p[2] & kIncompatSigned) != 0;
            frame_len = kHeaderV2 + p[1] + kChecksum + (is_signed ? kSignature : 0);
            info.system_id = p[5];
            info.is_heartbeat = p[7] == 0 && p[8] == 0 && p[9] == 0;
        } else if (p[0] == kMagicV1) {
            if (remaining < kHeaderV1) {
                return;
            }
            frame_len = kHeaderV1 + p[1] + kChecksum;
            info.system_id = p[3];
            info.is_heartbeat = p[5] == 0;
        } else {
            ++i;
            continue;
        }

        if (remaining < frame_len) {
            return;
        }
        on_frame(p, frame_len, info);
        i += frame_len;
    }
}

} // namespace falconer
