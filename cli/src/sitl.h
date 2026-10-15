#pragma once

#include <optional>
#include <string>

namespace fctl::sitl {

struct UpOptions {
    int count = 1;
    std::string model = "sihsim_quadx";
    std::string image;          // pinned image, from sitl/image at build time
    std::string data_dir;       // ULogs land in <data_dir>/sitl/instance-<i>/
    std::optional<std::string> home; // "lat,lon,alt"
};

// Starts one PX4 SITL (SIH) container per vehicle, instance i = 0..count-1,
// so vehicle i gets MAV_SYS_ID i+1. Returns false on failure.
bool up(const UpOptions& options);

// Removes every container started by fctl.
bool down();

// Lists containers started by fctl.
bool ps();

} // namespace fctl::sitl
