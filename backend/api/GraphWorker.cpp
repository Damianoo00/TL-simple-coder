
#include "JsonGraphParser.hpp"
#include <exception>
#include <cstdio>
#include <csignal>
#include <chrono>
#include <thread>
#include <unistd.h>

static volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_signal(int) {
    stop_requested = 1;
}

int main(int argc, char* argv[]) try {
    if (argc > 2) {
        std::fprintf(stderr, "Usage: %s [config.json]\n", argv[0]);
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    auto graph = JsonGraphParser::parseFile(argc == 2 ? argv[1] : "config.json");
    graph->start();

    printf("Graph started, PID: %ld\n",
           static_cast<long>(getpid()));

    while (!stop_requested && graph->poll().state == Graph::State::Running) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    if (stop_requested) graph->stop();
    const auto result = graph->wait();

    printf("Graph stopped, PID: %ld\n",
           static_cast<long>(getpid()));

    if (result.state == Graph::State::Failed) {
        std::fprintf(stderr, "Graph failed: node=%zu code=%d\n",
                     *result.failedNode, result.error);
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "Graph worker: %s\n", error.what());
    return 1;
}
