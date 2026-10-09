
#include "Graph.hpp"
#include "InputNode.hpp"
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

int main() try {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    Graph graph;

    InputNode& input = graph.addNode<InputNode>();
    input.configure({{"filename", "input.txt"}});
    Node& node1 = graph.addNode();
    Node& node2 = graph.addNode();

    graph.connect(input, node1);
    graph.connect(node1, node2);

    graph.start();

    printf("Graph started, PID: %ld\n",
           static_cast<long>(getpid()));

    while (!stop_requested) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    graph.stop();

    printf("Graph stopped, PID: %ld\n",
           static_cast<long>(getpid()));

    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "Graph worker: %s\n", error.what());
    return 1;
}
