#include "sitl.h"

#include "output.h"

#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <sstream>
#include <vector>

namespace fctl::sitl {

namespace {

constexpr const char* kLabel = "falconer.sitl=1";
constexpr const char* kNamePrefix = "falconer-sitl-";
// With -i N, PX4 runs (and logs) in rootfs/N.
constexpr const char* kContainerRootfs = "/root/.local/share/px4/rootfs/";

// Arguments are validated before they reach the shell, so plain quoting is
// enough.
std::string quote(const std::string& s)
{
    return "'" + s + "'";
}

int run(const std::string& command)
{
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    return WEXITSTATUS(status);
}

} // namespace

bool up(const UpOptions& options)
{
    if (options.count < 1 || options.count > 10) {
        // Beyond 10 instances PX4 collapses the offboard port (px4-rc.mavlink).
        print_error("sitl up: count must be 1 to 10");
        return false;
    }
    if (!std::regex_match(options.model, std::regex("[a-z0-9_]+"))) {
        print_error("sitl up: bad model name " + options.model);
        return false;
    }
    std::string lat, lon, alt;
    if (options.home) {
        std::smatch m;
        const std::regex number_triplet(
            R"(^(-?[0-9]+(?:\.[0-9]+)?),(-?[0-9]+(?:\.[0-9]+)?),(-?[0-9]+(?:\.[0-9]+)?)$)");
        if (!std::regex_match(*options.home, m, number_triplet)) {
            print_error("sitl up: --home must be lat,lon,alt");
            return false;
        }
        lat = m[1];
        lon = m[2];
        alt = m[3];
    }

#if defined(__linux__)
    const std::string network = " --network host";
#else
    // Docker Desktop: the image's entrypoint sends MAVLink to
    // host.docker.internal, which reaches the backend on the host.
    const std::string network;
#endif

    for (int i = 0; i < options.count; ++i) {
        const std::string name = kNamePrefix + std::to_string(i);
        const auto log_dir = std::filesystem::absolute(
            std::filesystem::path(options.data_dir) / "sitl" / ("instance-" + std::to_string(i)));
        std::filesystem::create_directories(log_dir);

        std::ostringstream cmd;
        cmd << "docker run -d --rm --name " << quote(name) << " --label " << kLabel << network
            << " -e PX4_SIM_MODEL=" << quote(options.model);
        if (options.home) {
            cmd << " -e PX4_HOME_LAT=" << lat << " -e PX4_HOME_LON=" << lon
                << " -e PX4_HOME_ALT=" << alt;
        }
        cmd << " -v " << quote(log_dir.string() + ":" + kContainerRootfs + std::to_string(i) + "/log") << " "
            << quote(options.image) << " -i " << i << " -d > /dev/null"; // -d: no pxh shell, it spins without a tty

        print_line("starting " + name + " (instance " + std::to_string(i) + ", system " +
                   std::to_string(i + 1) + ")");
        if (run(cmd.str()) != 0) {
            print_error("docker run failed for " + name);
            return false;
        }
    }
    print_line("ULogs: " + std::filesystem::absolute(options.data_dir).string() + "/sitl/");
    return true;
}

bool down()
{
    const std::string cmd = std::string("ids=$(docker ps -aq --filter label=") + kLabel +
                            "); [ -z \"$ids\" ] || docker rm -f $ids > /dev/null";
    if (run(cmd) != 0) {
        print_error("docker rm failed");
        return false;
    }
    print_line("SITL containers removed");
    return true;
}

bool ps()
{
    const std::string cmd = std::string("docker ps -a --filter label=") + kLabel +
                            " --format 'table {{.Names}}\\t{{.Status}}\\t{{.Image}}'";
    return run(cmd) == 0;
}

} // namespace fctl::sitl
