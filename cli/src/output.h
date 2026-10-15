#pragma once

#include <functional>
#include <string>

namespace fctl {

// Line output shared by the REPL, scripts and the stream thread. By default
// lines go to stdout/stderr; the dashboard installs a sink instead.
void print_line(const std::string& line);
void print_error(const std::string& line);

using OutputSink = std::function<void(const std::string& line, bool is_error)>;
void set_output_sink(OutputSink sink);

} // namespace fctl
