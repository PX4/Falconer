#include "output.h"

#include <cstdio>
#include <mutex>

namespace fctl {

namespace {
std::mutex g_mutex;
OutputSink g_sink;

void emit(const std::string& line, bool is_error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(line, is_error);
        return;
    }
    if (is_error) {
        std::fprintf(stderr, "error: %s\n", line.c_str());
        std::fflush(stderr);
    } else {
        std::fprintf(stdout, "%s\n", line.c_str());
        std::fflush(stdout);
    }
}
} // namespace

void print_line(const std::string& line)
{
    emit(line, false);
}

void print_error(const std::string& line)
{
    emit(line, true);
}

void set_output_sink(OutputSink sink)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

} // namespace fctl
