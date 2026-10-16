#include "core/Graph.hpp"

#include <chrono>
#include <atomic>
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

class BareFailure : public NodeBase {
public:
    ~BareFailure() override { stop(); }
    int process() override { return InvalidDataFrame; }
};

class ThrowFailure : public NodeBase {
public:
    ~ThrowFailure() override { stop(); }
    int process() override { throw std::runtime_error("expected failure"); }
};

class UnknownFailure : public NodeBase {
public:
    ~UnknownFailure() override { stop(); }
    int process() override { throw 42; }
};

class BusySource : public NodeBase {
public:
    BusySource() { addOutput<int>(); }
    ~BusySource() override { stop(); }
    std::atomic<int> pushes{0};
    int process() override {
        if (m_stop_requested) return Finished;
        if (!output<int>()->push(1)) return Finished;
        ++pushes;
        return Success;
    }
};

class BlockedSink : public NodeBase {
public:
    BlockedSink() { addInput<int>(); setQueueCapacity(1); }
    ~BlockedSink() override { stop(); }
    int process() override {
        while (!m_stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return Finished;
    }
};

class DistantFailure : public NodeBase {
public:
    explicit DistantFailure(const std::atomic<int>& pushes) : m_pushes(pushes) {}
    ~DistantFailure() override { stop(); }
    int process() override {
        while (m_pushes.load() == 0 && !m_stop_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return InvalidConfiguration;
    }
private:
    const std::atomic<int>& m_pushes;
};

class CancellationFailure : public NodeBase {
public:
    ~CancellationFailure() override { stop(); }
    int process() override {
        while (!m_stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return InvalidPorts;
    }
};

int main() {
    std::vector<std::string> values;
    Graph graph;
    auto& source = graph.addNode<Source>();
    auto& convert = graph.addNode<ToString>();
    auto& sink = graph.addNode<Sink>(values);
    graph.connect(source, convert);
    graph.connect(convert, sink);
    convert.setQueueCapacity(1);
    sink.setQueueCapacity(1);

    try {
        graph.connect(source, sink);
        throw std::logic_error("Connection accepted incompatible types");
    } catch (const std::invalid_argument&) {}
    try {
        graph.connect(sink, source);
        throw std::logic_error("Connection accepted absent ports");
    } catch (const std::out_of_range&) {}

    graph.start();
    const auto natural = graph.wait();
    if (natural.state != Graph::State::Completed) throw std::logic_error("Natural drain did not complete");
    if (!sink.finished) throw std::logic_error("EOF did not reach the sink");
    graph.stop();
    if (graph.wait().state != Graph::State::Completed)
        throw std::logic_error("Stop changed a completed result");
    if (values != std::vector<std::string>{"1", "2", "3"})
        throw std::logic_error("Pipeline lost or changed data");

    {
        Graph failure;
        auto& node = failure.addNode<BareFailure>();
        failure.start();
        const auto result = failure.wait();
        if (result.state != Graph::State::Failed || result.failedNode != 0 ||
            result.error != NodeBase::InvalidDataFrame || node.status() != NodeBase::InvalidDataFrame)
            throw std::logic_error("Bare process error was lost");
        failure.stop();
        if (failure.wait().error != result.error) throw std::logic_error("Failure changed after stop");
    }
    {
        Graph failure;
        auto& node = failure.addNode<ThrowFailure>();
        failure.start();
        const auto result = failure.wait();
        if (result.state != Graph::State::Failed || result.error != NodeBase::MediaError ||
            node.status() != NodeBase::MediaError)
            throw std::logic_error("Exception was lost");
    }
    {
        Graph failure;
        failure.addNode<UnknownFailure>();
        failure.start();
        if (failure.wait().error != NodeBase::MediaError)
            throw std::logic_error("Unknown exception was lost");
    }
    {
        Graph failure;
        auto& busy = failure.addNode<BusySource>();
        auto& blocked = failure.addNode<BlockedSink>();
        failure.addNode<DistantFailure>(busy.pushes);
        failure.connect(busy, blocked);
        failure.start();
        const auto result = failure.wait();
        if (result.state != Graph::State::Failed || result.failedNode != 2 ||
            result.error != NodeBase::InvalidConfiguration || busy.pushes < 1)
            throw std::logic_error("Distant failure did not cancel bounded producer");
    }
    {
        Graph failure;
        auto& secondary = failure.addNode<CancellationFailure>();
        failure.addNode<BareFailure>();
        failure.start();
        const auto result = failure.wait();
        if (result.state != Graph::State::Failed || result.failedNode != 1 ||
            result.error != NodeBase::InvalidDataFrame || secondary.status() != NodeBase::InvalidPorts)
            throw std::logic_error("Cancellation error replaced original failure");
        failure.stop();
        if (failure.wait().failedNode != 1) throw std::logic_error("Failure was not stable");
    }
    {
        Graph lifecycle;
        lifecycle.addNode<BlockedSink>();
        try {
            lifecycle.wait();
            throw std::logic_error("Wait before start was accepted");
        } catch (const std::logic_error& error) {
            if (std::string(error.what()) == "Wait before start was accepted") throw;
        }
        lifecycle.start();
        lifecycle.stop();
        lifecycle.stop();
        if (lifecycle.wait().state != Graph::State::Cancelled)
            throw std::logic_error("Cancellation result changed");
    }
    {
        Graph lifecycle;
        lifecycle.addNode<BlockedSink>();
        lifecycle.stop();
        try {
            lifecycle.start();
            throw std::logic_error("Stopped graph was restarted");
        } catch (const std::logic_error& error) {
            if (std::string(error.what()) == "Stopped graph was restarted") throw;
        }
    }
}
