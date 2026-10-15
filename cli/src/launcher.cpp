#include "launcher.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

extern char** environ;

namespace fctl {

namespace {

constexpr size_t kSecretBytes = 32;
constexpr int kStartupTimeoutMs = 10000;
constexpr const char* kListeningPrefix = "FALCONER_LISTENING port=";

std::string tail_of(const std::string& path, size_t max_lines)
{
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    std::ostringstream out;
    const size_t start = lines.size() > max_lines ? lines.size() - max_lines : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        out << "  " << lines[i] << "\n";
    }
    return out.str();
}

bool wait_for_exit(pid_t pid, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

BackendProcess::BackendProcess(const Options& options) :
    log_path_(options.log_path)
{
    secrets_.client = falconer::random_bytes(kSecretBytes);
    secrets_.server = falconer::random_bytes(kSecretBytes);

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }
    const int log_fd = open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        throw std::runtime_error("cannot open backend log " + log_path_);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);

    const std::string port_arg = std::to_string(options.mavlink_port);
    std::vector<char*> argv = {
        const_cast<char*>(options.binary.c_str()),
        const_cast<char*>("--mavlink-port"),
        const_cast<char*>(port_arg.c_str()),
        nullptr,
    };
    const int spawn_result =
        posix_spawn(&pid_, options.binary.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(log_fd);
    if (spawn_result != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        throw std::runtime_error(
            "cannot start " + options.binary + ": " + std::strerror(spawn_result));
    }
    stdin_fd_ = in_pipe[1];
    // Never let a dead backend kill fctl through SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    // Secrets go over the pipe, never argv or the environment.
    const std::string line =
        falconer::to_hex(secrets_.client) + " " + falconer::to_hex(secrets_.server) + "\n";
    if (write(stdin_fd_, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
        close(out_pipe[0]);
        stop();
        throw std::runtime_error("cannot pass secrets to the backend");
    }

    // Read the single port line from stdout.
    std::string out;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStartupTimeoutMs);
    while (out.find('\n') == std::string::npos) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }
        pollfd pfd{out_pipe[0], POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            break;
        }
        char buf[256];
        const ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    close(out_pipe[0]);

    const auto pos = out.find(kListeningPrefix);
    if (pos == std::string::npos) {
        stop();
        throw std::runtime_error(
            "backend did not start; last lines of " + log_path_ + ":\n" + tail_of(log_path_, 10));
    }
    port_ = static_cast<uint16_t>(std::stoi(out.substr(pos + std::strlen(kListeningPrefix))));
}

BackendProcess::~BackendProcess()
{
    stop();
}

void BackendProcess::stop()
{
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (pid_ <= 0) {
        return;
    }
    if (!wait_for_exit(pid_, std::chrono::seconds(3))) {
        kill(pid_, SIGTERM);
        if (!wait_for_exit(pid_, std::chrono::seconds(2))) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }
    pid_ = -1;
}

} // namespace fctl
