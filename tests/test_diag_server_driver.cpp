// Socket-test control process. Production server handles real loopback TCP.
#include "diag_server.h"
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

void dlog(const char*, ...) {}
bool diagnostics_flush(unsigned) { return true; }
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string directory = argv[1];
    diag_server::set_status("{\"test\":true}");
    std::string command;
    while (std::getline(std::cin, command)) {
        const auto start = Clock::now();
        if (command == "enable") diag_server::configure(true, directory, 0);
        else if (command == "disable") diag_server::configure(false, directory, 0);
        else if (command == "stop") diag_server::stop();
        else if (command == "quit") { diag_server::stop(); return 0; }
        else if (command != "state") return 3;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        std::cout << command << " " << diag_server::running() << " " << diag_server::port() << " " << elapsed << std::endl;
    }
    diag_server::stop();
}
