#include "core/Graph.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_abstract_v<NodeBase>);

class Source : public NodeBase {
public:
    Source() { addOutput<int>(); }
    ~Source() override { stop(); }
    int process() override {
        if (m_stop_requested || m_next == 4) return -1;
        return output<int>()->push(m_next++) ? 0 : -1;
    }
private:
    int m_next = 1;
};

class ToString : public NodeBase {
public:
    ToString() { addInput<int>(); addOutput<std::string>(); }
    ~ToString() override { stop(); }
    int process() override {
        auto value = input<int>()->pop();
        if (!value) return -1;
        return output<std::string>()->push(std::to_string(*value)) ? 0 : -1;
    }
};

class Sink : public NodeBase {
public:
    explicit Sink(std::vector<std::string>& values) : m_values(values) { addInput<std::string>(); }
    ~Sink() override { stop(); }
    std::atomic<bool> finished{false};
    int process() override {
        auto value = input<std::string>()->pop();
        if (!value) { finished = true; return -1; }
        m_values.push_back(std::move(*value));
        return 0;
    }
private:
    std::vector<std::string>& m_values;
};

int main() {
    std::vector<std::string> values;
    Graph graph;
    auto& source = graph.addNode<Source>();
    auto& convert = graph.addNode<ToString>();
    auto& sink = graph.addNode<Sink>(values);
    graph.connect(source, convert);
    graph.connect(convert, sink);

    try {
        graph.connect(source, sink);
        throw std::logic_error("Connection accepted incompatible types");
    } catch (const std::invalid_argument&) {}
    try {
        graph.connect(sink, source);
        throw std::logic_error("Connection accepted absent ports");
    } catch (const std::out_of_range&) {}

    graph.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink.finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!sink.finished) throw std::logic_error("EOF did not reach the sink");
    graph.stop();
    if (values != std::vector<std::string>{"1", "2", "3"})
        throw std::logic_error("Pipeline lost or changed data");
}
