#pragma once

#include "interpreter.h"

#include <cstdint>
#include <functional>
#include <string>

namespace fctl {

struct TopInfo {
    std::string version;
    uint16_t contract_port = 0;
    uint16_t mavlink_port = 0;
    std::string log_path;
};

// Full-screen live dashboard: fleet table, selected vehicle detail, event log
// and a command line. Blocks until the user quits. `set_refresh` receives a
// function the caller must call whenever new data arrives (any thread).
int run_top(
    Interpreter& interpreter,
    const TopInfo& info,
    const std::function<void(std::function<void()>)>& set_refresh);

} // namespace fctl
