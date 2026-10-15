#pragma once

#include "auth.h"

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace fctl {

// Starts falconer-backend the way the Tauri shell will: fresh secrets over
// stdin, the contract port read from stdout, stderr to a log file. The
// backend exits when this object closes its stdin.
class BackendProcess {
public:
    struct Options {
        std::string binary;
        uint16_t mavlink_port = 14550;
        std::string log_path;
    };

    // Throws std::runtime_error if the backend fails to start.
    explicit BackendProcess(const Options& options);
    ~BackendProcess();

    BackendProcess(const BackendProcess&) = delete;
    BackendProcess& operator=(const BackendProcess&) = delete;

    uint16_t port() const { return port_; }
    const falconer::SessionSecrets& secrets() const { return secrets_; }
    const std::string& log_path() const { return log_path_; }

private:
    void stop();

    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    uint16_t port_ = 0;
    falconer::SessionSecrets secrets_;
    std::string log_path_;
};

} // namespace fctl
