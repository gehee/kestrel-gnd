#include "input.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    using namespace kestrel_ui;
    const auto original = std::filesystem::current_path();
    std::string directory;
    pid_t child = -1;
    try {
        const char* temporary = std::getenv("TMPDIR");
        std::string pattern = std::string(temporary ? temporary : "/tmp") + "/kestrel-control-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
        if (!mkdtemp(name.data())) throw std::runtime_error("Cannot create test directory");
        directory = std::filesystem::absolute(name.data()).string();
        int ready[2];
        if (pipe(ready)) throw std::runtime_error("Cannot create readiness pipe");
        child = fork();
        if (child < 0) throw std::runtime_error("Cannot fork control server");
        if (child == 0) {
            close(ready[0]);
            // Bound both the handshake and server loop if setup/client fails.
            alarm(5);
            try {
                std::filesystem::current_path("/");
                Model model; Controller controller(model);
                {
                    Input input("", directory + "/control", false);
                    if (write(ready[1], "R", 1) != 1) _exit(2);
                    close(ready[1]);
                    while (!model.quit) { input.poll(controller, monotonic_us()); usleep(1000); }
                }
                _exit(model.key_count == 2 && model.menu_open ? 0 : 3);
            } catch (...) { _exit(4); }
        }
        close(ready[1]);
        char byte = 0;
        const auto n = read(ready[0], &byte, 1); close(ready[0]);
        if (n != 1 || byte != 'R') throw std::runtime_error("Control server failed to start");
        std::filesystem::current_path(directory);
        const auto response = send_control("control", "enter");
        if (response.find("open=1 stage=pick") == std::string::npos)
            throw std::runtime_error("Relative control key was not acknowledged");
        send_control("control", "quit");
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
            throw std::runtime_error("Control server failed");
        child = -1;
        if (!std::filesystem::is_empty(directory)) throw std::runtime_error("Control sockets leaked");
        std::filesystem::current_path(original); std::filesystem::remove(directory);
        std::puts("PASS: relative control paths work across different working directories");
        return 0;
    } catch (const std::exception& error) {
        if (child > 0) { kill(child, SIGTERM); waitpid(child, nullptr, 0); }
        std::filesystem::current_path(original);
        if (!directory.empty()) std::filesystem::remove_all(directory);
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
