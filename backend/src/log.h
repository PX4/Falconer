#pragma once

#include <string>

namespace falconer {

// All backend logging goes to stderr. Stdout is reserved for the launcher
// protocol (the listening port line).
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

} // namespace falconer
