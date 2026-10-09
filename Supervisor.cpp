
#include "Supervisor.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

namespace {
volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_signal(int) {
    stop_requested = 1;
}
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [graph_worker_path]\n";
        return 1;
    }
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    try {
        Supervisor supervisor;
        const std::string executable = argc == 2 ? argv[1] : "./graph_worker";

        pid_t graph1 = supervisor.startGraph(executable);
        pid_t graph2 = supervisor.startGraph(executable);
        pid_t graph3 = supervisor.startGraph(executable);

        std::cout << "Started processes: "
                  << graph1 << ", "
                  << graph2 << ", "
                  << graph3 << std::endl;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        supervisor.stopAll();
    } catch (const std::exception& error) {
        std::cerr << "Supervisor: " << error.what() << '\n';
        return 1;
    }
}
