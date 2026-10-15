#include "log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace falconer {

namespace {
std::mutex g_mutex;

void write(const char* level, const std::string& message)
{
    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%s.%03d %s %s\n", stamp, static_cast<int>(ms), level, message.c_str());
}
} // namespace

void log_debug(const std::string& message)
{
    write("DEBUG", message);
}

void log_info(const std::string& message)
{
    write("INFO ", message);
}

void log_warn(const std::string& message)
{
    write("WARN ", message);
}

void log_error(const std::string& message)
{
    write("ERROR", message);
}

} // namespace falconer
