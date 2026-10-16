#include "core/Graph.hpp"

#include <atomic>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Probe : public NodeBase {
public:
    Probe() { addInput<int>(); addOutput<int>(); }
    ~Probe() override { stop(); }
    std::atomic<int> calls{0};
    int process() override { return ++calls == 4 ? Finished : Success; }
    auto incoming() { return input<int>(); }
    auto outgoing() { return output<int>(); }
};

int main() {
    {
        Probe producer, consumer;
        producer.connect(consumer);
        require(!producer.finished(), "New node already finished");
        producer.start();
        producer.join();
        require(producer.finished() && producer.result() == NodeBase::Finished,
                "Normal completion was not published");
        require(producer.status() == NodeBase::Success && producer.calls == 4,
                "Process loop did not stop at Finished");
        require(!producer.incoming()->push(1), "Completed node left input open");
        require(!consumer.incoming()->pop(), "Completed node left output open");
        producer.stop();
        producer.stop();
        require(producer.result() == NodeBase::Finished, "Repeated stop changed result");
    }
    {
        Probe producer, consumer;
        producer.connect(consumer);
        producer.outgoing()->push(42);
        producer.requestStop();
        require(!producer.incoming()->pop(), "Stop request left input open");
        require(consumer.incoming()->pop() == 42 && !consumer.incoming()->pop(),
                "Stop request did not preserve queued output before EOF");
        require(!producer.outgoing()->push(43), "Stop request left output writable");
    }
    {
        Graph graph;
        require(graph.poll().state == Graph::State::Running, "Initial graph state changed");
        graph.start();
        graph.start();
        const auto status = graph.wait();
        require(status.state == Graph::State::Completed && !status.failedNode &&
                status.error == NodeBase::Success, "Empty graph did not complete successfully");
        graph.stop();
        require(graph.poll().state == Graph::State::Completed, "Stop changed completed state");
    }
}
